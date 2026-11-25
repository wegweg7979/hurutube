#pragma once

// Starting a helper process that must not be running twice.
//
// A state check alone is not enough. Measured in one real session:
// `QProcess::start: Process is already running` six times in eight seconds. Qt's
// message does not name the process, and `start()` in that state does NOTHING --
// so the request is dropped silently, which is a resolve that never happens (the
// video falls back to mpv's own, slower extraction) or a channel listing that
// never arrives. So: stop the previous run, wait for it, and when it is still
// winding down, DEFER the new start to the event loop instead of losing it. Every
// refusal is warned about with a name, so the next occurrence in the journal says
// which process was busy instead of leaving a bare Qt line.

#include <QDebug>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

namespace Restart {

// Stops `process` if it is running and starts it again with the given arguments.
// `who` is only for the warning. `waitMs` is how long to wait for a forced kill to
// be reaped (tests pass 0 to reach the deferred path deliberately).
inline void process(QProcess &process, const QString &program, const QStringList &arguments,
                    const QString &who, int waitMs = 1500)
{
    if (process.state() != QProcess::NotRunning) {
        // Ask first -- yt-dlp and mpv both exit cleanly on SIGTERM -- then insist.
        process.terminate();
        if (!process.waitForFinished(400)) {
            process.kill();
            process.waitForFinished(waitMs);
        }
    }
    if (process.state() == QProcess::NotRunning) {
        process.start(program, arguments);
        return;
    }

    qWarning().noquote() << "hurutube:" << who
                         << "was still running after a kill; its restart is deferred"
                            " (so the request is not dropped)";
    QObject::connect(&process, &QProcess::finished, &process,
                     [&process, program, arguments] {
                         if (process.state() == QProcess::NotRunning)
                             process.start(program, arguments);
                     },
                     Qt::SingleShotConnection);
}

}  // namespace Restart
