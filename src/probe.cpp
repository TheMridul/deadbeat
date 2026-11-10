#include "probe.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace {

qint64 parseIso(const QString &text) {
    const QDateTime time = QDateTime::fromString(text, Qt::ISODateWithMs);
    return time.isValid() ? time.toSecsSinceEpoch() : 0;
}

QByteArray tail(const QString &path, qint64 bytes = 4096) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    if (file.size() > bytes)
        file.seek(file.size() - bytes);
    return file.readAll();
}

void markActivity(Job &job, qint64 activity, qint64 now) {
    job.activity = activity;
    const bool quiet = activity > 0 && now - activity > qint64(job.stallMinutes) * 60;
    job.state = quiet ? "stalled" : "running";
}

} // namespace

QString Probe::shellQuote(const QString &text) {
    QString quoted = text;
    quoted.replace("'", "'\\''");
    return "'" + quoted + "'";
}

QString Probe::remotePath(const QString &path) {
    if (path == "~")
        return "\"$HOME\"";
    if (path.startsWith("~/"))
        return "\"$HOME\"/" + shellQuote(path.mid(2));
    return shellQuote(path);
}

QString Probe::lastLine(const QByteArray &text) {
    static const QRegularExpression ansi("\x1b\\[[0-9;?]*[ -/]*[@-~]");
    const QStringList lines = QString::fromUtf8(text).split('\n');
    for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
        QString line = it->section('\r', -1, -1, QString::SectionSkipEmpty);
        line.remove(ansi);
        line = line.trimmed();
        if (!line.isEmpty())
            return line.left(200);
    }
    return {};
}

QString Probe::errorText(const QByteArray &err, int exitCode) {
    const QString text = QString::fromUtf8(err);
    // Hosts behind SSM and every aws call fail this way once `aws login`
    // has run out; say what to do rather than echo the CLI's noise.
    if (text.contains("expired", Qt::CaseInsensitive) || text.contains("aws login")
        || text.contains("ExpiredToken"))
        return tr("AWS session ended; run aws login");
    const QString line = lastLine(err);
    return line.isEmpty() ? tr("Exited with code %1").arg(exitCode) : line;
}

QString Probe::startTicks(qint64 pid) {
    QFile stat(QStringLiteral("/proc/%1/stat").arg(pid));
    if (pid <= 0 || !stat.open(QIODevice::ReadOnly))
        return {};
    // The command name is in parentheses and may hold spaces, so count fields
    // from the last ')'. Start time is field 22, the 20th after it.
    const QByteArray line = stat.readAll();
    const QList<QByteArray> fields = line.mid(line.lastIndexOf(')') + 2).split(' ');
    return fields.size() > 19 ? QString::fromLatin1(fields.at(19)) : QString();
}

Probe::Command Probe::command(const Job &job) {
    const QJsonObject &s = job.spec;
    if (job.kind == "ssh") {
        const qint64 pid = s.value("pid").toInteger();
        const QString pidCheck = pid > 0
            ? QStringLiteral("if kill -0 %1 2>/dev/null; then echo alive; else echo dead; fi").arg(pid)
            : QStringLiteral("echo -");
        // One line each: the remote clock, the log's mtime (-1 when missing),
        // whether the process lives, then the log's last lines.
        const QString script = QStringLiteral(
            "f=%1; date +%s; if [ -e \"$f\" ]; then stat -c %Y \"$f\"; else echo -1; fi; %2; "
            "[ -e \"$f\" ] && tail -n 3 \"$f\"; true").arg(remotePath(s.value("log").toString()), pidCheck);
        // No shared connection: with ControlPersist the proxy runs in a
        // background master, and an SSM host's "session has expired" error
        // from aws never reaches us, leaving only "Connection closed".
        return {"ssh", {"-o", "BatchMode=yes", "-o", "ConnectTimeout=30", s.value("host").toString(), script}};
    }
    if (job.kind == "ec2") {
        QStringList args{"ec2", "describe-instances", "--instance-ids", s.value("instance").toString(),
                         "--output", "json", "--query",
                         "Reservations[0].Instances[0].{state:State.Name,launch:LaunchTime,"
                         "lifecycle:InstanceLifecycle,reason:StateTransitionReason}"};
        if (s.contains("region"))
            args << "--region" << s.value("region").toString();
        return {"aws", args};
    }
    if (job.kind == "git") {
        return {"gh", {"api", QStringLiteral("repos/%1/commits/%2").arg(s.value("repo").toString(),
                                                                       s.value("branch").toString()),
                       "--jq", "[.commit.committer.date, (.commit.message|split(\"\\n\")[0])]|@tsv"}};
    }
    return {};
}

void Probe::evaluate(Job &job, int exitCode, const QByteArray &out, const QByteArray &err, qint64 now) {
    job.checked = now;
    if (job.kind == "ec2" && err.contains("InvalidInstanceID.NotFound")) {
        job.state = "ended";
        job.detail = tr("Instance no longer exists");
        return;
    }
    if (exitCode != 0) {
        // A failed check says nothing about the job itself, so keep its last
        // known activity and only report why the check failed.
        job.state = "unknown";
        job.detail = errorText(err, exitCode);
        return;
    }

    if (job.kind == "ssh") {
        const QStringList lines = QString::fromUtf8(out).split('\n');
        if (lines.size() < 3) {
            job.state = "unknown";
            job.detail = tr("Unexpected reply from %1").arg(job.spec.value("host").toString());
            return;
        }
        const qint64 remoteNow = lines.at(0).trimmed().toLongLong();
        const qint64 mtime = lines.at(1).trimmed().toLongLong();
        const QString process = lines.at(2).trimmed();
        if (mtime < 0) {
            job.state = "unknown";
            job.detail = tr("Log not found on %1").arg(job.spec.value("host").toString());
            return;
        }
        job.detail = lastLine(lines.mid(3).join('\n').toUtf8());
        // Measure the log's age on the remote clock, then place it on ours,
        // so a skewed remote clock does not look like a stall.
        const qint64 activity = now - (remoteNow - mtime);
        if (process == "dead") {
            job.activity = activity;
            job.state = "ended";
            if (job.detail.isEmpty())
                job.detail = tr("Process ended");
            return;
        }
        markActivity(job, activity, now);
        return;
    }

    if (job.kind == "ec2") {
        const QJsonObject o = QJsonDocument::fromJson(out).object();
        const QString state = o.value("state").toString();
        const bool spot = o.value("lifecycle").toString() == "spot";
        job.activity = parseIso(o.value("launch").toString());
        if (state == "running" || state == "pending") {
            job.state = "running";
            job.detail = spot ? tr("Running (spot)") : tr("Running");
        } else {
            job.state = state.isEmpty() ? "unknown" : "ended";
            const QString reason = o.value("reason").toString();
            job.detail = reason.isEmpty() ? state : state + ": " + reason;
        }
        return;
    }

    if (job.kind == "git") {
        const QString line = QString::fromUtf8(out).trimmed();
        const qint64 date = parseIso(line.section('\t', 0, 0));
        if (date == 0) {
            job.state = "unknown";
            job.detail = tr("Unexpected reply from GitHub");
            return;
        }
        job.detail = line.section('\t', 1).left(200);
        markActivity(job, date, now);
    }
}

void Probe::evaluateLocal(Job &job, qint64 now) {
    job.checked = now;
    const QString log = job.spec.value("log").toString();
    const QString exitPath = job.spec.value("exit").toString();
    job.detail = lastLine(tail(log));

    QFile exitFile(exitPath);
    if (exitFile.open(QIODevice::ReadOnly)) {
        bool ok = false;
        const int code = exitFile.readAll().trimmed().toInt(&ok);
        if (ok) {
            job.activity = QFileInfo(exitPath).lastModified().toSecsSinceEpoch();
            job.state = code == 0 ? "done" : "failed";
            if (code != 0)
                job.detail = tr("Exit code %1").arg(code) + (job.detail.isEmpty() ? "" : ": " + job.detail);
            return;
        }
    }

    const QString ticks = job.spec.value("ticks").toString();
    if (!ticks.isEmpty() && startTicks(job.spec.value("pid").toInteger()) == ticks) {
        const QFileInfo info(log);
        markActivity(job, info.exists() ? info.lastModified().toSecsSinceEpoch() : job.created, now);
        return;
    }
    // The wrapper shell writes the exit code, so its absence means the job
    // was killed from outside, or the machine went down under it.
    job.state = "failed";
    job.detail = tr("Stopped without an exit code");
}
