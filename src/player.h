#pragma once

#include <QHash>
#include <QJsonObject>
#include <QLocalSocket>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QTimer>

#include "codec.h"
#include "resolves.h"

// Video playback, driven through mpv's JSON IPC socket.
//
// mpv is the engine on purpose: it resolves YouTube URLs through yt-dlp itself,
// it decodes through VA-API, and Qt Multimedia could do neither without a lot of
// glue (and linking libav). VIDEO lives in mpv's own window, not inside this app:
// embedding libmpv would mean linking mpv-libs and sharing the Qt Quick scene
// graph's GL context, for a window the user watches full screen anyway.
//
// THE FAST PATH is pre-resolution. Measured on studiobox2: letting mpv resolve a
// URL itself took 17.5 s to first frame, because that is yt-dlp extracting
// formats inside the playback path. Resolving the same URL ahead of time with
// `yt-dlp -g` and handing mpv the two direct URLs through loadfile's per-file
// options reached a picture in 0.9 s (yt-dlp itself still costs ~12 s, but it
// runs while the user is still reading the list). Note which form works: setting
// the `audio-file` property BEFORE loadfile attaches NO audio at all; the option
// only takes effect as part of the loadfile call.
//
// A pre-resolved URL is a googlevideo URL that expires (hours, per IP). If one
// dies mid-flight, the entry is dropped from the cache and the same video is
// retried once through the plain YouTube URL, which is slow but always correct.
class Player : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool playing READ playing NOTIFY stateChanged)
    Q_PROPERTY(bool paused READ paused NOTIFY stateChanged)
    Q_PROPERTY(double position READ position NOTIFY positionChanged)
    Q_PROPERTY(double duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(int volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(QString mediaTitle READ mediaTitle NOTIFY mediaTitleChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    // What the engine is really decoding with: "vaapi" is hardware, anything
    // else is software, and the window says so rather than assuming.
    Q_PROPERTY(QString hwdec READ hwdec NOTIFY hwdecChanged)
    Q_PROPERTY(QString videoDescription READ videoDescription NOTIFY videoChanged)
    // True when this video needed the cookies to resolve -- an anonymous attempt
    // had already failed. Reported because "it only played signed in" is worth
    // knowing, and because claiming it from the configuration rather than from
    // what happened would be a guess.
    Q_PROPERTY(bool usedCookies READ usedCookies NOTIFY usedCookiesChanged)
    Q_PROPERTY(bool audioOnly READ audioOnly WRITE setAudioOnly NOTIFY audioOnlyChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged)
    // What codec will be asked for, and why -- from VA-API's own answer about this
    // machine, never from an assumption about its generation.
    Q_PROPERTY(QString codecStatus READ codecStatus NOTIFY codecChanged)
    // The same thing in a few words, for the footer.
    Q_PROPERTY(QString codecSummary READ codecSummary NOTIFY codecChanged)
    // True while a play is WAITING for its own resolve. The footer says so: a wait
    // the user can see is not the same as a hang.
    Q_PROPERTY(bool resolving READ resolving NOTIFY resolvingChanged)

public:
    explicit Player(QObject *parent = nullptr, const QString &mpvBinary = {},
                    const QString &socketPath = {});
    ~Player() override;

    bool playing() const { return m_playing; }
    bool paused() const { return m_paused; }
    double position() const { return m_position; }
    double duration() const { return m_duration; }
    int volume() const { return m_volume; }
    QString mediaTitle() const { return m_mediaTitle; }
    QString lastError() const { return m_lastError; }
    QString hwdec() const { return m_hwdec; }
    QString videoDescription() const { return m_videoDescription; }
    bool usedCookies() const { return m_usedCookies; }
    bool audioOnly() const { return m_audioOnly; }
    bool ready() const { return m_ready; }
    QString codecStatus() const { return Codec::describe(m_codec, m_capability); }
    QString codecSummary() const { return Codec::summary(m_codec, m_capability); }

    void setVolume(int volume);
    Q_INVOKABLE void setVolumeFromQml(int volume) { setVolume(volume); }

    void setCookieSource(const QString &cookiesFile, const QString &cookiesFromBrowser);
    Q_INVOKABLE void setCookieSourceQml(const QString &cookiesFile, const QString &cookiesFromBrowser)
    {
        setCookieSource(cookiesFile, cookiesFromBrowser);
    }

    // Haswell and Broadwell decode H.264 in hardware and nothing newer, so on
    // those machines the plain `bestvideo` (AV1 or VP9) silently decodes on the
    // CPU. `codec = auto` reads that from VA-API; an explicit preference overrides
    // it, and V in the window cycles it for the session. Nothing is written back
    // to config.toml, which is read-only on purpose (config.h).
    void setCodecPreference(Codec::Preference preference);
    Q_INVOKABLE void cycleCodec();
    Q_INVOKABLE QString codecName() const
    {
        return Codec::name(Codec::effective(m_codec, m_capability));
    }

    // The engine is started by the app, never by the constructor. main() sets the
    // cookie source first, and setCookieSource() terminates the engine when the
    // source changes -- so starting in the constructor meant every launch WITH a
    // cookie source killed the engine it had just started, and when mpv had not
    // finished dying inside the wait, the restart was skipped by the guard: the
    // app then had no engine at all (measured: a socket that refuses connections
    // while an mpv process still existed). Nothing needs an engine before the
    // window is loaded, so the start belongs here.
    void start();

    // `title` becomes mpv's window title, which for a direct stream URL would
    // otherwise be a googlevideo URL.
    Q_INVOKABLE void play(const QString &url, double startSeconds, const QString &title);
    // Resolve in the background so the slow half overlaps the browsing. Safe to
    // call on every selection change: an in-flight resolve for another URL is
    // abandoned -- unless a play is waiting on it.
    Q_INVOKABLE void presolve(const QString &url);
    // The same, for the rows the user is about to move to: they are resolved one
    // at a time, in order, because extractions that overlap are ~9x slower each
    // (measured: 8 s alone, 73 s with three running).
    Q_INVOKABLE void presolveSoon(const QStringList &urls);
    bool resolving() const { return m_awaitingPlay; }
    // Forget a cached resolve (the row is gone, or playback of it failed).
    Q_INVOKABLE void forget(const QString &url);

    Q_INVOKABLE void togglePause();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void resume();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void seekAbsolute(double seconds);
    Q_INVOKABLE void seekRelative(double seconds);
    Q_INVOKABLE void toggleFullscreen();
    void setAudioOnly(bool audioOnly);

signals:
    void stateChanged();
    void positionChanged();
    void durationChanged();
    void volumeChanged();
    void mediaTitleChanged();
    void lastErrorChanged();
    void hwdecChanged();
    void videoChanged();
    void usedCookiesChanged();
    void audioOnlyChanged();
    void codecChanged();
    void resolvingChanged();
    // Playback of the current entry ran to its end (reason=eof), which is what
    // advances the list.
    void finished();
    // The engine exited because the user closed its window. `wasPlaying` says
    // whether that interrupted a video or just ended one, and the position and
    // duration ride along because this is the last moment they are known.
    void engineClosed(bool wasPlaying, double position, double duration);
    void started();
    // The background resolve finished: `ok` says whether a fast start is ready.
    void resolved(const QString &url, bool ok);

private:
    void startEngine();
    // Terminate a *running* engine and start a fresh one with the new command line.
    // The wait is not cosmetic: if mpv has not finished dying inside it,
    // startEngine() refuses to start a second one and the app is left with no
    // engine at all -- measured once, and the socket then refused connections while
    // an mpv process still existed. A process that outlives SIGTERM is killed
    // rather than waited on forever.
    void restartRunningEngine();
    void send(const QJsonArray &command, bool withRequestId = true);
    void flushPending();
    void handleLine(const QByteArray &line);
    void handleProperty(int id, const QJsonValue &value);
    void setError(const QString &message);
    void observe(int id, const QString &property);
    void connectSocket();
    void handleEngineExit(int code, QProcess::ExitStatus status);
    void updateVideoDescription();
    void stopStaleEngines();
    QString formatSelector() const;
    void replayWithoutCache(const QString &url, double start, const QString &title);
    // The one place a resolve is started, so play() and presolve() cannot drift.
    void startResolve(const QString &url, bool withCookies);
    // Load a resolved entry straight into the engine: the ~1 s path.
    void playResolved(const Resolves::Entry &entry);
    // Resolve the next queued row, unless a play is waiting for its own.
    void resolveNextInQueue();
    // Give up on our own resolve and let mpv extract it instead. Kills the
    // resolve FIRST, so this never becomes the two-extractions case it fixes.
    void fallBackToMpv();

    // yt-dlp's arguments for ONE resolve attempt. Anonymous goes first and the
    // cookies are a fallback, never the default: a signed-in session is what
    // makes YouTube hand the clients yt-dlp uses an empty format list (measured
    // on this machine: 37 formats anonymously against 0 signed in, on every
    // video tried), so cookies-as-default plays nothing at all.
    QStringList resolveArguments(const QString &url, bool withCookies) const;
    bool hasCookieSource() const
    {
        return !m_cookiesFile.isEmpty() || !m_cookiesFromBrowser.isEmpty();
    }
    void setUsedCookies(bool used);

    QString m_mpv;
    QString m_socketPath;
    QString m_cookiesFile;
    QString m_cookiesFromBrowser;
    QProcess m_process;
    QLocalSocket m_socket;
    QByteArray m_buffer;
    // Commands issued before the socket is up (a URL passed on the command line
    // is played immediately, long before mpv has finished starting) are queued
    // here instead of being written into a closed device and lost.
    QList<QByteArray> m_pending;
    QTimer m_reconnect;
    int m_nextRequestId = 100;
    bool m_observed = false;
    bool m_ready = false;
    bool m_shuttingDown = false;
    QByteArray m_engineError;

    QProcess m_resolve;
    QByteArray m_resolveOutput;
    QString m_resolving;
    // Direct URLs, now kept across runs: a fresh entry turns a 7-31 s resolve into
    // a 1 s start, and history replays are the common case (resolves.cpp).
    Resolves m_cache;
    // A play that is waiting for the resolve of its own URL. This is what stops
    // the app extracting the same video twice at once (measured: 23.5 s against
    // 7 s for one extraction): mpv is only handed the plain URL if our own
    // resolve FAILS, and then only after it has been killed.
    struct PendingPlay {
        QString url;
    } m_pendingPlay;
    bool m_awaitingPlay = false;
    // Rows to resolve after the current one, one at a time and in order.
    QStringList m_presolveQueue;
    // Safety net: a resolve that never returns must not hang playback forever.
    QTimer m_playTimeout;
    // The entry currently loaded, so a failed direct URL can be retried the slow
    // way exactly once.
    QString m_currentUrl;
    QString m_currentTitle;
    double m_currentStart = 0;
    bool m_retried = false;
    // Set for the second resolve attempt of one URL: it is what tells a
    // cookie-only video apart from one that came back anonymously.
    bool m_cookieRetryTried = false;

    bool m_playing = false;
    bool m_paused = false;
    bool m_audioOnly = false;
    double m_position = 0;
    double m_duration = 0;
    int m_volume = 100;
    QString m_mediaTitle;
    QString m_lastError;
    QString m_hwdec;
    // What this machine can decode in hardware, asked once at startup, plus the
    // preference in force. Together they decide the yt-dlp format expression.
    Codec::Preference m_codec = Codec::Preference::Auto;
    Codec::Capability m_capability;
    bool m_usedCookies = false;
    QString m_videoDescription;
    QString m_videoFormat;
    int m_videoWidth = 0;
    int m_videoHeight = 0;
};
