#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

// YouTube browsing: search, and channel/playlist listing.
//
// Two engines, chosen per job -- measured, not guessed:
//
//  * SEARCH goes to YouTube's own inner API (the WEB client). One POST returns
//    real videos with channel, duration and view count in well under a second,
//    and -- the reason it is here rather than yt-dlp -- SHORTS ARRIVE IN THEIR
//    OWN RENDERER (`shortsLockupViewModel`), so they are left out structurally
//    instead of being filtered by a duration guess that also throws away real
//    short videos. Measured on a "cats being stupid" search: 12 `videoRenderer`
//    and 30 `shortsLockupViewModel` in the same reply, and on "kdenlive
//    tutorial": 17 videos, 0 shorts. This is the Shorts block.
//  * CHANNELS and PLAYLISTS go through yt-dlp, because the inner browse
//    endpoint answers an anonymous WEB client with a header and nothing else
//    (measured: responseContext/header/alerts/trackingParams/topbar, no
//    `contents`). yt-dlp handles pagination and cookies, and a channel's
//    `/videos` tab carries no Shorts at all -- so that block comes free.
//
// The API credentials (key + client version) are read from youtube.com at
// runtime and cached, because they rotate.
class Search : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(QString heading READ heading NOTIFY headingChanged)

public:
    explicit Search(QObject *parent = nullptr, const QString &ytdlpBinary = {});
    ~Search() override;

    bool busy() const { return m_ytdlpProcess.state() != QProcess::NotRunning || m_reply != nullptr; }
    QString lastError() const { return m_lastError; }
    QString heading() const { return m_heading; }

    // Parses a WEB search response into rows. Static and pure, so it is tested
    // against the payload saved in tests/fixtures with no network at all.
    static QVariantList parseSearchResponse(const QByteArray &json);
    // The same for a `yt-dlp --flat-playlist -J` reply.
    static QVariantList parseFlatList(const QByteArray &json);

    // What a pasted string is: "video", "playlist", "channel", "shorts",
    // "search" or "unknown". Pure, so the refusals are testable.
    static QString classify(const QString &text);
    // The 11-character id in a URL, or an empty string.
    static QString videoId(const QString &text);
    // Seconds to "23:56" / "1:02:03", and a raw count to "68K views". Both pure,
    // because yt-dlp's flat listing gives numbers where the inner API gives the
    // human string YouTube already formatted.
    static QString formatDuration(double seconds);
    static QString formatViews(double count);
    // The other direction, for the API's own "23:56": the list needs seconds to
    // draw a resume marker, and re-asking YouTube for each row would be absurd.
    static double secondsFromText(const QString &text);

    // Cookies go to BOTH engines: the inner API does not need them, but yt-dlp
    // does for restricted videos, and the engine (mpv) resolves through yt-dlp.
    void setCookies(const QString &cookiesFile, const QString &cookiesFromBrowser);

    Q_INVOKABLE void search(const QString &query);
    // A URL, an id, or -- when it is neither -- a search phrase.
    Q_INVOKABLE void open(const QString &text);
    Q_INVOKABLE void loadChannel(const QString &path);
    Q_INVOKABLE void loadPlaylist(const QString &id);

signals:
    void busyChanged();
    void lastErrorChanged();
    void headingChanged();
    // A list to show: one entry per video, keys `id, title, channel, channelId,
    // duration, views, published, live, thumbnail, url`.
    void resultsReady(const QVariantList &entries, const QString &heading);
    // A single video asked for directly, which the window plays at once.
    void videoReady(const QVariantMap &entry);
    void failed(const QString &message);
    // A Shorts link was refused, on purpose. Carries the id for the message.
    void shortsRefused(const QString &id);

private:
    void fail(const QString &message);
    void setHeading(const QString &heading);
    void fetchCredentials();
    void performSearch(const QString &query);
    void runYtDlp(const QString &url, const QString &heading);
    void handleFlatList(const QByteArray &json);
    QStringList cookieArguments() const;
    bool needsCredentials() const { return m_key.isEmpty() || m_clientVersion.isEmpty(); }

    QString m_ytdlp;
    QString m_ytdlpPointAt;
    QProcess m_ytdlpProcess;
    QByteArray m_ytdlpOutput;
    QNetworkAccessManager m_network;
    QNetworkReply *m_reply = nullptr;
    QString m_key;
    QString m_clientVersion;
    QString m_pendingQuery;
    QString m_lastError;
    QString m_heading;
    QString m_cookiesFile;
    QString m_cookiesFromBrowser;
    QString m_currentRequest;
};
