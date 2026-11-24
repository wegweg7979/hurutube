#include "search.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <QUrlQuery>

namespace {

// YouTube answers a browser user agent and a consent cookie; an agent-looking
// request gets a redirect to consent.youtube.com whose page carries no API key.
constexpr char kUserAgent[] =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/131.0.0.0 Safari/537.36";
constexpr char kConsentCookie[] = "SOCS=CAI";

// A page of a channel or a playlist. Long enough to be useful, short enough to
// keep the yt-dlp call inside a second or two.
constexpr int kFlatListLimit = 60;

QString textOf(const QJsonObject &node, const QString &key)
{
    const QJsonObject holder = node.value(key).toObject();
    const QString simple = holder.value(QStringLiteral("simpleText")).toString();
    if (!simple.isEmpty())
        return simple;
    const QJsonArray runs = holder.value(QStringLiteral("runs")).toArray();
    QString joined;
    for (const QJsonValue &run : runs) {
        if (!joined.isEmpty())
            joined += QLatin1Char(' ');
        joined += run.toObject().value(QStringLiteral("text")).toString();
    }
    return joined;
}

QString browseIdOf(const QJsonObject &node, const QString &key)
{
    const QJsonArray runs = node.value(key).toObject().value(QStringLiteral("runs")).toArray();
    if (runs.isEmpty())
        return {};
    return runs.first()
        .toObject()
        .value(QStringLiteral("navigationEndpoint"))
        .toObject()
        .value(QStringLiteral("browseEndpoint"))
        .toObject()
        .value(QStringLiteral("browseId"))
        .toString();
}

// Walks the response and returns every node stored under `key`, in document
// order -- which is YouTube's own ranking order, and the order the list shows.
QList<QJsonObject> collect(const QJsonValue &value, const QString &key)
{
    QList<QJsonObject> found;
    if (value.isObject()) {
        const QJsonObject object = value.toObject();
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            if (it.key() == key && it.value().isObject())
                found.append(it.value().toObject());
            found += collect(it.value(), key);
        }
    } else if (value.isArray()) {
        const QJsonArray array = value.toArray();
        for (const QJsonValue &entry : array)
            found += collect(entry, key);
    }
    return found;
}

QVariantMap videoRow(const QJsonObject &video)
{
    const QString id = video.value(QStringLiteral("videoId")).toString();
    if (id.size() != 11)
        return {};

    QVariantMap row;
    row.insert(QStringLiteral("id"), id);
    row.insert(QStringLiteral("title"), textOf(video, QStringLiteral("title")));
    row.insert(QStringLiteral("channel"), textOf(video, QStringLiteral("ownerText")));
    row.insert(QStringLiteral("channelId"), browseIdOf(video, QStringLiteral("ownerText")));
    row.insert(QStringLiteral("duration"), textOf(video, QStringLiteral("lengthText")));
    row.insert(QStringLiteral("seconds"),
               Search::secondsFromText(textOf(video, QStringLiteral("lengthText"))));
    row.insert(QStringLiteral("views"), textOf(video, QStringLiteral("viewCountText")));
    row.insert(QStringLiteral("published"), textOf(video, QStringLiteral("publishedTimeText")));
    // A live stream carries no length; the badge is what says so, and without
    // noting it the row looks like a video whose duration failed to load. Any
    // other badge ("New", "4K", verified) must not be mistaken for one.
    const QString badges =
        QString::fromUtf8(QJsonDocument(video.value(QStringLiteral("badges")).toArray())
                              .toJson(QJsonDocument::Compact));
    row.insert(QStringLiteral("live"),
               textOf(video, QStringLiteral("lengthText")).isEmpty()
                   && badges.contains(QStringLiteral("LIVE")));
    // Built from the id rather than taken from the payload: the payload's URLs
    // are signed and expire, and this one is what the CDN has cached.
    row.insert(QStringLiteral("thumbnail"),
               QStringLiteral("https://i.ytimg.com/vi/%1/mqdefault.jpg").arg(id));
    row.insert(QStringLiteral("url"), QStringLiteral("https://www.youtube.com/watch?v=%1").arg(id));
    return row;
}

}  // namespace

Search::Search(QObject *parent, const QString &ytdlpBinary)
    : QObject(parent),
      m_ytdlp(ytdlpBinary.isEmpty() ? QStringLiteral("yt-dlp") : ytdlpBinary)
{
    connect(&m_ytdlpProcess, &QProcess::readyReadStandardOutput, this, [this] {
        m_ytdlpOutput += m_ytdlpProcess.readAllStandardOutput();
    });
    connect(&m_ytdlpProcess,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            [this](int code, QProcess::ExitStatus status) {
                const QByteArray output = m_ytdlpOutput;
                m_ytdlpOutput.clear();
                const QString what = m_currentRequest;
                m_currentRequest.clear();
                emit busyChanged();
                if (status != QProcess::NormalExit || code != 0) {
                    fail(tr("yt-dlp could not list %1").arg(what));
                    return;
                }
                handleFlatList(output);
            });
}

Search::~Search()
{
    if (m_ytdlpProcess.state() != QProcess::NotRunning) {
        m_ytdlpProcess.kill();
        m_ytdlpProcess.waitForFinished(500);
    }
}

void Search::setHeading(const QString &heading)
{
    if (heading == m_heading)
        return;
    m_heading = heading;
    emit headingChanged();
}

void Search::fail(const QString &message)
{
    m_lastError = message;
    emit lastErrorChanged();
    emit failed(message);
}

void Search::setCookies(const QString &cookiesFile, const QString &cookiesFromBrowser)
{
    m_cookiesFile = cookiesFile;
    m_cookiesFromBrowser = cookiesFromBrowser;
}

QStringList Search::cookieArguments() const
{
    QStringList arguments;
    if (!m_cookiesFile.isEmpty())
        arguments << QStringLiteral("--cookies") << m_cookiesFile;
    if (!m_cookiesFromBrowser.isEmpty())
        arguments << QStringLiteral("--cookies-from-browser") << m_cookiesFromBrowser;
    return arguments;
}

QString Search::videoId(const QString &text)
{
    static const QRegularExpression patterns[] = {
        QRegularExpression(QStringLiteral("[?&]v=([A-Za-z0-9_-]{11})")),
        QRegularExpression(QStringLiteral("youtu\\.be/([A-Za-z0-9_-]{11})")),
        QRegularExpression(QStringLiteral("/embed/([A-Za-z0-9_-]{11})")),
        QRegularExpression(QStringLiteral("/live/([A-Za-z0-9_-]{11})")),
        // A Shorts link carries a real video id, and the refusal message names
        // it, so this pattern has to exist even though the link itself is never
        // opened.
        QRegularExpression(QStringLiteral("/shorts/([A-Za-z0-9_-]{11})")),
    };
    for (const QRegularExpression &pattern : patterns) {
        const QRegularExpressionMatch match = pattern.match(text);
        if (match.hasMatch())
            return match.captured(1);
    }
    static const QRegularExpression bare(QStringLiteral("^[A-Za-z0-9_-]{11}$"));
    if (bare.match(text.trimmed()).hasMatch())
        return text.trimmed();
    return {};
}

QString Search::classify(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return QStringLiteral("unknown");
    // Checked before the id patterns: /shorts/<id> carries a real video id, and
    // this refusal is the point of the app, not an accident of parsing.
    if (trimmed.contains(QStringLiteral("/shorts/")))
        return QStringLiteral("shorts");
    if (!videoId(trimmed).isEmpty())
        return QStringLiteral("video");
    if (trimmed.contains(QStringLiteral("list=")) && trimmed.contains(QStringLiteral("playlist")))
        return QStringLiteral("playlist");
    if (trimmed.contains(QStringLiteral("list=")))
        return QStringLiteral("playlist");
    static const QRegularExpression channel(
        QStringLiteral("youtube\\.com/(@[^/]+|channel/[A-Za-z0-9_-]+|c/[^/]+|user/[^/]+)"));
    if (channel.match(trimmed).hasMatch())
        return QStringLiteral("channel");
    return QStringLiteral("search");
}

QVariantList Search::parseSearchResponse(const QByteArray &json)
{
    const QJsonObject root = QJsonDocument::fromJson(json).object();
    QVariantList rows;
    QSet<QString> seen;
    for (const QJsonObject &video : collect(root, QStringLiteral("videoRenderer"))) {
        const QVariantMap row = videoRow(video);
        if (row.isEmpty())
            continue;
        const QString id = row.value(QStringLiteral("id")).toString();
        if (seen.contains(id))
            continue;
        seen.insert(id);
        rows.append(row);
    }
    return rows;
}

QVariantList Search::parseFlatList(const QByteArray &json)
{
    const QJsonObject root = QJsonDocument::fromJson(json).object();
    QVariantList rows;
    QSet<QString> seen;
    const QJsonArray entries = root.value(QStringLiteral("entries")).toArray();
    for (const QJsonValue &value : entries) {
        const QJsonObject entry = value.toObject();
        const QString id = entry.value(QStringLiteral("id")).toString();
        if (id.size() != 11 || seen.contains(id))
            continue;
        seen.insert(id);

        const double seconds = entry.value(QStringLiteral("duration")).toDouble();
        const double views = entry.value(QStringLiteral("view_count")).toDouble();

        QVariantMap row;
        row.insert(QStringLiteral("id"), id);
        row.insert(QStringLiteral("title"), entry.value(QStringLiteral("title")).toString());
        row.insert(QStringLiteral("channel"),
                   entry.value(QStringLiteral("channel")).toString().isEmpty()
                       ? entry.value(QStringLiteral("uploader")).toString()
                       : entry.value(QStringLiteral("channel")).toString());
        row.insert(QStringLiteral("channelId"), entry.value(QStringLiteral("channel_id")).toString());
        row.insert(QStringLiteral("duration"), formatDuration(seconds));
        row.insert(QStringLiteral("seconds"), seconds);
        row.insert(QStringLiteral("views"), views > 0 ? formatViews(views) : QString());
        row.insert(QStringLiteral("viewCount"), views);
        row.insert(QStringLiteral("published"), QString());
        row.insert(QStringLiteral("live"),
                   entry.value(QStringLiteral("live_status")).toString() == QStringLiteral("is_live"));
        row.insert(QStringLiteral("thumbnail"),
                   QStringLiteral("https://i.ytimg.com/vi/%1/mqdefault.jpg").arg(id));
        row.insert(QStringLiteral("url"), QStringLiteral("https://www.youtube.com/watch?v=%1").arg(id));
        rows.append(row);
    }
    return rows;
}

QString Search::formatDuration(double seconds)
{
    if (seconds <= 0)
        return {};
    const int total = static_cast<int>(seconds);
    const int hours = total / 3600;
    const int minutes = (total % 3600) / 60;
    const int rest = total % 60;
    if (hours > 0)
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(rest, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(minutes).arg(rest, 2, 10, QLatin1Char('0'));
}

QString Search::formatViews(double count)
{
    if (count <= 0)
        return {};
    const double million = 1000000.0;
    const double thousand = 1000.0;
    if (count >= million)
        return QStringLiteral("%1M views").arg(count / million, 0, 'f', 1);
    if (count >= thousand)
        return QStringLiteral("%1K views").arg(count / thousand, 0, 'f', 0);
    return QStringLiteral("%1 views").arg(static_cast<qlonglong>(count));
}

double Search::secondsFromText(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return 0;
    const QStringList parts = trimmed.split(QLatin1Char(':'));
    if (parts.size() > 3)
        return 0;
    double total = 0;
    for (const QString &part : parts) {
        bool ok = false;
        const int value = part.toInt(&ok);
        if (!ok)
            return 0;
        total = total * 60 + value;
    }
    return total;
}

void Search::fetchCredentials()
{
    QNetworkRequest request(QUrl(QStringLiteral("https://www.youtube.com/")));
    request.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(kUserAgent));
    request.setRawHeader("Accept-Language", "en-US,en;q=0.9");
    request.setRawHeader("Cookie", kConsentCookie);
    m_reply = m_network.get(request);
    emit busyChanged();
    connect(m_reply, &QNetworkReply::finished, this, [this] {
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        const QByteArray body = reply->readAll();
        reply->deleteLater();
        emit busyChanged();
        if (reply->error() != QNetworkReply::NoError) {
            fail(tr("could not reach YouTube (%1)").arg(reply->errorString()));
            return;
        }
        static const QRegularExpression keyPattern(QStringLiteral("\"INNERTUBE_API_KEY\":\"([^\"]+)\""));
        static const QRegularExpression versionPattern(
            QStringLiteral("\"INNERTUBE_CLIENT_VERSION\":\"([^\"]+)\""));
        const QRegularExpressionMatch keyMatch = keyPattern.match(QString::fromUtf8(body));
        const QRegularExpressionMatch versionMatch = versionPattern.match(QString::fromUtf8(body));
        if (!keyMatch.hasMatch() || !versionMatch.hasMatch()) {
            fail(tr("YouTube answered without its API credentials (a consent or bot page?)"));
            return;
        }
        m_key = keyMatch.captured(1);
        m_clientVersion = versionMatch.captured(1);
        if (!m_pendingQuery.isEmpty()) {
            const QString query = m_pendingQuery;
            m_pendingQuery.clear();
            performSearch(query);
        }
    });
}

void Search::search(const QString &query)
{
    const QString trimmed = query.trimmed();
    if (trimmed.isEmpty())
        return;
    setHeading(tr("Results for “%1”").arg(trimmed));
    if (needsCredentials()) {
        m_pendingQuery = trimmed;
        fetchCredentials();
        return;
    }
    performSearch(trimmed);
}

void Search::performSearch(const QString &query)
{
    QJsonObject context;
    QJsonObject client;
    client.insert(QStringLiteral("clientName"), QStringLiteral("WEB"));
    client.insert(QStringLiteral("clientVersion"), m_clientVersion);
    client.insert(QStringLiteral("hl"), QStringLiteral("en"));
    client.insert(QStringLiteral("gl"), QStringLiteral("US"));
    context.insert(QStringLiteral("client"), client);

    QJsonObject body;
    body.insert(QStringLiteral("context"), context);
    body.insert(QStringLiteral("query"), query);

    QNetworkRequest request(QUrl(QStringLiteral("https://www.youtube.com/youtubei/v1/search?key=%1"
                                               "&prettyPrint=false")
                                     .arg(m_key)));
    request.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(kUserAgent));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Accept-Language", "en-US,en;q=0.9");
    request.setRawHeader("Cookie", kConsentCookie);
    m_reply = m_network.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    emit busyChanged();
    connect(m_reply, &QNetworkReply::finished, this, [this, query] {
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        const QByteArray body = reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();
        emit busyChanged();
        if (reply->error() != QNetworkReply::NoError || status != 200) {
            fail(tr("search failed (HTTP %1)").arg(status));
            return;
        }
        const QVariantList rows = parseSearchResponse(body);
        if (rows.isEmpty()) {
            // Credentials rotate; a stale key answers 200 with no results, so
            // drop them and let the next search fetch a fresh pair.
            m_key.clear();
            m_clientVersion.clear();
            fail(tr("no results for “%1”").arg(query));
            return;
        }
        emit resultsReady(rows, m_heading);
    });
}

void Search::open(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return;
    const QString kind = classify(trimmed);
    if (kind == QStringLiteral("shorts")) {
        emit shortsRefused(videoId(trimmed));
        return;
    }
    if (kind == QStringLiteral("video")) {
        const QString id = videoId(trimmed);
        QVariantMap row;
        row.insert(QStringLiteral("id"), id);
        row.insert(QStringLiteral("title"), id);
        row.insert(QStringLiteral("channel"), QString());
        row.insert(QStringLiteral("channelId"), QString());
        row.insert(QStringLiteral("duration"), QString());
        row.insert(QStringLiteral("views"), QString());
        row.insert(QStringLiteral("published"), QString());
        row.insert(QStringLiteral("live"), false);
        row.insert(QStringLiteral("thumbnail"),
                   QStringLiteral("https://i.ytimg.com/vi/%1/mqdefault.jpg").arg(id));
        row.insert(QStringLiteral("url"),
                   QStringLiteral("https://www.youtube.com/watch?v=%1").arg(id));
        emit videoReady(row);
        return;
    }
    if (kind == QStringLiteral("playlist")) {
        QUrl url(trimmed);
        const QString id = QUrlQuery(url.query()).queryItemValue(QStringLiteral("list"));
        loadPlaylist(id);
        return;
    }
    if (kind == QStringLiteral("channel")) {
        loadChannel(trimmed);
        return;
    }
    search(trimmed);
}

void Search::loadChannel(const QString &path)
{
    QString url = path.trimmed();
    if (!url.startsWith(QStringLiteral("http"))) {
        if (url.startsWith(QLatin1Char('@')))
            url = QStringLiteral("https://www.youtube.com/") + url;
        else if (url.startsWith(QStringLiteral("UC")) && url.size() > 20)
            url = QStringLiteral("https://www.youtube.com/channel/") + url;
        else
            url = QStringLiteral("https://www.youtube.com/@") + url;
    }
    // The /videos tab, not the channel front page: this is what keeps Shorts
    // out of a channel listing, since YouTube publishes them under /shorts.
    while (url.endsWith(QLatin1Char('/')))
        url.chop(1);
    for (const QString &tab : {QStringLiteral("/videos"), QStringLiteral("/shorts"),
                               QStringLiteral("/playlists"), QStringLiteral("/featured")}) {
        if (url.endsWith(tab))
            url.chop(tab.size());
    }
    url += QStringLiteral("/videos");
    runYtDlp(url, tr("Channel videos"));
}

void Search::loadPlaylist(const QString &id)
{
    if (id.isEmpty())
        return;
    runYtDlp(QStringLiteral("https://www.youtube.com/playlist?list=%1").arg(id), tr("Playlist"));
}

void Search::runYtDlp(const QString &url, const QString &heading)
{
    if (m_ytdlpProcess.state() != QProcess::NotRunning) {
        m_ytdlpProcess.kill();
        m_ytdlpProcess.waitForFinished(500);
    }
    m_ytdlpOutput.clear();
    m_currentRequest = heading;
    setHeading(heading);
    QStringList arguments{
        QStringLiteral("--flat-playlist"),
        QStringLiteral("--playlist-end"),
        QString::number(kFlatListLimit),
        QStringLiteral("--no-warnings"),
        QStringLiteral("-J"),
    };
    arguments += cookieArguments();
    arguments << url;
    emit busyChanged();
    m_ytdlpProcess.start(m_ytdlp, arguments);
    if (!m_ytdlpProcess.waitForStarted(3000)) {
        m_currentRequest.clear();
        emit busyChanged();
        fail(tr("could not start yt-dlp; is it installed?"));
    }
}

void Search::handleFlatList(const QByteArray &json)
{
    const QVariantList rows = parseFlatList(json);
    if (rows.isEmpty()) {
        fail(tr("that list came back empty"));
        return;
    }
    emit resultsReady(rows, m_heading);
}
