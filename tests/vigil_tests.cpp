#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

#include "backend.h"
#include "checker.h"
#include "jobstore.h"
#include "probe.h"
#include "theme.h"
#include "translator.h"

class VigilTests : public QObject {
    Q_OBJECT

private slots:
    void storeRoundTrips()
    {
        QTemporaryDir dir;
        JobStore store(dir.path());
        Job job;
        job.id = "abc123";
        job.name = "train";
        job.kind = "ssh";
        job.spec = {{"host", "box"}, {"log", "~/run.log"}};
        QVERIFY(store.update([&](QList<Job> &jobs) { jobs.append(job); }));
        const QList<Job> loaded = store.load();
        QCOMPARE(loaded.size(), 1);
        QCOMPARE(loaded.first().name, QString("train"));
        QCOMPARE(loaded.first().source(), QString("box:~/run.log"));
        QVERIFY(QDir(store.logDirectory()).exists());
    }

    void localJobReadsItsExitCode()
    {
        QTemporaryDir dir;
        Job job = localJob(dir.path());
        write(dir.path() + "/job.log", "step 1\nstep 2\n");
        write(dir.path() + "/job.exit", "0\n");
        Probe::evaluateLocal(job, now());
        QCOMPARE(job.state, QString("done"));
        QCOMPARE(job.detail, QString("step 2"));

        write(dir.path() + "/job.exit", "3\n");
        Probe::evaluateLocal(job, now());
        QCOMPARE(job.state, QString("failed"));
        QCOMPARE(job.detail, QString("Exit code 3: step 2"));
    }

    void localJobStallsWhenItsLogGoesQuiet()
    {
        QTemporaryDir dir;
        Job job = localJob(dir.path());
        job.spec.insert("pid", QCoreApplication::applicationPid());
        job.spec.insert("ticks", Probe::startTicks(QCoreApplication::applicationPid()));
        write(dir.path() + "/job.log", "epoch 4\n");
        Probe::evaluateLocal(job, now());
        QCOMPARE(job.state, QString("running"));

        QFile log(dir.path() + "/job.log");
        QVERIFY(log.open(QIODevice::ReadWrite));
        QVERIFY(log.setFileTime(QDateTime::currentDateTime().addSecs(-30 * 60), QFileDevice::FileModificationTime));
        log.close();
        Probe::evaluateLocal(job, now());
        QCOMPARE(job.state, QString("stalled"));
    }

    void localJobWithAReusedPidCountsAsKilled()
    {
        QTemporaryDir dir;
        Job job = localJob(dir.path());
        job.spec.insert("pid", QCoreApplication::applicationPid());
        job.spec.insert("ticks", "1");
        Probe::evaluateLocal(job, now());
        QCOMPARE(job.state, QString("failed"));
        QCOMPARE(job.detail, QString("Stopped without an exit code"));
    }

    void sshReplyIsMeasuredOnTheRemoteClock()
    {
        Job job = sshJob();
        // The remote clock is far behind ours; the log is 10 minutes old there.
        Probe::evaluate(job, 0, "1000\n400\nalive\nepoch 3 loss 0.21\n", {}, 5000);
        QCOMPARE(job.state, QString("running"));
        QCOMPARE(job.activity, qint64(4400));
        QCOMPARE(job.detail, QString("epoch 3 loss 0.21"));

        job.stallMinutes = 5;
        Probe::evaluate(job, 0, "1000\n400\nalive\n", {}, 5000);
        QCOMPARE(job.state, QString("stalled"));

        Probe::evaluate(job, 0, "1000\n990\ndead\ndone\n", {}, 5000);
        QCOMPARE(job.state, QString("ended"));

        Probe::evaluate(job, 0, "1000\n-1\n-\n", {}, 5000);
        QCOMPARE(job.state, QString("unknown"));
    }

    void anExpiredAwsLoginIsNamed()
    {
        Job job = sshJob();
        job.activity = 42;
        Probe::evaluate(job, 255, {}, "kex_exchange_identification\nYour session has expired. "
                                      "Please reauthenticate using 'aws login'.\n", 5000);
        QCOMPARE(job.state, QString("unknown"));
        QCOMPARE(job.detail, QString("AWS session ended; run aws login"));
        QCOMPARE(job.activity, qint64(42));
    }

    void sshScriptRunsInARealShell()
    {
        // Run the probe's remote script locally, with HOME pointing at a temp
        // directory, so its quoting and output format are tested for real.
        QTemporaryDir dir;
        write(dir.path() + "/odd 'name'.log", "first\nloss 0.5\r loss 0.4\n");
        Job job = sshJob();
        job.spec = {{"host", "box"}, {"log", "~/odd 'name'.log"}, {"pid", QCoreApplication::applicationPid()}};
        const Probe::Command command = Probe::command(job);
        QCOMPARE(command.program, QString("ssh"));
        QProcess shell;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("HOME", dir.path());
        shell.setProcessEnvironment(env);
        shell.start("sh", {"-c", command.arguments.last()});
        QVERIFY(shell.waitForFinished());
        Probe::evaluate(job, shell.exitCode(), shell.readAllStandardOutput(), shell.readAllStandardError(), now());
        QCOMPARE(job.state, QString("running"));
        QCOMPARE(job.detail, QString("loss 0.4"));
    }

    void ec2States()
    {
        Job job;
        job.kind = "ec2";
        Probe::evaluate(job, 0, R"({"state":"running","launch":"2026-11-09T02:00:00+00:00","lifecycle":"spot","reason":""})", {}, now());
        QCOMPARE(job.state, QString("running"));
        QCOMPARE(job.detail, QString("Running (spot)"));
        QVERIFY(job.activity > 0);

        Probe::evaluate(job, 0, R"j({"state":"terminated","reason":"Service initiated (spot)"})j", {}, now());
        QCOMPARE(job.state, QString("ended"));
        QCOMPARE(job.detail, QString("terminated: Service initiated (spot)"));

        Probe::evaluate(job, 254, {}, "An error occurred (InvalidInstanceID.NotFound) when calling", now());
        QCOMPARE(job.state, QString("ended"));
    }

    void gitBranchStallsWithoutCommits()
    {
        Job job;
        job.kind = "git";
        job.stallMinutes = 180;
        const qint64 commit = QDateTime::fromString("2026-11-09T07:14:28Z", Qt::ISODate).toSecsSinceEpoch();
        Probe::evaluate(job, 0, "2026-11-09T07:14:28Z\tMerge pull request #2\n", {}, commit + 3600);
        QCOMPARE(job.state, QString("running"));
        QCOMPARE(job.detail, QString("Merge pull request #2"));
        Probe::evaluate(job, 0, "2026-11-09T07:14:28Z\tMerge pull request #2\n", {}, commit + 4 * 3600);
        QCOMPARE(job.state, QString("stalled"));
    }

    void notificationsGoOutOncePerState()
    {
        Job stored = sshJob();
        stored.id = "a";
        stored.state = "running";
        QList<Job> jobs{stored};
        Job result = stored;

        result.state = "stalled";
        QCOMPARE(Checker::merge(jobs, {result}).size(), 1);
        QCOMPARE(Checker::merge(jobs, {result}).size(), 0);

        result.state = "running";
        QCOMPARE(Checker::merge(jobs, {result}).size(), 0);
        result.state = "stalled";
        QCOMPARE(Checker::merge(jobs, {result}).size(), 1);

        result.state = "unknown";
        QCOMPARE(Checker::merge(jobs, {result}).size(), 0);
        result.state = "failed";
        QCOMPARE(Checker::merge(jobs, {result}).size(), 1);

        // A job removed while the probes ran is not brought back.
        QList<Job> empty;
        QCOMPARE(Checker::merge(empty, {result}).size(), 0);
        QVERIFY(empty.isEmpty());
    }

    void lastLineCleansProgressOutput()
    {
        QCOMPARE(Probe::lastLine("a\n\x1b[32m 50%\r 100% done\x1b[0m\n\n"), QString("100% done"));
        QCOMPARE(Probe::lastLine(""), QString());
    }

    void remotePathsAreQuoted()
    {
        QCOMPARE(Probe::remotePath("~/a b'c"), QString("\"$HOME\"/'a b'\\''c'"));
        QCOMPARE(Probe::remotePath("/var/log/x"), QString("'/var/log/x'"));
    }

    void removalsUndoAndRedo()
    {
        QTemporaryDir dir;
        JobStore store(dir.path());
        Job job = sshJob();
        job.id = "a";
        job.name = "export";
        QVERIFY(store.update([&](QList<Job> &jobs) { jobs.append(job); }));

        Backend backend(store, nullptr, false);
        QCOMPARE(backend.jobs().size(), 1);
        backend.remove("a");
        QCOMPARE(backend.jobs().size(), 0);
        QVERIFY(backend.canUndo());
        backend.undo();
        QCOMPARE(backend.jobs().size(), 1);
        QVERIFY(backend.canRedo());
        backend.redo();
        QCOMPARE(backend.jobs().size(), 0);
        QCOMPARE(store.load().size(), 0);
    }

    void awsBadgeData()
    {
        const QVariantMap aws = Backend::parseAws(R"({"active": true, "seconds_left": 600, "ends_at": 1})");
        QCOMPARE(aws.value("available").toBool(), true);
        QCOMPARE(aws.value("secondsLeft").toLongLong(), 600);
        QVERIFY(Backend::parseAws("").isEmpty());
    }

    void themeFallsBack()
    {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QString("black"));
    }

    void themeReadsAccent()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        write(dir.path() + "/theme/colors.toml", "accent = \"#112233\"\n");
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#112233"));
        QCOMPARE(theme.accentForeground(), QString("white"));
    }

    void themeFollowsHyprlandRounding()
    {
        QTemporaryDir dir;
        const QString response = dir.filePath("response.json"), command = dir.filePath("hyprctl");
        write(command, "#!/bin/sh\ncat '" + response.toUtf8() + "'\n");
        QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        write(response, "{\"int\":0}");
        Theme theme(dir.path(), nullptr, command);
        QCOMPARE(theme.radius(), 0);
        write(response, "{\"int\":6}");
        QTRY_COMPARE(theme.radius(), 6);
    }

    void translatorFallsBackToEnglish()
    {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Done").isNull());
    }

    void shippedCatalogsTranslate()
    {
        QTemporaryDir dir;
        write(dir.path() + "/language", "hi\n");
        Translator translator(dir.path() + "/language");
        QCOMPARE(translator.translate("Main", "Stalled"), QString("अटका हुआ"));
        QCOMPARE(translator.translate("Main", "%n h ago", nullptr, 2), QString("%n घंटे पहले"));
    }

    void translatorPicksPluralForms()
    {
        QTemporaryDir dir;
        write(dir.path() + "/language", "ru\n");
        write(dir.path() + "/en.json", R"({"%n clips": {"one": "%n clip", "other": "%n clips"}})");
        write(dir.path() + "/ru.json", R"({"%n clips": {"one": "%n клип", "few": "%n клипа", "many": "%n клипов", "other": "%n клипа"}})");
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 21), QString("%n клип"));
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 25), QString("%n клипов"));
    }

private:
    static qint64 now() { return QDateTime::currentSecsSinceEpoch(); }

    static Job localJob(const QString &dir)
    {
        Job job;
        job.kind = "local";
        job.stallMinutes = 20;
        job.spec = {{"log", dir + "/job.log"}, {"exit", dir + "/job.exit"}, {"pid", 0}, {"ticks", ""}};
        return job;
    }

    static Job sshJob()
    {
        Job job;
        job.kind = "ssh";
        job.stallMinutes = 20;
        job.spec = {{"host", "box"}, {"log", "~/run.log"}};
        return job;
    }

    static void write(const QString &path, const QByteArray &contents)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(contents);
    }
};

QTEST_MAIN(VigilTests)
#include "vigil_tests.moc"
