#pragma once

#include <QByteArray>
#include <QCoreApplication>
#include <QString>
#include <QStringList>

#include "jobstore.h"

// How each kind of job is checked. A remote kind is one command (ssh, aws or
// gh) whose output evaluate() turns into a state; a local job is read straight
// from /proc and its own files. Everything here is pure apart from the files a
// local job reads, so the tests drive it with canned output.
class Probe {
    Q_DECLARE_TR_FUNCTIONS(Probe)

public:
    struct Command {
        QString program;
        QStringList arguments;
    };

    // The command that checks a remote job. Empty for a local job.
    static Command command(const Job &job);
    static void evaluate(Job &job, int exitCode, const QByteArray &out, const QByteArray &err, qint64 now);
    static void evaluateLocal(Job &job, qint64 now);

    // The kernel's start time of a process, so a reused PID is not mistaken
    // for the job. Empty when the process is gone.
    static QString startTicks(qint64 pid);
    // The last meaningful line of some output: after the last carriage return
    // (progress bars), without colour codes, cut to 200 characters.
    static QString lastLine(const QByteArray &text);
    static QString shellQuote(const QString &text);
    // A path for a remote shell, keeping a leading ~/ working.
    static QString remotePath(const QString &path);
    static QString errorText(const QByteArray &err, int exitCode);
};
