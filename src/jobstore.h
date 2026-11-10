#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <functional>

// One watched job. `kind` is "local", "ssh", "ec2" or "git", and `spec` holds
// what the probe for that kind needs. The fields after `stallMinutes` are the
// result of the latest check.
struct Job {
    QString id;
    QString name;
    QString kind;
    QJsonObject spec;
    qint64 created = 0;
    int stallMinutes = 20;

    QString state = "unknown"; // running, stalled, done, failed, ended or unknown
    QString detail;            // the latest log line, commit, instance state or error
    qint64 activity = 0;       // last sign of life, in epoch seconds
    qint64 checked = 0;
    QString notified;          // the state a notification last went out for

    QJsonObject toJson() const;
    static Job fromJson(const QJsonObject &object);
    // What the job watches, in one short line: a command, host:log, an instance.
    QString source() const;
};

// The jobs live in one JSON file that the window, the timer's `deadbeat check`
// and agents' `deadbeat add` all change, so every change goes through update(),
// which holds a lock file across the read and the atomic save.
class JobStore {
public:
    explicit JobStore(const QString &directory = {});

    QString directory() const { return m_directory; }
    QString path() const { return m_directory + "/jobs.json"; }
    QString logDirectory() const { return m_directory + "/logs"; }

    QList<Job> load() const;
    bool update(const std::function<void(QList<Job> &)> &change, QString *error = nullptr) const;

    static QString defaultDirectory();
    static QString newId();

private:
    bool save(const QList<Job> &jobs, QString *error) const;

    QString m_directory;
};
