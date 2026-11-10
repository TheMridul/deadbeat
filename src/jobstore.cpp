#include "jobstore.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QRandomGenerator>
#include <QSaveFile>

QJsonObject Job::toJson() const {
    return {
        {"id", id}, {"name", name}, {"kind", kind}, {"spec", spec},
        {"created", created}, {"stallMinutes", stallMinutes},
        {"state", state}, {"detail", detail}, {"activity", activity},
        {"checked", checked}, {"notified", notified},
    };
}

Job Job::fromJson(const QJsonObject &o) {
    Job job;
    job.id = o.value("id").toString();
    job.name = o.value("name").toString();
    job.kind = o.value("kind").toString();
    job.spec = o.value("spec").toObject();
    job.created = o.value("created").toInteger();
    job.stallMinutes = o.value("stallMinutes").toInt(20);
    job.state = o.value("state").toString("unknown");
    job.detail = o.value("detail").toString();
    job.activity = o.value("activity").toInteger();
    job.checked = o.value("checked").toInteger();
    job.notified = o.value("notified").toString();
    return job;
}

QString Job::source() const {
    if (kind == "local")
        return spec.value("command").toString();
    if (kind == "ssh")
        return spec.value("host").toString() + ":" + spec.value("log").toString();
    if (kind == "ec2")
        return spec.value("instance").toString();
    if (kind == "git")
        return spec.value("repo").toString() + "@" + spec.value("branch").toString();
    return kind;
}

JobStore::JobStore(const QString &directory)
    : m_directory(directory.isEmpty() ? defaultDirectory() : directory) {}

QString JobStore::defaultDirectory() {
    // VIGIL_STATE_DIR lets tests and the screenshot harness use a scratch store.
    const QString override = qEnvironmentVariable("VIGIL_STATE_DIR");
    if (!override.isEmpty())
        return override;
    const QString state = qEnvironmentVariable("XDG_STATE_HOME", QDir::homePath() + "/.local/state");
    return state + "/vigil";
}

QString JobStore::newId() {
    return QString::number(QRandomGenerator::global()->bounded(0x100000, 0xFFFFFF), 16);
}

QList<Job> JobStore::load() const {
    QList<Job> jobs;
    QFile file(path());
    if (!file.open(QIODevice::ReadOnly))
        return jobs;
    const QJsonArray array = QJsonDocument::fromJson(file.readAll()).object().value("jobs").toArray();
    for (const auto &value : array)
        jobs.append(Job::fromJson(value.toObject()));
    return jobs;
}

bool JobStore::save(const QList<Job> &jobs, QString *error) const {
    QJsonArray array;
    for (const auto &job : jobs)
        array.append(job.toJson());
    // QSaveFile writes beside the target and renames, so a crash mid-write
    // never leaves a half-written jobs.json.
    QSaveFile file(path());
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    file.write(QJsonDocument(QJsonObject{{"jobs", array}}).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

bool JobStore::update(const std::function<void(QList<Job> &)> &change, QString *error) const {
    if (!QDir().mkpath(m_directory) || !QDir().mkpath(logDirectory())) {
        if (error)
            *error = QStringLiteral("cannot create %1").arg(m_directory);
        return false;
    }
    QLockFile lock(m_directory + "/jobs.lock");
    lock.setStaleLockTime(15000);
    if (!lock.tryLock(10000)) {
        if (error)
            *error = QStringLiteral("another vigil is holding %1/jobs.lock").arg(m_directory);
        return false;
    }
    QList<Job> jobs = load();
    change(jobs);
    return save(jobs, error);
}
