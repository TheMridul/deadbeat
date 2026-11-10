#pragma once

#include <QCoreApplication>
#include <QList>
#include <QObject>
#include <functional>

#include "jobstore.h"

// Checks a set of jobs at once: every remote probe runs in parallel with a
// 60-second limit, and finished() hands back the jobs with new states.
class Checker : public QObject {
    Q_OBJECT

public:
    explicit Checker(QObject *parent = nullptr) : QObject(parent) {}

    bool busy() const { return m_pending > 0; }
    void start(const QList<Job> &jobs);

    // Copies the results onto the stored jobs (which may have changed while
    // the probes ran) and returns the jobs that need a notification: the ones
    // that just stalled, finished, failed or ended.
    static QList<Job> merge(QList<Job> &stored, const QList<Job> &results);
    // Merges the results into the store under its lock, then sends the
    // notifications. Both the window and `deadbeat check` finish a check here,
    // so a job is announced once even when both are running.
    static bool record(const JobStore &store, const QList<Job> &results, QString *error = nullptr);
    static void notify(const Job &job);

signals:
    void finished(const QList<Job> &results);

private:
    void complete(int index);

    QList<Job> m_results;
    int m_pending = 0;
};

class Notification {
    Q_DECLARE_TR_FUNCTIONS(Notification)
public:
    static QString title(const Job &job);
};
