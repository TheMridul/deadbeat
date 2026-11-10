// Command-line shape follows Hype's src/cli.cpp (MIT, David Heinemeier Hansson).
#include "cli.h"

#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QHash>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTextStream>

#include "checker.h"
#include "jobstore.h"
#include "probe.h"

namespace {

QTextStream out(stdout);
QTextStream err(stderr);

const char *usage =
    "deadbeat watches long jobs and tells you when they stall, finish or fail.\n"
    "\n"
    "  deadbeat [open]                          the window\n"
    "  deadbeat run --name N [--stall MIN] -- COMMAND...\n"
    "                                        start COMMAND in the background and watch it\n"
    "  deadbeat add ssh --name N --host H --log PATH [--pid PID] [--stall MIN]\n"
    "                                        watch a log (and process) on an ssh host\n"
    "  deadbeat add ec2 --name N --instance ID [--region R]\n"
    "                                        watch an EC2 instance until it stops\n"
    "  deadbeat add git --name N --repo OWNER/REPO [--branch B] [--stall MIN]\n"
    "                                        watch a branch that a loop pushes to\n"
    "  deadbeat list                            the jobs as last checked\n"
    "  deadbeat check                           check every job now and notify\n"
    "  deadbeat remove ID                       stop watching a job (it keeps running)\n"
    "  deadbeat skill install                   install the agent skill\n"
    "\n"
    "Every command takes --json. A job is stalled when its log, or the branch,\n"
    "has been quiet longer than --stall minutes (20, or 180 for git).\n";

struct Options {
    QStringList positional;
    QHash<QString, QString> values;
    QStringList command; // everything after --
    bool json = false;
    QString error;
};

Options parse(const QStringList &args) {
    static const QStringList valued{"--name", "--stall", "--host", "--log", "--pid",
                                    "--instance", "--region", "--repo", "--branch"};
    Options o;
    for (int i = 0; i < args.size(); ++i) {
        const QString &arg = args.at(i);
        if (arg == "--") {
            o.command = args.mid(i + 1);
            break;
        }
        if (arg == "--json") {
            o.json = true;
        } else if (valued.contains(arg)) {
            if (i + 1 >= args.size()) {
                o.error = arg + " needs a value";
                return o;
            }
            o.values.insert(arg.mid(2), args.at(++i));
        } else if (arg.startsWith("--")) {
            o.error = "unknown option " + arg;
            return o;
        } else {
            o.positional.append(arg);
        }
    }
    return o;
}

int fail(const QString &message) {
    err << "deadbeat: " << message << "\n";
    err.flush();
    return 1;
}

QString displayCommand(const QStringList &command) {
    static const QRegularExpression plain("^[A-Za-z0-9_./:=@%+,-]+$");
    QStringList parts;
    for (const QString &part : command)
        parts.append(plain.match(part).hasMatch() ? part : Probe::shellQuote(part));
    return parts.join(' ');
}

void printJobs(const QList<Job> &jobs, bool json) {
    if (json) {
        QJsonArray array;
        for (const Job &job : jobs) {
            QJsonObject o = job.toJson();
            o.insert("source", job.source());
            array.append(o);
        }
        out << QJsonDocument(array).toJson(QJsonDocument::Compact) << "\n";
        return;
    }
    if (jobs.isEmpty())
        out << "No jobs. Add one with deadbeat run or deadbeat add.\n";
    for (const Job &job : jobs)
        out << job.id << "  " << job.state.leftJustified(8) << job.name << "  (" << job.source() << ")"
            << (job.detail.isEmpty() ? QString() : "\n        " + job.detail) << "\n";
}

int addJob(Job job, const Options &o) {
    const JobStore store;
    job.id = JobStore::newId();
    job.name = o.values.value("name");
    job.created = QDateTime::currentSecsSinceEpoch();
    if (job.name.isEmpty())
        return fail("--name is required");
    if (o.values.contains("stall")) {
        bool ok = false;
        job.stallMinutes = o.values.value("stall").toInt(&ok);
        if (!ok || job.stallMinutes < 1)
            return fail("--stall takes a number of minutes");
    }
    QString error;
    if (!store.update([&](QList<Job> &jobs) { jobs.append(job); }, &error))
        return fail(error);
    if (o.json)
        out << QJsonDocument(job.toJson()).toJson(QJsonDocument::Compact) << "\n";
    else
        out << "Watching " << job.name << " as " << job.id << "\n";
    return 0;
}

int runCommand(const Options &o) {
    if (o.command.isEmpty())
        return fail("nothing to run; put the command after --");
    const JobStore store;
    QString error;
    if (!store.update([](QList<Job> &) {}, &error)) // creates the directories
        return fail(error);

    Job job;
    job.kind = "local";
    job.state = "running";
    job.stallMinutes = 20;
    const QString id = JobStore::newId();
    const QString log = store.logDirectory() + "/" + id + ".log";
    const QString exitFile = store.logDirectory() + "/" + id + ".exit";

    // A small shell owns the command so its exit code lands in a file, which
    // is how a later `deadbeat check` tells done from failed. Its own session
    // keeps it alive when the terminal or agent that started it goes away.
    QProcess process;
    process.setProgram("sh");
    process.setArguments(QStringList{"-c", "log=$1 exitf=$2; shift 2; \"$@\" >\"$log\" 2>&1 </dev/null; "
                                           "echo $? >\"$exitf\"", "deadbeat-run", log, exitFile} + o.command);
    process.setWorkingDirectory(QDir::currentPath());
    process.setStandardInputFile(QProcess::nullDevice());
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
    process.setUnixProcessParameters(QProcess::UnixProcessFlag::CreateNewSession);
    qint64 pid = 0;
    if (!process.startDetached(&pid))
        return fail("could not start sh: " + process.errorString());

    job.spec = {{"command", displayCommand(o.command)}, {"cwd", QDir::currentPath()}, {"pid", pid},
                {"ticks", Probe::startTicks(pid)}, {"log", log}, {"exit", exitFile}};
    job.activity = QDateTime::currentSecsSinceEpoch();
    // The id names the log, so keep the one picked above.
    job.id = id;
    job.name = o.values.value("name");
    job.created = job.activity;
    if (job.name.isEmpty())
        job.name = o.command.first();
    if (o.values.contains("stall"))
        job.stallMinutes = qMax(1, o.values.value("stall").toInt());
    if (!store.update([&](QList<Job> &jobs) { jobs.append(job); }, &error))
        return fail(error);
    if (o.json)
        out << QJsonDocument(job.toJson()).toJson(QJsonDocument::Compact) << "\n";
    else
        out << "Started " << job.name << " as " << job.id << " (pid " << pid << ")\nLog: " << log << "\n";
    return 0;
}

int check(const Options &o) {
    const JobStore store;
    const QList<Job> jobs = store.load();
    Checker checker;
    QEventLoop loop;
    QList<Job> results;
    QObject::connect(&checker, &Checker::finished, &loop, [&](const QList<Job> &r) {
        results = r;
        loop.quit();
    });
    checker.start(jobs);
    loop.exec();
    QString error;
    if (!Checker::record(store, results, &error))
        return fail(error);
    printJobs(store.load(), o.json);
    return 0;
}

int remove(const Options &o) {
    if (o.positional.size() < 2)
        return fail("which job? deadbeat remove ID");
    const QString id = o.positional.at(1);
    bool found = false;
    QString error;
    const bool saved = JobStore().update([&](QList<Job> &jobs) {
        found = jobs.removeIf([&](const Job &job) { return job.id == id; }) > 0;
    }, &error);
    if (!saved)
        return fail(error);
    if (!found)
        return fail("no job " + id);
    if (o.json)
        out << QJsonDocument(QJsonObject{{"removed", id}}).toJson(QJsonDocument::Compact) << "\n";
    else
        out << "Stopped watching " << id << "\n";
    return 0;
}

int installSkill(const Options &o) {
    const QString root = qEnvironmentVariable("DEADBEAT_SKILL_DIR", QDir::homePath() + "/.agents/skills");
    const QString path = root + "/deadbeat/SKILL.md";
    QFile source(":/skill/SKILL.md");
    if (!source.open(QIODevice::ReadOnly) || !QDir().mkpath(root + "/deadbeat"))
        return fail("cannot write " + path);
    QSaveFile target(path);
    if (!target.open(QIODevice::WriteOnly) || target.write(source.readAll()) < 0 || !target.commit())
        return fail("cannot write " + path + ": " + target.errorString());
    if (o.json)
        out << QJsonDocument(QJsonObject{{"installed", path}}).toJson(QJsonDocument::Compact) << "\n";
    else
        out << "Installed " << path << "\n";
    return 0;
}

} // namespace

bool Cli::wantsWindow(const QStringList &args) {
    return args.isEmpty() || args.first() == "open";
}

int Cli::run(const QStringList &args) {
    const Options o = parse(args);
    if (!o.error.isEmpty())
        return fail(o.error);
    const QString command = o.positional.value(0);
    if (command == "help" || args.contains("--help") || args.contains("-h")) {
        out << usage;
        return 0;
    }
    if (command.isEmpty()) {
        err << usage;
        return 1;
    }
    if (command == "run")
        return runCommand(o);
    if (command == "add") {
        const QString kind = o.positional.value(1);
        Job job;
        job.kind = kind;
        const auto need = [&](const QString &key) { return o.values.value(key); };
        if (kind == "ssh") {
            if (need("host").isEmpty() || need("log").isEmpty())
                return fail("add ssh needs --host and --log");
            job.spec = {{"host", need("host")}, {"log", need("log")}};
            if (o.values.contains("pid"))
                job.spec.insert("pid", need("pid").toLongLong());
        } else if (kind == "ec2") {
            if (need("instance").isEmpty())
                return fail("add ec2 needs --instance");
            job.spec = {{"instance", need("instance")}};
            if (o.values.contains("region"))
                job.spec.insert("region", need("region"));
        } else if (kind == "git") {
            if (!need("repo").contains('/'))
                return fail("add git needs --repo OWNER/REPO");
            job.spec = {{"repo", need("repo")}, {"branch", o.values.value("branch", "main")}};
            job.stallMinutes = 180;
        } else {
            return fail("add takes ssh, ec2 or git");
        }
        return addJob(job, o);
    }
    if (command == "list") {
        printJobs(JobStore().load(), o.json);
        return 0;
    }
    if (command == "check")
        return check(o);
    if (command == "remove")
        return remove(o);
    if (command == "skill" && o.positional.value(1) == "install")
        return installSkill(o);
    if (command == "--version" || command == "version") {
        out << "deadbeat 0.1.0\n";
        return 0;
    }
    return fail("unknown command " + command + "; see deadbeat help");
}
