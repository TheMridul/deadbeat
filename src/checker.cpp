#include "checker.h"

#include <QDateTime>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTimer>
#include <memory>

#include "probe.h"

void Checker::start(const QList<Job> &jobs) {
    m_results = jobs;
    m_pending = 0;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QList<int> remote;
    for (int i = 0; i < m_results.size(); ++i) {
        if (m_results[i].kind == "local")
            Probe::evaluateLocal(m_results[i], now);
        else
            remote.append(i);
    }
    if (remote.isEmpty()) {
        // Queued, so a caller can connect to finished() after start().
        QTimer::singleShot(0, this, [this] { emit finished(m_results); });
        return;
    }

    m_pending = remote.size();
    for (int index : remote) {
        const Probe::Command command = Probe::command(m_results[index]);
        auto *process = new QProcess(this);
        auto env = QProcessEnvironment::systemEnvironment();
        // gh hangs on a locked keyring; with no D-Bus it reads hosts.yml.
        // Nothing here may prompt, because nobody is there to answer.
        env.insert("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent");
        env.insert("GH_PROMPT_DISABLED", "1");
        env.insert("AWS_PAGER", "");
        process->setProcessEnvironment(env);
        process->setStandardInputFile(QProcess::nullDevice());

        auto done = std::make_shared<bool>(false);
        auto timedOut = std::make_shared<bool>(false);
        auto finish = [this, process, index, done, timedOut](int code, const QByteArray &err) {
            if (*done)
                return;
            *done = true;
            Probe::evaluate(m_results[index], *timedOut ? 124 : code, process->readAllStandardOutput(),
                            *timedOut ? QByteArray("Timed out after 60 seconds") : err,
                            QDateTime::currentSecsSinceEpoch());
            process->deleteLater();
            complete(index);
        };
        connect(process, &QProcess::finished, this, [process, finish](int code, QProcess::ExitStatus status) {
            finish(status == QProcess::NormalExit ? code : 255, process->readAllStandardError());
        });
        connect(process, &QProcess::errorOccurred, this, [command, finish](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart)
                finish(127, command.program.toUtf8() + " not found");
        });
        QTimer::singleShot(60000, process, [process, timedOut] {
            *timedOut = true;
            process->kill();
        });
        process->start(command.program, command.arguments);
    }
}

void Checker::complete(int) {
    if (--m_pending == 0)
        emit finished(m_results);
}

QList<Job> Checker::merge(QList<Job> &stored, const QList<Job> &results) {
    static const QStringList alerts{"stalled", "done", "failed", "ended"};
    QList<Job> toNotify;
    for (Job &job : stored) {
        for (const Job &result : results) {
            if (result.id != job.id)
                continue;
            job.state = result.state;
            job.detail = result.detail;
            if (result.activity > 0)
                job.activity = result.activity;
            job.checked = result.checked;
            if (alerts.contains(job.state) && job.notified != job.state) {
                job.notified = job.state;
                toNotify.append(job);
            } else if (job.state == "running") {
                // Back to running, so the next stall gets its own notification.
                job.notified.clear();
            }
            break;
        }
    }
    return toNotify;
}

bool Checker::record(const JobStore &store, const QList<Job> &results, QString *error) {
    QList<Job> toNotify;
    const bool saved = store.update([&](QList<Job> &stored) { toNotify = merge(stored, results); }, error);
    if (saved)
        for (const Job &job : toNotify)
            notify(job);
    return saved;
}

QString Notification::title(const Job &job) {
    if (job.state == "stalled")
        return tr("%1 has stalled").arg(job.name);
    if (job.state == "done")
        return tr("%1 is done").arg(job.name);
    if (job.state == "failed")
        return tr("%1 failed").arg(job.name);
    return tr("%1 has ended").arg(job.name);
}

void Checker::notify(const Job &job) {
    const bool bad = job.state == "failed" || job.state == "stalled";
    QProcess::startDetached("notify-send", {"-a", "Vigil", "-i", "vigil", "-u", bad ? "critical" : "normal",
                                            Notification::title(job), job.detail});
}
