#pragma once

#include <QFileSystemWatcher>
#include <QList>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include "checker.h"
#include "jobstore.h"

// The window's view of the job store. It reloads whenever the file changes
// (an agent ran `deadbeat add`), checks every job each minute while open, and
// keeps removals undoable.
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList jobs READ jobs NOTIFY jobsChanged)
    Q_PROPERTY(bool checking READ checking NOTIFY checkingChanged)
    Q_PROPERTY(qint64 lastChecked READ lastChecked NOTIFY checkingChanged)
    Q_PROPERTY(QVariantMap aws READ aws NOTIFY awsChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)

public:
    explicit Backend(const JobStore &store = JobStore(), QObject *parent = nullptr, bool autoCheck = true);

    QVariantList jobs() const;
    bool checking() const { return m_checker.busy(); }
    qint64 lastChecked() const { return m_lastChecked; }
    QVariantMap aws() const { return m_aws; }
    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }
    QString message() const { return m_message; }

    Q_INVOKABLE void check();
    // Re-reads `aws-session --json` for the badge; check() does this too.
    Q_INVOKABLE void refreshAws();
    Q_INVOKABLE void remove(const QString &id);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    // Opens the job's log in a terminal, or its page in the browser.
    Q_INVOKABLE void follow(const QString &id);
    Q_INVOKABLE void clearMessage();

    // The AWS banner's data from `aws-session --json`, empty when the tool
    // is not installed. Public for the tests.
    static QVariantMap parseAws(const QByteArray &json);

signals:
    void jobsChanged();
    void checkingChanged();
    void awsChanged();
    void historyChanged();
    void messageChanged();

private:
    void reload();
    void watch();
    void say(const QString &text);
    bool removeNow(const QString &id, Job *removed);

    JobStore m_store;
    QList<Job> m_jobs;
    Checker m_checker;
    qint64 m_lastChecked = 0;
    QVariantMap m_aws;
    QProcess m_awsProcess;
    QList<Job> m_undo, m_redo;
    QString m_message;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce, m_poll;
};
