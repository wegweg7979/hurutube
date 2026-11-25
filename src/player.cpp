#include "player.h"

#include "restartprocess.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QStandardPaths>

#include <csignal>
#include <unistd.h>

namespace {

// Observed property ids. mpv answers `observe_property` with the id we chose, so
// these numbers are the wire protocol between us and the engine.
enum PropertyId {
    IdTimePos = 1,
    IdDuration = 2,
    IdPause = 3,
    IdVolume = 4,
    IdMediaTitle = 5,
    IdIdleActive = 6,
    IdHwdec = 7,
    IdVideoParams = 8,
    IdVideoFormat = 9,
};

// A pre-resolved URL is good for hours, but the safe assumption is minutes:
// googlevideo ties it to the client IP and a timestamp, and a long video watched
// from a laptop that changed networks would otherwise fail on resume.
constexpr qint64 kResolveTtlSeconds = 90 * 60;

}  // namespace

Player::Player(QObject *parent, const QString &mpvBinary, const QString &socketPath)
    : QObject(parent),
      m_mpv(mpvBinary.isEmpty() ? QStringLiteral("mpv") : mpvBinary),
      m_socketPath(socketPath.isEmpty()
                       ? QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation)
                             + QStringLiteral("/hurutube-mpv.sock")
                       : socketPath)
{
    connect(&m_socket, &QLocalSocket::connected, this, [this] {
        m_buffer.clear();
        m_observed = false;
        observe(IdTimePos, QStringLiteral("time-pos"));
        observe(IdDuration, QStringLiteral("duration"));
        observe(IdPause, QStringLiteral("pause"));
        observe(IdVolume, QStringLiteral("volume"));
        observe(IdMediaTitle, QStringLiteral("media-title"));
        observe(IdIdleActive, QStringLiteral("idle-active"));
        observe(IdHwdec, QStringLiteral("hwdec-current"));
        observe(IdVideoParams, QStringLiteral("video-params"));
        // "video-format" is the source codec ("av1", "vp9", "h264") and stays
        // readable even while decoding through VA-API, where video-params reports
        // the pixel format as "vaapi" instead.
        observe(IdVideoFormat, QStringLiteral("video-format"));
        // The arrow keys are the app's ten seconds, but while watching, the video
        // window has focus and mpv's own built-in bindings (five seconds) are the
        // ones that fire. `keybind` replaces a default rather than joining it --
        // measured with `keypress` against a throwaway engine: one press moved
        // exactly 10.3 s, not 15 -- so this makes both windows agree with the
        // transport buttons and the help. (mpv 0.41 has no --left/--right option;
        // `get_property input-bindings` still lists the shadowed default, which is
        // why the property is not the proof -- the seek is.)
        send({QStringLiteral("keybind"), QStringLiteral("LEFT"), QStringLiteral("seek -10")});
        send({QStringLiteral("keybind"), QStringLiteral("RIGHT"), QStringLiteral("seek 10")});
        m_observed = true;
        m_ready = true;
        emit stateChanged();
        flushPending();
    });
    connect(&m_socket, &QLocalSocket::readyRead, this, [this] {
        m_buffer.append(m_socket.readAll());
        int newline;
        while ((newline = m_buffer.indexOf('\n')) >= 0) {
            const QByteArray line = m_buffer.left(newline);
            m_buffer.remove(0, newline + 1);
            if (!line.trimmed().isEmpty())
                handleLine(line);
        }
    });
    connect(&m_socket, &QLocalSocket::disconnected, this, [this] {
        m_playing = false;
        m_paused = false;
        m_ready = false;
        emit stateChanged();
    });
    connect(&m_socket, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError error) {
        if (error == QLocalSocket::ServerNotFoundError || error == QLocalSocket::ConnectionRefusedError) {
            // The socket is not up yet (or mpv just restarted); retry quietly.
            m_reconnect.start(300);
            return;
        }
        setError(QStringLiteral("mpv socket: %1").arg(m_socket.errorString()));
    });

    m_reconnect.setSingleShot(true);
    connect(&m_reconnect, &QTimer::timeout, this, &Player::connectSocket);

    // mpv's stderr is kept (rarely more than a line), because a rejected option
    // or a missing device makes it exit immediately and silently otherwise.
    m_process.setProcessChannelMode(QProcess::SeparateChannels);
    connect(&m_process, &QProcess::readyReadStandardError, this, [this] {
        m_engineError = m_process.readAllStandardError().trimmed();
    });
    connect(&m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            &Player::handleEngineExit);

    // The background resolver: one at a time, always for the newest request.
    connect(&m_resolve, &QProcess::readyReadStandardOutput, this, [this] {
        m_resolveOutput += m_resolve.readAllStandardOutput();
    });
    connect(&m_resolve, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus status) {
                const QByteArray output = m_resolveOutput;
                m_resolveOutput.clear();
                const QString url = m_resolving;
                m_resolving.clear();
                if (url.isEmpty())
                    return;
                const QStringList lines = QString::fromUtf8(output).split(QLatin1Char('\n'),
                                                                         Qt::SkipEmptyParts);
                if (status != QProcess::NormalExit || code != 0 || lines.isEmpty()) {
                    // One retry, WITH the cookies, and only after an anonymous
                    // attempt has failed: this is the shape an age-gated or
                    // members-only video fails in. It cannot be the other way
                    // round -- signing in is also what makes YouTube serve no
                    // formats at all to the clients yt-dlp uses, so cookies as
                    // the default would play nothing (resolveArguments()).
                    if (!m_cookieRetryTried && hasCookieSource()) {
                        m_cookieRetryTried = true;
                        m_resolving = url;
                        Restart::process(m_resolve, QStringLiteral("yt-dlp"),
                                         resolveArguments(url, true),
                                         QStringLiteral("the cookie retry"));
                        return;
                    }
                    emit resolved(url, false);
                    return;
                }
                Resolved entry;
                entry.video = lines.first().trimmed();
                // A combined selector prints one URL: the video stream carries
                // its own audio, so there is nothing to attach.
                entry.audio = lines.size() > 1 ? lines.at(1).trimmed() : QString();
                entry.stamp = QDateTime::currentSecsSinceEpoch();
                entry.cookies = m_cookieRetryTried;
                if (entry.video.isEmpty()) {
                    emit resolved(url, false);
                    return;
                }
                m_cache.insert(url, entry);
                emit resolved(url, true);
            });

    startEngine();
}

Player::~Player()
{
    m_shuttingDown = true;
    if (m_resolve.state() != QProcess::NotRunning) {
        m_resolve.kill();
        m_resolve.waitForFinished(500);
    }
    if (m_process.state() != QProcess::NotRunning) {
        send({QStringLiteral("quit")});
        m_process.waitForFinished(500);
        if (m_process.state() != QProcess::NotRunning)
            m_process.kill();
    }
}

QString Player::formatSelector() const
{
    // 1080p keeps VA-API decoding comfortable on this class of GPU while staying
    // ahead of any screen the user has. AV1 is allowed: Iris Xe reports
    // VAProfileAV1Profile0 VLD, so the reason other players exclude it does not
    // apply here.
    return m_audioOnly
               ? QStringLiteral("bestaudio/best")
               : QStringLiteral("bestvideo[height<=1080]+bestaudio/best[height<=1080]");
}

void Player::setError(const QString &message)
{
    if (message == m_lastError)
        return;
    m_lastError = message;
    emit lastErrorChanged();
}

void Player::setCookieSource(const QString &cookiesFile, const QString &cookiesFromBrowser)
{
    const bool changed = cookiesFile != m_cookiesFile || cookiesFromBrowser != m_cookiesFromBrowser;
    m_cookiesFile = cookiesFile;
    m_cookiesFromBrowser = cookiesFromBrowser;
    // The cookie source is a command-line option, so it takes effect on the next
    // engine start rather than mid-flight.
    if (changed && m_process.state() != QProcess::NotRunning) {
        m_process.closeWriteChannel();
        m_process.terminate();
        m_process.waitForFinished(1500);
        startEngine();
    }
}

void Player::startEngine()
{
    if (m_process.state() != QProcess::NotRunning) {
        // Named, because Qt's own "Process is already running" never says which
        // process it means -- and one of the six occurrences in the session this
        // was written for has never been accounted for.
        qWarning().noquote() << "hurutube: the playback engine is still running (state"
                             << int(m_process.state()) << "); not starting a second one";
        return;
    }

    stopStaleEngines();
    QFile::remove(m_socketPath);
    QDir().mkpath(QFileInfo(m_socketPath).absolutePath());

    QStringList args{
        QStringLiteral("--idle=yes"),
        QStringLiteral("--keep-open=no"),
        QStringLiteral("--no-terminal"),
        QStringLiteral("--really-quiet"),
        QStringLiteral("--input-ipc-server=%1").arg(m_socketPath),
        QStringLiteral("--volume=%1").arg(m_volume),
        // Video is what this app is for, and the user's own mpv.conf must not
        // redirect it into the terminal: an `vo=sixel` or `vo=tct` line there
        // (perfectly reasonable for a TUI player) would otherwise win and this
        // app would show no window at all. Command-line options beat the config
        // file, so these are stated explicitly rather than left to defaults.
        QStringLiteral("--vo=gpu-next"),
        QStringLiteral("--hwdec=vaapi"),
        QStringLiteral("--audio-display=no"),
        QStringLiteral("--save-position-on-quit=no"),
        // The app owns resume, so mpv's own watch-later database stays out of it.
        // NOT `--watch-later=no`: no such option exists (measured: mpv exits 1
        // without even creating the IPC socket), and the exit is silent, which is
        // why the engine's own stderr is reported below.
        QStringLiteral("--ytdl-format=%1").arg(formatSelector()),
    };

    // Test seam: HURUTUBE_AO=null / HURUTUBE_VO=null let playback be verified
    // without making a sound or a window (the app is often exercised while
    // somebody is using the machine). hwdec-current then reads "no", which is
    // what the seam is for -- the hardware claim is only ever made from a real
    // render path.
    const QByteArray audioOutput = qgetenv("HURUTUBE_AO");
    if (!audioOutput.isEmpty())
        args << QStringLiteral("--ao=%1").arg(QString::fromLocal8Bit(audioOutput));
    const QByteArray videoOutput = qgetenv("HURUTUBE_VO");
    if (!videoOutput.isEmpty())
        args << QStringLiteral("--vo=%1").arg(QString::fromLocal8Bit(videoOutput));

    // mpv's own yt-dlp call is the last resort -- this app resolves URLs itself
    // and hands mpv a direct stream -- so it deliberately carries NO cookies. A
    // signed-in session is exactly what makes YouTube serve no formats at all to
    // these clients, and a fallback that cannot play is worse than an anonymous
    // one that can. Cookies reach a video through the app's own resolve, after an
    // anonymous attempt has failed, and then mpv gets a direct URL anyway.
    // Without a JS runtime YouTube extraction degrades to images only.
    args << QStringLiteral("--ytdl-raw-options=%1").arg(QStringLiteral("js-runtimes=node"));

    m_process.start(m_mpv, args);
    if (!m_process.waitForStarted(3000)) {
        setError(tr("could not start mpv (%1); is it installed?").arg(m_mpv));
        return;
    }
    m_reconnect.start(100);
}

// An engine that starts and then exits looks exactly like an engine that is
// simply idle, so its own last words are kept and reported: that is the
// difference between "nothing plays" and knowing why.
//
// With video, the engine exits whenever the user closes mpv's window -- that is
// the normal way to stop watching, not a fault, so the app treats it as the end
// of playback and starts a fresh idle engine for the next video. Only an exit
// that came with something on stderr (a rejected option, a missing device) is
// reported as an error, and then the engine is left down rather than restarted
// into a loop.
void Player::handleEngineExit(int code, QProcess::ExitStatus status)
{
    if (m_shuttingDown)
        return;
    const QString said = QString::fromUtf8(m_engineError).trimmed();
    m_engineError.clear();
    m_ready = false;
    const bool wasPlaying = m_playing;
    const double lastPosition = m_position;
    const double lastDuration = m_duration;
    m_playing = false;
    m_paused = false;
    m_position = 0;
    m_duration = 0;
    emit positionChanged();
    emit durationChanged();
    emit stateChanged();

    if (said.isEmpty()) {
        emit engineClosed(wasPlaying, lastPosition, lastDuration);
        startEngine();
        return;
    }
    setError(tr("the playback engine (mpv) stopped: %1").arg(said));
    Q_UNUSED(code);
    Q_UNUSED(status);
}

// An engine that outlived the app keeps holding the IPC socket path, and the
// next start then talks to a process nobody is driving -- which reads as "the
// app stopped playing for no reason" with no error anywhere. Measured on this
// machine: two orphaned mpv processes after test runs, both of them hurutube's
// (a kill never runs the destructor that would have stopped them).
//
// Scoped to THIS socket path, and to a process whose program name is mpv: a
// stale engine belonging to another player of the user's is not ours to kill,
// and another app's socket path must never match here.
void Player::stopStaleEngines()
{
    const QByteArray needle = m_socketPath.toUtf8();
    const QDir proc(QStringLiteral("/proc"));
    const QStringList entries = proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &entry : entries) {
        bool numeric = false;
        const qint64 pid = entry.toLongLong(&numeric);
        if (!numeric || pid == QCoreApplication::applicationPid())
            continue;
        QFile cmdline(QStringLiteral("/proc/%1/cmdline").arg(pid));
        if (!cmdline.open(QIODevice::ReadOnly))
            continue;
        const QByteArray command = cmdline.readAll();
        if (!command.contains(needle))
            continue;
        // argv[0]'s basename has to be mpv; anything else that merely mentions
        // the path (a shell, a probe) is left alone.
        const QByteArray program = command.split('\0').value(0);
        if (!program.endsWith("/mpv") && program != QByteArrayLiteral("mpv"))
            continue;
        qInfo("hurutube: stopping a leftover engine (pid %lld)", static_cast<long long>(pid));
        ::kill(static_cast<pid_t>(pid), SIGTERM);
    }
}

// One line for the footer: the codec and the resolution the engine is really
// decoding, which is what makes a hardware-decode claim checkable rather than a
// claim.
void Player::updateVideoDescription()
{
    QString description;
    if (m_videoWidth > 0) {
        if (!m_videoFormat.isEmpty())
            description += m_videoFormat + QStringLiteral(" · ");
        description += QStringLiteral("%1×%2").arg(m_videoWidth).arg(m_videoHeight);
    }
    if (description != m_videoDescription) {
        m_videoDescription = description;
        emit videoChanged();
    }
}

void Player::connectSocket()
{
    if (m_socket.state() == QLocalSocket::ConnectedState)
        return;
    m_socket.connectToServer(m_socketPath);
}

void Player::observe(int id, const QString &property)
{
    send({QStringLiteral("observe_property"), id, property});
}

void Player::send(const QJsonArray &command, bool withRequestId)
{
    QJsonObject request;
    request.insert(QStringLiteral("command"), command);
    if (withRequestId)
        request.insert(QStringLiteral("request_id"), m_nextRequestId++);
    QByteArray line = QJsonDocument(request).toJson(QJsonDocument::Compact);
    line.append('\n');

    if (m_socket.state() != QLocalSocket::ConnectedState) {
        // Keeping the order matters: loadfile must not overtake set_property.
        m_pending.append(line);
        connectSocket();
        return;
    }
    m_socket.write(line);
    m_socket.flush();
}

void Player::flushPending()
{
    if (m_pending.isEmpty())
        return;
    for (const QByteArray &line : m_pending)
        m_socket.write(line);
    m_socket.flush();
    m_pending.clear();
}

void Player::presolve(const QString &url)
{
    if (url.isEmpty() || url.contains(QStringLiteral("googlevideo")))
        return;
    const auto cached = m_cache.constFind(url);
    if (cached != m_cache.constEnd()
        && QDateTime::currentSecsSinceEpoch() - cached->stamp < kResolveTtlSeconds)
        return;
    if (m_resolving == url && m_resolve.state() != QProcess::NotRunning)
        return;
    m_resolveOutput.clear();
    m_resolving = url;
    // Anonymous first, always: see resolveArguments(). The helper stops an
    // in-flight resolve and -- if it cannot be reaped in time -- defers this start
    // instead of dropping it, which would silently hand playback to mpv's own,
    // much slower extraction.
    m_cookieRetryTried = false;
    Restart::process(m_resolve, QStringLiteral("yt-dlp"), resolveArguments(url, false),
                     QStringLiteral("the resolver"));
}

QStringList Player::resolveArguments(const QString &url, bool withCookies) const
{
    QStringList arguments{QStringLiteral("-g"), QStringLiteral("--no-warnings"),
                          QStringLiteral("-f"), formatSelector()};
    if (withCookies) {
        if (!m_cookiesFile.isEmpty())
            arguments << QStringLiteral("--cookies") << m_cookiesFile;
        if (!m_cookiesFromBrowser.isEmpty())
            arguments << QStringLiteral("--cookies-from-browser") << m_cookiesFromBrowser;
    }
    arguments << url;
    return arguments;
}

void Player::setUsedCookies(bool used)
{
    if (used == m_usedCookies)
        return;
    m_usedCookies = used;
    emit usedCookiesChanged();
}

void Player::forget(const QString &url)
{
    m_cache.remove(url);
}

void Player::play(const QString &url, double startSeconds, const QString &title)
{
    if (url.isEmpty())
        return;
    m_position = 0;
    m_duration = 0;
    m_retried = false;
    m_currentUrl = url;
    m_currentTitle = title.isEmpty() ? url : title;
    m_currentStart = startSeconds > 5 ? startSeconds : 0;
    m_mediaTitle = m_currentTitle;
    emit positionChanged();
    emit durationChanged();
    emit mediaTitleChanged();

    QJsonObject options;
    if (!title.isEmpty())
        options.insert(QStringLiteral("force-media-title"), title);
    // The value has to be a STRING: mpv's per-file options object rejects a JSON
    // number for a time-valued option with `invalid parameter` (measured against
    // mpv 0.41: {"start":40} fails, {"start":"40"} works), and the whole loadfile
    // then does nothing at all.
    if (m_currentStart > 0)
        options.insert(QStringLiteral("start"), QString::number(m_currentStart, 'f', 3));

    const auto cached = m_cache.constFind(url);
    const bool usable = cached != m_cache.constEnd()
                        && QDateTime::currentSecsSinceEpoch() - cached->stamp < kResolveTtlSeconds;
    QString target = url;
    if (usable) {
        target = cached->video;
        if (!cached->audio.isEmpty())
            options.insert(QStringLiteral("audio-file"), cached->audio);
    }
    // From what actually happened, not from what is configured.
    setUsedCookies(usable && cached->cookies);
    m_mediaTitle = m_currentTitle;
    // `replace` so the new entry starts immediately instead of being appended to
    // an idle playlist; the app's own list is the source of order.
    send({QStringLiteral("loadfile"), target, QStringLiteral("replace"), -1, options});
    send({QStringLiteral("set_property"), QStringLiteral("pause"), false});
}

void Player::replayWithoutCache(const QString &url, double start, const QString &title)
{
    // The pre-resolved URL expired between the resolve and the play (or died
    // mid-flight). Drop it and let mpv resolve through yt-dlp itself: slower,
    // but it cannot be stale.
    forget(url);
    m_retried = true;
    QJsonObject options;
    if (!title.isEmpty())
        options.insert(QStringLiteral("force-media-title"), title);
    // String, not number -- see the note in play().
    if (start > 5)
        options.insert(QStringLiteral("start"), QString::number(start, 'f', 3));
    send({QStringLiteral("loadfile"), url, QStringLiteral("replace"), -1, options});
    send({QStringLiteral("set_property"), QStringLiteral("pause"), false});
}

void Player::togglePause()
{
    send({QStringLiteral("cycle"), QStringLiteral("pause")});
}

void Player::pause()
{
    send({QStringLiteral("set_property"), QStringLiteral("pause"), true});
}

void Player::resume()
{
    send({QStringLiteral("set_property"), QStringLiteral("pause"), false});
}

void Player::stop()
{
    send({QStringLiteral("stop")});
    m_position = 0;
    m_playing = false;
    m_paused = false;
    emit positionChanged();
    emit stateChanged();
}

void Player::seekAbsolute(double seconds)
{
    send({QStringLiteral("seek"), seconds, QStringLiteral("absolute")});
}

void Player::seekRelative(double seconds)
{
    send({QStringLiteral("seek"), seconds, QStringLiteral("relative")});
}

void Player::toggleFullscreen()
{
    send({QStringLiteral("cycle"), QStringLiteral("fullscreen")});
}

void Player::setVolume(int volume)
{
    volume = qBound(0, volume, 130);
    if (volume == m_volume)
        return;
    m_volume = volume;
    send({QStringLiteral("set_property"), QStringLiteral("volume"), m_volume});
    emit volumeChanged();
}

void Player::setAudioOnly(bool audioOnly)
{
    if (audioOnly == m_audioOnly)
        return;
    m_audioOnly = audioOnly;
    // Mid-flight this only has to hide the video stream, and mpv stops decoding
    // it once `vid` is "no" -- so the current video keeps playing instead of
    // being cut off, which is what restarting the engine here would do. The
    // format selector still decides what a LATER play downloads, and it is read
    // when the entry is pre-resolved.
    send({QStringLiteral("set_property"), QStringLiteral("vid"),
          audioOnly ? QStringLiteral("no") : QStringLiteral("auto")});
    emit audioOnlyChanged();
}

void Player::handleProperty(int id, const QJsonValue &value)
{
    switch (id) {
    case IdTimePos:
        if (value.isDouble()) {
            m_position = value.toDouble();
            emit positionChanged();
        }
        break;
    case IdDuration:
        if (value.isDouble()) {
            m_duration = value.toDouble();
            emit durationChanged();
        }
        break;
    case IdPause:
        if (value.isBool() && value.toBool() != m_paused) {
            m_paused = value.toBool();
            emit stateChanged();
        }
        break;
    case IdVolume:
        if (value.isDouble()) {
            const int volume = static_cast<int>(value.toDouble());
            if (volume != m_volume) {
                m_volume = volume;
                emit volumeChanged();
            }
        }
        break;
    case IdMediaTitle:
        if (value.isString() && value.toString() != m_mediaTitle) {
            m_mediaTitle = value.toString();
            emit mediaTitleChanged();
        }
        break;
    case IdHwdec:
        if (value.isString() && value.toString() != m_hwdec) {
            m_hwdec = value.toString();
            emit hwdecChanged();
        }
        break;
    case IdVideoParams: {
        const QJsonObject params = value.toObject();
        const int width = params.value(QStringLiteral("w")).toInt();
        const int height = params.value(QStringLiteral("h")).toInt();
        if (width != m_videoWidth || height != m_videoHeight) {
            m_videoWidth = width;
            m_videoHeight = height;
            updateVideoDescription();
        }
        break;
    }
    case IdVideoFormat:
        if (value.isString()) {
            const QString format = value.toString().toUpper();
            if (format != m_videoFormat) {
                m_videoFormat = format;
                updateVideoDescription();
            }
        }
        break;
    case IdIdleActive:
        if (value.isBool()) {
            const bool playing = !value.toBool();
            if (playing != m_playing) {
                m_playing = playing;
                emit stateChanged();
                if (playing)
                    emit started();
            }
        }
        break;
    default:
        break;
    }
}

void Player::handleLine(const QByteArray &line)
{
    const QJsonObject event = QJsonDocument::fromJson(line).object();
    const QString name = event.value(QStringLiteral("event")).toString();

    if (name == QStringLiteral("property-change")) {
        const QString property = event.value(QStringLiteral("name")).toString();
        const QJsonValue value = event.value(QStringLiteral("data"));
        // Map the property name back to the id we registered, so a reply to a
        // different observer ordering cannot land in the wrong slot.
        if (property == QStringLiteral("time-pos"))
            handleProperty(IdTimePos, value);
        else if (property == QStringLiteral("duration"))
            handleProperty(IdDuration, value);
        else if (property == QStringLiteral("pause"))
            handleProperty(IdPause, value);
        else if (property == QStringLiteral("volume"))
            handleProperty(IdVolume, value);
        else if (property == QStringLiteral("media-title"))
            handleProperty(IdMediaTitle, value);
        else if (property == QStringLiteral("hwdec-current"))
            handleProperty(IdHwdec, value);
        else if (property == QStringLiteral("video-params"))
            handleProperty(IdVideoParams, value);
        else if (property == QStringLiteral("video-format"))
            handleProperty(IdVideoFormat, value);
        else if (property == QStringLiteral("idle-active"))
            handleProperty(IdIdleActive, value);
        return;
    }

    if (name == QStringLiteral("end-file")) {
        const QString reason = event.value(QStringLiteral("reason")).toString();
        if (reason == QStringLiteral("eof")) {
            emit finished();
            return;
        }
        if (reason == QStringLiteral("error")) {
            // A dead pre-resolved URL is the likeliest cause, and it is worth
            // one slow retry rather than an error the user cannot act on.
            if (!m_retried && !m_currentUrl.isEmpty()) {
                replayWithoutCache(m_currentUrl, m_currentStart, m_currentTitle);
                return;
            }
            // Say what went wrong rather than looking like a finished video.
            setError(tr("mpv could not play that video"));
            m_playing = false;
            emit stateChanged();
        }
        return;
    }

    if (name.isEmpty()) {
        // A reply to one of our commands.
        const QJsonValue error = event.value(QStringLiteral("error"));
        if (error.isString() && error.toString() != QStringLiteral("success"))
            setError(QStringLiteral("mpv: %1").arg(error.toString()));
    }
}
