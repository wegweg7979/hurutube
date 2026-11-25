#include <QtTest>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <functional>

#include "codec.h"
#include "cookies.h"
#include "history.h"
#include "restartprocess.h"
#include "search.h"

// Captures warnings so a test can assert that a fix is quiet when it should be.
namespace {
QStringList g_warnings;
void captureWarnings(QtMsgType type, const QMessageLogContext &, const QString &message)
{
    if (type == QtWarningMsg)
        g_warnings.append(message);
}
}  // namespace

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
    void cookiesAreCountedByName();
    void cookiesDescribeTellsTheTruth();
    void cookiesLadderHonoursExplicitConfig();
    void cookiesLadderDoesNotLetAVisitorFileShadowFirefox();
    void cookiesLadderInheritsAndEndsEmpty();
    void restartReplacesARunningProcess();
    void restartStartsDirectlyWhenNothingIsRunning();
    void restartDoesNotWarnWhenItCanJustStart();
    void codecConfigWordsAreRead();
    void codecAutoFollowsTheDriverNotTheModel();
    void codecOnlyDecodingEntrypointsCount();
    void codecSelectorAlwaysKeepsAWayToPlay();
    void codecCycleVisitsEveryChoiceAndReturns();
    void codecWordingMatchesTheDecision();
    void codecLegacyIntelDriverPrefersH264();

private:
    static QByteArray fixture(const QString &name)
    {
        QFile file(fixturePath(name));
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return file.readAll();
    }

    static QString fixturePath(const QString &name)
    {
        return QStringLiteral(HURUTUBE_FIXTURE_DIR "/") + name;
    }

    // A home directory the ladder can be driven against: every path it looks at
    // is relative to the home it is given, so the order can be exercised without
    // going near the real one.
    static void writeFile(const QString &path, const QString &contents)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            file.write(contents.toUtf8());
    }

    static void copyFixture(const QString &name, const QString &destination)
    {
        writeFile(destination, QString::fromUtf8(fixture(name)));
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

void HurutubeTests::cookiesAreCountedByName()
{
    // Both counts come from inside the file, and the `#HttpOnly_` rows are the
    // whole point: those are the real session cookies, so a reader that skips
    // every `#` line sees a signed-in export as an empty file.
    int cookies = 0;
    int sessions = 0;
    Cookies::count(fixturePath(QStringLiteral("cookies-visitor.txt")), &cookies, &sessions);
    QCOMPARE(cookies, 3);
    QCOMPARE(sessions, 0);

    Cookies::count(fixturePath(QStringLiteral("cookies-session.txt")), &cookies, &sessions);
    QCOMPARE(cookies, 4);
    QCOMPARE(sessions, 2);

    // A path that is not there is zero, not a crash and not a guess.
    Cookies::count(QStringLiteral("/nonexistent/cookies.txt"), &cookies, &sessions);
    QCOMPARE(cookies, 0);
    QCOMPARE(sessions, 0);
}

void HurutubeTests::cookiesDescribeTellsTheTruth()
{
    // Anonymous is a working state, and it says so rather than implying breakage.
    QVERIFY(Cookies::describe(QString(), QString()).contains(QStringLiteral("anonymous")));

    // A browser is named and nothing more: nothing here reads a browser's
    // database, so this wording must not claim to know whether it holds a session.
    QCOMPARE(Cookies::describe(QStringLiteral("firefox"), QStringLiteral("browser")),
             QStringLiteral("browser cookies: firefox"));

    // A file of visitor cookies must not read as signed in. That false claim is
    // exactly what this wording exists to prevent.
    const QString visitor =
        Cookies::describe(fixturePath(QStringLiteral("cookies-visitor.txt")), QStringLiteral("file"));
    QVERIFY(visitor.contains(QStringLiteral("no session")));
    QVERIFY(!visitor.startsWith(QStringLiteral("signed in")));

    // A real session says how many cookies and how many of them are the session.
    const QString live =
        Cookies::describe(fixturePath(QStringLiteral("cookies-session.txt")), QStringLiteral("file"));
    QVERIFY(live.startsWith(QStringLiteral("signed in")));
    QVERIFY(live.contains(QStringLiteral("4 cookies")));
    QVERIFY(live.contains(QStringLiteral("2 of them a session")));
}

void HurutubeTests::cookiesLadderHonoursExplicitConfig()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    const QString config = home.path() + QStringLiteral("/.config/hurutube/config.toml");

    writeFile(config, QStringLiteral("cookies_from = \"brave\"\n"));
    QString kind;
    QCOMPARE(Cookies::resolve(&kind, home.path()), QStringLiteral("brave"));
    QCOMPARE(kind, QStringLiteral("browser"));

    // A named file wins over a named browser: it is the more specific instruction,
    // and the session check is for sources the app found by itself.
    const QString named = home.path() + QStringLiteral("/exported.txt");
    writeFile(config,
              QStringLiteral("cookies_from = \"brave\"\ncookies_file = \"%1\"\n").arg(named));
    QCOMPARE(Cookies::resolve(&kind, home.path()), named);
    QCOMPARE(kind, QStringLiteral("file"));
}

void HurutubeTests::cookiesLadderDoesNotLetAVisitorFileShadowFirefox()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    const QString appCookies = home.path() + QStringLiteral("/.cache/hurutube/cookies.txt");

    // A cookies file is present but carries no session: it must not shadow a
    // browser that can actually serve a signed-in request.
    copyFixture(QStringLiteral("cookies-visitor.txt"), appCookies);
    writeFile(home.path() + QStringLiteral("/.mozilla/firefox/profiles.ini"),
              QStringLiteral("[Profile0]\nPath=abc.default\n"));
    QString kind;
    QCOMPARE(Cookies::resolve(&kind, home.path()), QStringLiteral("firefox"));
    QCOMPARE(kind, QStringLiteral("browser"));

    // Put a session in the same file and it wins: it is a working source, and
    // costs no browser lookup.
    copyFixture(QStringLiteral("cookies-session.txt"), appCookies);
    QCOMPARE(Cookies::resolve(&kind, home.path()), appCookies);
    QCOMPARE(kind, QStringLiteral("file"));
}

void HurutubeTests::cookiesLadderInheritsAndEndsEmpty()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());

    // Nothing of its own: the sibling player's setting is inherited instead of
    // being asked for a second time.
    writeFile(home.path() + QStringLiteral("/.config/huruwave/config.toml"),
              QStringLiteral("cookies_from = \"firefox\"\n"));
    QString kind;
    QCOMPARE(Cookies::resolve(&kind, home.path()), QStringLiteral("firefox"));
    QCOMPARE(kind, QStringLiteral("browser"));

    // A home with nothing in it resolves to nothing -- and clears `kind`, so a
    // caller cannot mistake a stale value for a source that was found.
    QTemporaryDir bare;
    QVERIFY(bare.isValid());
    kind = QStringLiteral("stale");
    QVERIFY(Cookies::resolve(&kind, bare.path()).isEmpty());
    QVERIFY(kind.isEmpty());
}

void HurutubeTests::restartReplacesARunningProcess()
{
    // The case presolve hits when you move through the list quickly: a resolve is
    // still running and must be replaced. Qt's own start() does nothing here --
    // it warns and drops the request -- so the helper stops the old one first.
    QProcess process;
    process.start(QStringLiteral("/usr/bin/sleep"), {QStringLiteral("30")});
    QVERIFY(process.waitForStarted(5000));
    QCOMPARE(process.state(), QProcess::Running);

    Restart::process(process, QStringLiteral("/usr/bin/echo"), {QStringLiteral("replacement")},
                     QStringLiteral("the test"));
    QTRY_COMPARE(process.state(), QProcess::Running);
    QVERIFY(process.waitForFinished(5000));
    QCOMPARE(process.exitCode(), 0);
    QVERIFY(QString::fromUtf8(process.readAllStandardOutput()).contains(QStringLiteral("replacement")));
}

void HurutubeTests::restartStartsDirectlyWhenNothingIsRunning()
{
    QProcess process;
    Restart::process(process, QStringLiteral("/usr/bin/echo"), {QStringLiteral("direct")},
                     QStringLiteral("the test"));
    QTRY_COMPARE(process.state(), QProcess::Running);
    QVERIFY(process.waitForFinished(5000));
    QCOMPARE(process.exitCode(), 0);
    QVERIFY(QString::fromUtf8(process.readAllStandardOutput()).contains(QStringLiteral("direct")));
}

void HurutubeTests::restartDoesNotWarnWhenItCanJustStart()
{
    // A recovery path that cries wolf is worse than Qt's own message, so the
    // ordinary case must be silent.
    g_warnings.clear();
    qInstallMessageHandler(captureWarnings);
    QProcess process;
    Restart::process(process, QStringLiteral("/usr/bin/echo"), {QStringLiteral("quiet")},
                     QStringLiteral("the test"));
    QVERIFY(process.waitForFinished(5000));
    qInstallMessageHandler(nullptr);
    QCOMPARE(g_warnings.size(), 0);
}

void HurutubeTests::codecConfigWordsAreRead()
{
    // config.toml is read-only, so a word the app does not understand can never be
    // corrected there -- and it must not be able to stop playback either.
    QCOMPARE(Codec::fromConfig(QStringLiteral("auto")), Codec::Preference::Auto);
    QCOMPARE(Codec::fromConfig(QStringLiteral("  AVC  ")), Codec::Preference::Avc);
    QCOMPARE(Codec::fromConfig(QStringLiteral("h264")), Codec::Preference::Avc);
    QCOMPARE(Codec::fromConfig(QStringLiteral("VP9")), Codec::Preference::Vp9);
    QCOMPARE(Codec::fromConfig(QStringLiteral("av1")), Codec::Preference::Av1);
    QCOMPARE(Codec::fromConfig(QString()), Codec::Preference::Auto);
    QCOMPARE(Codec::fromConfig(QStringLiteral("avc1")), Codec::Preference::Auto);  // not a word we defined
    QCOMPARE(Codec::fromConfig(QStringLiteral("hvec")), Codec::Preference::Auto);  // a misspelling
    QCOMPARE(Codec::name(Codec::Preference::Auto), QStringLiteral("auto"));
    QCOMPARE(Codec::name(Codec::Preference::Av1), QStringLiteral("av1"));
}

void HurutubeTests::codecAutoFollowsTheDriverNotTheModel()
{
    // The whole point of the feature: on a GPU that decodes H.264 and nothing newer,
    // `auto` must ask for H.264 instead of quietly handing mpv an AV1 stream to
    // decode on the CPU. Both fixtures are the driver's own answer -- one captured
    // here (Iris Xe), one reconstructed for a Haswell machine.
    const Codec::Capability haswell =
        Codec::fromVainfo(QString::fromUtf8(fixture(QStringLiteral("vainfo-haswell.txt"))));
    QVERIFY(haswell.h264);
    QVERIFY(!haswell.vp9);
    QVERIFY(!haswell.av1);
    QCOMPARE(haswell.driver, QStringLiteral("i965"));
    QCOMPARE(Codec::effective(Codec::Preference::Auto, haswell), Codec::Preference::Avc);

    const Codec::Capability iris =
        Codec::fromVainfo(QString::fromUtf8(fixture(QStringLiteral("vainfo-iris-xe.txt"))));
    QVERIFY(iris.h264);
    QVERIFY(iris.vp9);
    QVERIFY(iris.av1);
    QCOMPARE(iris.driver, QStringLiteral("iHD"));
    QCOMPARE(Codec::effective(Codec::Preference::Auto, iris), Codec::Preference::Auto);

    // No answer: restrict nothing, and do not pretend to know. This is the case the
    // app deliberately refuses to fill in from a CPU model number.
    const Codec::Capability unknown;
    QVERIFY(!unknown.any());
    QVERIFY(unknown.source.isEmpty());
    QCOMPARE(Codec::effective(Codec::Preference::Auto, unknown), Codec::Preference::Auto);

    // An explicit preference is the user's word, including where this GPU cannot
    // decode it in hardware -- overriding the app is allowed, being quiet about the
    // cost is not (the footer says "software").
    QCOMPARE(Codec::effective(Codec::Preference::Av1, haswell), Codec::Preference::Av1);
    QCOMPARE(Codec::effective(Codec::Preference::Vp9, unknown), Codec::Preference::Vp9);
}

void HurutubeTests::codecOnlyDecodingEntrypointsCount()
{
    // Haswell advertises H.264 for encoding as well as decoding. Reading "I can
    // encode this" as "I can decode this" would have the app claim a hardware path
    // that is not there.
    const Codec::Capability encodingOnly = Codec::fromVainfo(QStringLiteral(
        "      VAProfileH264High               : VAEntrypointEncSlice\n"
        "      VAProfileH264Main               : VAEntrypointEncSlice\n"
        "      VAProfileH264High               : VAEntrypointFEI\n"
        "      VAProfileVP9Profile0            : VAEntrypointEncSlice\n"));
    QVERIFY(!encodingOnly.any());
    QCOMPARE(Codec::effective(Codec::Preference::Auto, encodingOnly), Codec::Preference::Auto);
}

void HurutubeTests::codecSelectorAlwaysKeepsAWayToPlay()
{
    // A codec preference is a preference, never a demand: plenty of videos have no
    // stream in the wanted codec, and one must still play. Every expression ends in
    // the unfiltered fallback.
    const QString fallback = QStringLiteral("bestvideo[height<=1080]+bestaudio/best[height<=1080]");
    for (const Codec::Preference preference :
         {Codec::Preference::Auto, Codec::Preference::Avc, Codec::Preference::Vp9,
          Codec::Preference::Av1}) {
        const QString expression = Codec::selector(preference, false);
        QVERIFY2(expression.endsWith(fallback), qPrintable(expression));
        QCOMPARE(Codec::selector(preference, true), QStringLiteral("bestaudio/best"));
    }

    // The filters themselves, verbatim as measured against a real video: the plain
    // selection there resolves to av01 (399), H.264 to avc1.64002A (299) and VP9 to
    // vp9 (303). The regex forms are deliberate: `^vp0?9` also covers the `vp09.*`
    // spelling a future extraction may use.
    QVERIFY(Codec::selector(Codec::Preference::Avc, false).contains(QStringLiteral("vcodec~=\"^avc\"")));
    QVERIFY(Codec::selector(Codec::Preference::Vp9, false).contains(QStringLiteral("vcodec~=\"^vp0?9\"")));
    QVERIFY(Codec::selector(Codec::Preference::Av1, false).contains(QStringLiteral("vcodec~=\"^av0?1\"")));
    QVERIFY(!Codec::selector(Codec::Preference::Auto, false).contains(QStringLiteral("vcodec")));
}

void HurutubeTests::codecCycleVisitsEveryChoiceAndReturns()
{
    Codec::Preference preference = Codec::Preference::Auto;
    QStringList seen;
    for (int step = 0; step < 4; ++step) {
        preference = Codec::next(preference);
        seen << Codec::name(preference);
    }
    QCOMPARE(seen.join(QLatin1Char(' ')), QStringLiteral("avc vp9 av1 auto"));
}

void HurutubeTests::codecWordingMatchesTheDecision()
{
    const Codec::Capability haswell =
        Codec::fromVainfo(QString::fromUtf8(fixture(QStringLiteral("vainfo-haswell.txt"))));
    const Codec::Capability iris =
        Codec::fromVainfo(QString::fromUtf8(fixture(QStringLiteral("vainfo-iris-xe.txt"))));

    // The footer, in a few words.
    QCOMPARE(Codec::summary(Codec::Preference::Auto, haswell), QStringLiteral("auto → H.264"));
    QCOMPARE(Codec::summary(Codec::Preference::Avc, iris), QStringLiteral("H.264"));
    QCOMPARE(Codec::summary(Codec::Preference::Auto, iris), QStringLiteral("auto"));
    QCOMPARE(Codec::summary(Codec::Preference::Auto, Codec::Capability{}), QStringLiteral("auto"));
    // Forcing software is allowed, and said out loud rather than discovered as
    // dropped frames.
    QCOMPARE(Codec::summary(Codec::Preference::Av1, haswell), QStringLiteral("AV1 (software)"));

    // --status, with the reason.
    QVERIFY(Codec::describe(Codec::Preference::Auto, haswell).contains(QStringLiteral("CPU")));
    QVERIFY(Codec::describe(Codec::Preference::Auto, haswell).contains(QStringLiteral("H.264")));
    QVERIFY(Codec::describe(Codec::Preference::Auto, iris).contains(QStringLiteral("nothing to restrict")));
    QVERIFY(Codec::describe(Codec::Preference::Avc, iris).contains(QStringLiteral("asked for")));
    QVERIFY(Codec::describe(Codec::Preference::Av1, haswell).contains(QStringLiteral("decode on the CPU")));
    QVERIFY(Codec::describe(Codec::Preference::Auto, Codec::Capability{}).contains(QStringLiteral("no answer")));
}

void HurutubeTests::codecLegacyIntelDriverPrefersH264()
{
    // A pre-Skylake Intel GPU whose driver DOES advertise VP9. The VP9 the legacy
    // i965 driver offers those parts is hybrid -- part of the work stays on the CPU
    // -- so H.264 (fully accelerated there) is the better answer, which is what the
    // user reported for their Devil's Canyon. Without this rule the app would see
    // "VP9 available" and quietly hand the GPU a half-software stream.
    const Codec::Capability legacy =
        Codec::fromVainfo(QString::fromUtf8(fixture(QStringLiteral("vainfo-i965-vp9.txt"))));
    QCOMPARE(legacy.driver, QStringLiteral("i965"));
    QVERIFY(legacy.h264);
    QVERIFY(legacy.vp9);   // advertised, and still not what auto picks
    QVERIFY(!legacy.av1);
    QCOMPARE(Codec::effective(Codec::Preference::Auto, legacy), Codec::Preference::Avc);
    QVERIFY(Codec::selector(Codec::effective(Codec::Preference::Auto, legacy), false)
                .contains(QStringLiteral("vcodec~=\"^avc\"")));
    QCOMPARE(Codec::summary(Codec::Preference::Auto, legacy), QStringLiteral("auto → H.264"));

    // The reason printed must not claim the driver lacks VP9 -- it does not; it
    // just does not accelerate all of it. That distinction is the whole point.
    const QString why = Codec::describe(Codec::Preference::Auto, legacy);
    QVERIFY(why.contains(QStringLiteral("i965")));
    QVERIFY(why.contains(QStringLiteral("partly")));

    // Asking for VP9 anyway is allowed, and then said out loud as partial.
    QCOMPARE(Codec::effective(Codec::Preference::Vp9, legacy), Codec::Preference::Vp9);
    QVERIFY(Codec::describe(Codec::Preference::Vp9, legacy).contains(QStringLiteral("partly")));

    // And a modern driver must not be caught by that rule: iHD means full VP9/AV1,
    // so auto stays unrestricted.
    const Codec::Capability modern =
        Codec::fromVainfo(QString::fromUtf8(fixture(QStringLiteral("vainfo-iris-xe.txt"))));
    QCOMPARE(modern.driver, QStringLiteral("iHD"));
    QCOMPARE(Codec::effective(Codec::Preference::Auto, modern), Codec::Preference::Auto);

    // The same rule against the REAL payload from the user's Devil's Canyon, which
    // advertises no VP9 at all. H.264 again, but for the other stated reason: the
    // message must say the driver does not offer the newer codecs, not that it
    // accelerates them partly -- those are different machines and different reasons.
    const Codec::Capability canyon =
        Codec::fromVainfo(QString::fromUtf8(fixture(QStringLiteral("vainfo-devil-canyon.txt"))));
    QCOMPARE(canyon.driver, QStringLiteral("i965"));
    QVERIFY(canyon.h264);
    QVERIFY(!canyon.vp9);
    QVERIFY(!canyon.av1);
    QCOMPARE(Codec::effective(Codec::Preference::Auto, canyon), Codec::Preference::Avc);
    QCOMPARE(Codec::summary(Codec::Preference::Auto, canyon), QStringLiteral("auto → H.264"));
    const QString canyonWhy = Codec::describe(Codec::Preference::Auto, canyon);
    QVERIFY(canyonWhy.contains(QStringLiteral("H.264 only")));
    QVERIFY(!canyonWhy.contains(QStringLiteral("partly")));
}

QTEST_GUILESS_MAIN(HurutubeTests)

#include "hurutube_tests.moc"
