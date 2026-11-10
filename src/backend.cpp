#include "backend.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QUrl>

#include "probe.h"

Backend::Backend(const JobStore &store, QObject *parent, bool autoCheck)
    : QObject(parent), m_store(store) {
    QDir().mkpath(m_store.directory());
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(100);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_debounce.start(); });
    connect(&m_debounce, &QTimer::timeout, this, &Backend::reload);

    connect(&m_checker, &Checker::finished, this, [this](const QList<Job> &results) {
        QString error;
        if (!Checker::record(m_store, results, &error))
            say(tr("Could not save the check: %1").arg(error));
        m_lastChecked = QDateTime::currentSecsSinceEpoch();
        emit checkingChanged();
        reload();
    });

    connect(&m_awsProcess, &QProcess::finished, this, [this] {
        m_aws = parseAws(m_awsProcess.readAllStandardOutput());
        emit awsChanged();
    });

    // Remote checks cost an ssh or API round trip, so once a minute is enough
    // for jobs that run for hours; the timer's `deadbeat check` covers the time
    // the window is closed.
    m_poll.setInterval(60000);
    connect(&m_poll, &QTimer::timeout, this, &Backend::check);
    reload();
    if (autoCheck) {
        m_poll.start();
        QTimer::singleShot(0, this, &Backend::check);
    }
}

QVariantMap Backend::parseAws(const QByteArray &json) {
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    if (o.isEmpty())
        return {};
    return {{"available", true},
            {"active", o.value("active").toBool()},
            {"secondsLeft", o.value("seconds_left").toInteger()},
            {"endsAt", o.value("ends_at").toInteger()},
            {"error", o.value("error").toString()}};
}

QVariantList Backend::jobs() const {
    QVariantList list;
    for (const Job &job : m_jobs)
        list.append(QVariantMap{{"id", job.id}, {"name", job.name}, {"kind", job.kind},
                                {"state", job.state}, {"detail", job.detail}, {"source", job.source()},
                                {"activity", job.activity}, {"checked", job.checked},
                                {"created", job.created}, {"stallMinutes", job.stallMinutes}});
    return list;
}

void Backend::reload() {
    m_jobs = m_store.load();
    for (const Job &job : m_jobs)
        m_lastChecked = qMax(m_lastChecked, job.checked);
    watch();
    emit jobsChanged();
    emit checkingChanged();
}

void Backend::watch() {
    // Saving replaces jobs.json, which drops it from the watch, so re-arm on
    // every change and keep the directory watched to see it come back.
    const auto paths = m_watcher.files() + m_watcher.directories();
    if (!paths.isEmpty())
        m_watcher.removePaths(paths);
    m_watcher.addPath(m_store.directory());
    if (QFileInfo::exists(m_store.path()))
        m_watcher.addPath(m_store.path());
}

void Backend::check() {
    if (m_checker.busy())
        return;
    refreshAws();
    m_checker.start(m_store.load());
    emit checkingChanged();
}

void Backend::refreshAws() {
    const QString awsSession = QStandardPaths::findExecutable("aws-session");
    if (!awsSession.isEmpty() && m_awsProcess.state() == QProcess::NotRunning)
        m_awsProcess.start(awsSession, {"--json"});
}

bool Backend::removeNow(const QString &id, Job *removed) {
    bool found = false;
    QString error;
    const bool saved = m_store.update([&](QList<Job> &jobs) {
        for (int i = 0; i < jobs.size(); ++i) {
            if (jobs[i].id == id) {
                *removed = jobs.takeAt(i);
                found = true;
                return;
            }
        }
    }, &error);
    if (!saved)
        say(tr("Could not save: %1").arg(error));
    reload();
    return saved && found;
}

void Backend::remove(const QString &id) {
    Job removed;
    if (!removeNow(id, &removed))
        return;
    m_undo.append(removed);
    m_redo.clear();
    emit historyChanged();
    say(tr("Stopped watching %1. Ctrl+Z brings it back.").arg(removed.name));
}

void Backend::undo() {
    if (m_undo.isEmpty())
        return;
    const Job job = m_undo.takeLast();
    QString error;
    if (!m_store.update([&](QList<Job> &jobs) { jobs.append(job); }, &error)) {
        m_undo.append(job);
        say(tr("Could not save: %1").arg(error));
        return;
    }
    m_redo.append(job);
    emit historyChanged();
    reload();
}

void Backend::redo() {
    if (m_redo.isEmpty())
        return;
    Job removed;
    if (removeNow(m_redo.takeLast().id, &removed))
        m_undo.append(removed);
    emit historyChanged();
}

void Backend::follow(const QString &id) {
    const auto it = std::find_if(m_jobs.cbegin(), m_jobs.cend(), [&](const Job &j) { return j.id == id; });
    if (it == m_jobs.cend())
        return;
    const Job &job = *it;
    QString program;
    QStringList args;
    if (job.kind == "local") {
        program = "xdg-terminal-exec";
        args = {"less", "+F", job.spec.value("log").toString()};
    } else if (job.kind == "ssh") {
        program = "xdg-terminal-exec";
        args = {"ssh", "-t", job.spec.value("host").toString(),
                "f=" + Probe::remotePath(job.spec.value("log").toString()) + "; tail -n 50 -f \"$f\""};
    } else if (job.kind == "git") {
        program = "xdg-open";
        args = {QStringLiteral("https://github.com/%1/commits/%2")
                    .arg(job.spec.value("repo").toString(), job.spec.value("branch").toString())};
    } else if (job.kind == "ec2") {
        program = "xdg-open";
        const QString region = job.spec.value("region").toString();
        args = {QStringLiteral("https://console.aws.amazon.com/ec2/home%1#InstanceDetails:instanceId=%2")
                    .arg(region.isEmpty() ? QString() : "?region=" + region, job.spec.value("instance").toString())};
    }
    if (QStandardPaths::findExecutable(program).isEmpty()) {
        say(tr("%1 is not installed, so there is nothing to open this with").arg(program));
        return;
    }
    if (!QProcess::startDetached(program, args))
        say(tr("Could not start %1").arg(program));
}

void Backend::say(const QString &text) {
    m_message = text;
    emit messageChanged();
}

void Backend::clearMessage() {
    if (m_message.isEmpty())
        return;
    m_message.clear();
    emit messageChanged();
}
