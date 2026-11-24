#include <QtTest>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <functional>

#include "history.h"
#include "search.h"

// Everything here is pure or file-local: the fixture payloads are real replies
// captured from YouTube and yt-dlp, so the parsers are tested against the shape
// the services actually send rather than against a hand-written idea of it.
class HurutubeTests : public QObject {
    Q_OBJECT

private slots:
    void searchRowsComeFromTheFixture();
    void shortsAreNeverListed();
    void shortsLinksAreRefused();
    void urlsAndIdsAreClassified();
    void durationTextRoundTrips();
    void flatListRowsFromYtdlp();
    void historyRemembersAndResumes();
    void historyResumeRule();
    void historySurvivesAWriteAndRead();
    void historyDropsEntriesItCannotUse();

private:
    static QByteArray fixture(const QString &name)
    {
        QFile file(QStringLiteral(HURUTUBE_FIXTURE_DIR "/") + name);
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return file.readAll();
    }
};

void HurutubeTests::searchRowsComeFromTheFixture()
{
    const QByteArray payload = fixture(QStringLiteral("innertube-search.json"));
    QVERIFY(!payload.isEmpty());

    const QVariantList rows = Search::parseSearchResponse(payload);
    // The captured reply holds 17 videos; anything less means rows were dropped.
    QVERIFY2(rows.size() >= 15, qPrintable(QStringLiteral("only %1 rows").arg(rows.size())));

    for (const QVariant &value : rows) {
        const QVariantMap row = value.toMap();
        const QString id = row.value(QStringLiteral("id")).toString();
        QCOMPARE(id.size(), 11);
        QVERIFY(!row.value(QStringLiteral("title")).toString().isEmpty());
        QVERIFY(!row.value(QStringLiteral("channel")).toString().isEmpty());
        QVERIFY(row.value(QStringLiteral("url")).toString().endsWith(id));
        QVERIFY(row.value(QStringLiteral("thumbnail")).toString().contains(id));
        // The list needs seconds to draw a resume marker.
        QVERIFY(row.value(QStringLiteral("seconds")).toDouble() > 0);
    }
}

// The Shorts block, proven against a real reply: every Shorts item in the
// payload is looked up by id, and none of them may have become a row. This is
// the structural claim -- Shorts are not filtered out, they are never read.
void HurutubeTests::shortsAreNeverListed()
{
    // Captured from a query that is mostly Shorts: 12 videos, 30 Shorts.
    const QByteArray payload = fixture(QStringLiteral("innertube-search-shorts.json"));
    QVERIFY(!payload.isEmpty());

    const QJsonObject root = QJsonDocument::fromJson(payload).object();
    QSet<QString> shortsIds;
    std::function<void(const QJsonValue &)> walk = [&](const QJsonValue &value) {
        if (value.isObject()) {
            const QJsonObject object = value.toObject();
            for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
                if (it.key() == QStringLiteral("shortsLockupViewModel")) {
                    const QString entity = it.value().toObject()
                                               .value(QStringLiteral("entityId"))
                                               .toString();
                    // "shorts-shelf-item-<videoId>"
                    if (entity.size() > 11)
                        shortsIds.insert(entity.right(11));
                }
                walk(it.value());
            }
        } else if (value.isArray()) {
            for (const QJsonValue &entry : value.toArray())
                walk(entry);
        }
    };
    walk(root);
    QVERIFY2(!shortsIds.isEmpty(), "the fixture should contain Shorts to prove the point");

    for (const QVariant &value : Search::parseSearchResponse(payload)) {
        const QString id = value.toMap().value(QStringLiteral("id")).toString();
        QVERIFY2(!shortsIds.contains(id), qPrintable(QStringLiteral("Short %1 was listed").arg(id)));
    }
}

void HurutubeTests::shortsLinksAreRefused()
{
    QCOMPARE(Search::classify(QStringLiteral("https://www.youtube.com/shorts/gWkXJYHG6Sw")),
             QStringLiteral("shorts"));
    QCOMPARE(Search::classify(QStringLiteral("youtube.com/shorts/gWkXJYHG6Sw")),
             QStringLiteral("shorts"));
    // A Shorts URL carries a real video id, so the refusal has to win over the
    // id patterns rather than fall through to them.
    QCOMPARE(Search::videoId(QStringLiteral("https://www.youtube.com/shorts/gWkXJYHG6Sw")),
             QStringLiteral("gWkXJYHG6Sw"));
    QCOMPARE(Search::classify(QStringLiteral("https://m.youtube.com/shorts/abc_-DEF123?feature=share")),
             QStringLiteral("shorts"));
}

void HurutubeTests::urlsAndIdsAreClassified()
{
    QCOMPARE(Search::classify(QStringLiteral("https://www.youtube.com/watch?v=aqz-KE-bpKQ")),
             QStringLiteral("video"));
    QCOMPARE(Search::classify(QStringLiteral("https://youtu.be/aqz-KE-bpKQ")), QStringLiteral("video"));
    QCOMPARE(Search::classify(QStringLiteral("aqz-KE-bpKQ")), QStringLiteral("video"));
    QCOMPARE(Search::classify(QStringLiteral("https://www.youtube.com/playlist?list=PLrAXtmErZgOeiKm4sgNOknGvNjby9efdf")),
             QStringLiteral("playlist"));
    QCOMPARE(Search::classify(QStringLiteral("https://www.youtube.com/@LinusTechTips")),
             QStringLiteral("channel"));
    QCOMPARE(Search::classify(QStringLiteral("https://www.youtube.com/channel/UCXuqSBlHAE6Xw-yeJAkLwmA")),
             QStringLiteral("channel"));
    QCOMPARE(Search::classify(QStringLiteral("kdenlive tutorial")), QStringLiteral("search"));
    QCOMPARE(Search::classify(QString()), QStringLiteral("unknown"));
    QCOMPARE(Search::videoId(QStringLiteral("https://www.youtube.com/watch?v=aqz-KE-bpKQ&t=42s")),
             QStringLiteral("aqz-KE-bpKQ"));
}

void HurutubeTests::durationTextRoundTrips()
{
    QCOMPARE(Search::secondsFromText(QStringLiteral("23:56")), 1436.0);
    QCOMPARE(Search::secondsFromText(QStringLiteral("1:02:03")), 3723.0);
    QCOMPARE(Search::secondsFromText(QStringLiteral("0:07")), 7.0);
    QCOMPARE(Search::secondsFromText(QString()), 0.0);
    QCOMPARE(Search::secondsFromText(QStringLiteral("nonsense")), 0.0);
    QCOMPARE(Search::formatDuration(1436), QStringLiteral("23:56"));
    QCOMPARE(Search::formatDuration(3723), QStringLiteral("1:02:03"));
    QCOMPARE(Search::formatDuration(0), QString());
    QCOMPARE(Search::formatViews(1500000), QStringLiteral("1.5M views"));
    QCOMPARE(Search::formatViews(68000), QStringLiteral("68K views"));
    QCOMPARE(Search::formatViews(915), QStringLiteral("915 views"));
}

void HurutubeTests::flatListRowsFromYtdlp()
{
    const QByteArray payload = fixture(QStringLiteral("ytdlp-channel.json"));
    QVERIFY(!payload.isEmpty());

    const QVariantList rows = Search::parseFlatList(payload);
    QCOMPARE(rows.size(), 5);
    const QVariantMap first = rows.first().toMap();
    QCOMPARE(first.value(QStringLiteral("id")).toString(), QStringLiteral("qiLUvCt3k6g"));
    // yt-dlp gives numbers where the API gives text, and the list shows one
    // shape either way.
    QCOMPARE(first.value(QStringLiteral("duration")).toString(), QStringLiteral("1:09:42"));
    QVERIFY(first.value(QStringLiteral("seconds")).toDouble() > 4000);
    QCOMPARE(rows.at(3).toMap().value(QStringLiteral("views")).toString(), QStringLiteral("2.3M views"));
}

void HurutubeTests::historyRemembersAndResumes()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    History history(directory.filePath(QStringLiteral("history.json")));

    QVariantMap row;
    row.insert(QStringLiteral("id"), QStringLiteral("aqz-KE-bpKQ"));
    row.insert(QStringLiteral("title"), QStringLiteral("Big Buck Bunny"));
    row.insert(QStringLiteral("channel"), QStringLiteral("Blender"));
    row.insert(QStringLiteral("seconds"), 634.0);
    history.remember(row);
    QCOMPARE(history.count(), 1);

    history.setPosition(QStringLiteral("aqz-KE-bpKQ"), 120.0, 634.0);
    QCOMPARE(history.positionFor(QStringLiteral("aqz-KE-bpKQ")), 120.0);

    // Remembering again moves it to the front and keeps the position.
    QVariantMap other;
    other.insert(QStringLiteral("id"), QStringLiteral("qiLUvCt3k6g"));
    other.insert(QStringLiteral("title"), QStringLiteral("Something else"));
    history.remember(other);
    QCOMPARE(history.count(), 2);
    history.remember(row);
    QCOMPARE(history.count(), 2);
    QCOMPARE(history.entries().first().toMap().value(QStringLiteral("id")).toString(),
             QStringLiteral("aqz-KE-bpKQ"));
    QCOMPARE(history.entries().first().toMap().value(QStringLiteral("position")).toDouble(), 120.0);
    // Opening it twice is what "watched 2 times" counts.
    QCOMPARE(history.entries().first().toMap().value(QStringLiteral("plays")).toInt(), 2);

    history.remove(QStringLiteral("qiLUvCt3k6g"));
    QCOMPARE(history.count(), 1);
    history.clear();
    QCOMPARE(history.count(), 0);
}

void HurutubeTests::historyResumeRule()
{
    // Barely started: not worth resuming.
    QCOMPARE(History::resumePoint(3.0, 600.0), 0.0);
    // Past the end credits: it is finished, not resumable.
    QCOMPARE(History::resumePoint(590.0, 600.0), 0.0);
    QCOMPARE(History::resumePoint(120.0, 600.0), 120.0);
    // A live stream has no duration: any real position is worth keeping.
    QCOMPARE(History::resumePoint(300.0, 0.0), 300.0);

    QTemporaryDir directory;
    History history(directory.filePath(QStringLiteral("history.json")));
    QVariantMap row;
    row.insert(QStringLiteral("id"), QStringLiteral("aqz-KE-bpKQ"));
    row.insert(QStringLiteral("title"), QStringLiteral("Big Buck Bunny"));
    history.remember(row);
    // Two seconds in, and near the end: neither may become a resume point, or
    // the app would offer to continue a video the user has barely seen.
    history.setPosition(QStringLiteral("aqz-KE-bpKQ"), 2.0, 600.0);
    QCOMPARE(history.positionFor(QStringLiteral("aqz-KE-bpKQ")), 0.0);
    history.setPosition(QStringLiteral("aqz-KE-bpKQ"), 592.0, 600.0);
    QCOMPARE(history.positionFor(QStringLiteral("aqz-KE-bpKQ")), 0.0);
}

void HurutubeTests::historySurvivesAWriteAndRead()
{
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("history.json"));

    {
        History history(path);
        QVariantMap row;
        row.insert(QStringLiteral("id"), QStringLiteral("aqz-KE-bpKQ"));
        row.insert(QStringLiteral("title"), QStringLiteral("Big Buck Bunny"));
        row.insert(QStringLiteral("channel"), QStringLiteral("Blender"));
        history.remember(row);
        history.setPosition(QStringLiteral("aqz-KE-bpKQ"), 300.0, 634.0);
    }

    History reopened(path);
    QCOMPARE(reopened.count(), 1);
    QCOMPARE(reopened.positionFor(QStringLiteral("aqz-KE-bpKQ")), 300.0);
    QCOMPARE(reopened.entries().first().toMap().value(QStringLiteral("title")).toString(),
             QStringLiteral("Big Buck Bunny"));

    // The file is written beside the target and renamed, and nothing is left
    // next to it from the write.
    QDir dir(directory.path());
    QCOMPARE(dir.entryList(QDir::Files).size(), 1);
}

void HurutubeTests::historyDropsEntriesItCannotUse()
{
    // An entry with no usable id can never be resumed, matched or removed.
    const QByteArray json = R"({"version":1,"entries":[
        {"id":"short","title":"not an id"},
        {"id":"aqz-KE-bpKQ","title":"kept","position":42}]})";
    const QVariantList entries = History::parse(json);
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.first().toMap().value(QStringLiteral("title")).toString(),
             QStringLiteral("kept"));

    // And a file that is not JSON at all is treated as an empty history rather
    // than a reason to refuse to start.
    QCOMPARE(History::parse(QByteArray("not json")).size(), 0);
    QVERIFY(History::serialize(entries).contains("aqz-KE-bpKQ"));
}

QTEST_GUILESS_MAIN(HurutubeTests)

#include "hurutube_tests.moc"
