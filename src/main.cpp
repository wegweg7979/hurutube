// hurutube — a small YouTube player: search, watch, and nothing that gets in
// the way. Video plays through mpv (VA-API), so the window is a browser and a
// remote control rather than a video surface.

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QTextStream>
#include <QTimer>

#include <csignal>
#include <functional>

#include "config.h"
#include "history.h"
#include "player.h"
#include "search.h"
#include "theme.h"

namespace {

// A SIGTERM (a session shutdown, or a script stopping the app) otherwise ends
// the process without running a single destructor, so the playback engine
// outlives the app still holding its IPC socket -- measured: two orphaned mpv
// processes after a few test runs. Quitting through the event loop instead lets
// Player's destructor stop the engine. (Player also clears leftovers at startup,
// because SIGKILL and a crash cannot be caught at all.)
void requestQuit(int)
{
    QCoreApplication::quit();
}

// Headless runs, so every path (network, parsing, refusal rules) can be driven
// from a shell and checked without a display. Exits 0 or 1 with the error on
// stderr -- the pattern the user's other apps use.
int runHeadless(Search &search, const std::function<void(Search &)> &launch, bool asJson)
{
    int exitCode = 1;
    QObject::connect(&search, &Search::resultsReady, &search,
                     [&](const QVariantList &entries, const QString &heading) {
                         if (asJson) {
                             QJsonArray array;
                             for (const QVariant &entry : entries)
                                 array.append(QJsonObject::fromVariantMap(entry.toMap()));
                             QJsonObject root;
                             root.insert(QStringLiteral("heading"), heading);
                             root.insert(QStringLiteral("entries"), array);
                             QTextStream(stdout) << QJsonDocument(root).toJson(QJsonDocument::Compact)
                                                 << Qt::endl;
                         } else {
                             QTextStream(stdout) << heading << Qt::endl;
                             for (const QVariant &entry : entries) {
                                 const QVariantMap row = entry.toMap();
                                 QTextStream(stdout)
                                     << row.value(QStringLiteral("id")).toString() << "  "
                                     << row.value(QStringLiteral("duration")).toString().leftJustified(8)
                                     << row.value(QStringLiteral("channel")).toString().leftJustified(24)
                                     << row.value(QStringLiteral("title")).toString() << Qt::endl;
                             }
                         }
                         exitCode = 0;
                         QCoreApplication::quit();
                     });
    QObject::connect(&search, &Search::videoReady, &search, [&](const QVariantMap &entry) {
        if (asJson)
            QTextStream(stdout) << QJsonDocument(QJsonObject::fromVariantMap(entry)).toJson(
                                       QJsonDocument::Compact)
                                << Qt::endl;
        else
            QTextStream(stdout) << entry.value(QStringLiteral("url")).toString() << Qt::endl;
        exitCode = 0;
        QCoreApplication::quit();
    });
    QObject::connect(&search, &Search::shortsRefused, &search, [&](const QString &id) {
        QTextStream(stderr) << "error: Shorts are blocked by design"
                            << (id.isEmpty() ? QString() : QStringLiteral(" (/shorts/%1)").arg(id))
                            << Qt::endl;
        exitCode = 1;
        QCoreApplication::quit();
    });
    QObject::connect(&search, &Search::failed, &search, [&](const QString &message) {
        QTextStream(stderr) << "error: " << message << Qt::endl;
        exitCode = 1;
        QCoreApplication::quit();
    });

    // Ask once the event loop is running, so the reply has somewhere to land.
    QTimer::singleShot(0, &search, [&search, &launch] { launch(search); });
    QCoreApplication::exec();
    return exitCode;
}

void printHistory(const History &history, bool asJson)
{
    const QVariantList entries = history.entries();
    if (asJson) {
        QJsonArray array;
        for (const QVariant &entry : entries)
            array.append(QJsonObject::fromVariantMap(entry.toMap()));
        QTextStream(stdout) << QJsonDocument(array).toJson(QJsonDocument::Compact) << Qt::endl;
        return;
    }
    if (entries.isEmpty())
        QTextStream(stdout) << "(nothing watched yet)" << Qt::endl;
    for (const QVariant &value : entries) {
        const QVariantMap row = value.toMap();
        QTextStream(stdout) << row.value(QStringLiteral("id")).toString() << "  "
                            << Search::formatDuration(row.value(QStringLiteral("position")).toDouble())
                                   .leftJustified(8)
                            << row.value(QStringLiteral("channel")).toString().leftJustified(24)
                            << row.value(QStringLiteral("title")).toString() << Qt::endl;
    }
}

// What an agent needs to confirm hardware playback is possible on this machine,
// and what the app would actually use.
int printStatus(const QString &mpv, const QString &ytdlp)
{
    auto firstLine = [](const QString &program, const QStringList &arguments) {
        QProcess process;
        process.start(program, arguments);
        if (!process.waitForFinished(3000))
            return QStringLiteral("(no answer)");
        const QString output = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
        return output.section(QLatin1Char('\n'), 0, 0);
    };
    const QString renderNode = [] {
        QDir dri(QStringLiteral("/dev/dri"));
        for (const QString &name : dri.entryList({QStringLiteral("renderD*")}, QDir::System))
            return QStringLiteral("/dev/dri/") + name;
        return QStringLiteral("(none)");
    }();

    QTextStream(stdout) << "mpv:        " << firstLine(mpv, {QStringLiteral("--version")}) << Qt::endl
                        << "yt-dlp:     " << firstLine(ytdlp, {QStringLiteral("--version")}) << Qt::endl
                        << "render:     " << renderNode << Qt::endl;

    // The VA-API profiles that decide whether the streams YouTube serves can be
    // decoded on the GPU instead of the CPU.
    QProcess vainfo;
    vainfo.start(QStringLiteral("vainfo"), {});
    QString profiles;
    if (vainfo.waitForFinished(4000)) {
        const QStringList lines =
            QString::fromUtf8(vainfo.readAllStandardOutput()).split(QLatin1Char('\n'));
        for (const QString &line : lines) {
            const QString trimmed = line.trimmed();
            if (!trimmed.contains(QStringLiteral("VAEntrypointVLD")))
                continue;
            if (trimmed.contains(QStringLiteral("VAProfileH264High"))
                || trimmed.contains(QStringLiteral("VAProfileVP9Profile0"))
                || trimmed.contains(QStringLiteral("VAProfileAV1Profile0")))
                profiles += trimmed.section(QLatin1Char(':'), 0, 0).trimmed() + QLatin1Char(' ');
        }
    }
    QTextStream(stdout) << "vaapi-vld:  "
                        << (profiles.isEmpty() ? QStringLiteral("(none found — software decode)")
                                               : profiles.trimmed())
                        << Qt::endl;
    QTextStream(stdout) << "note:       the app reports the codec it is really using in its "
                           "footer (hwdec-current), never an assumption"
                        << Qt::endl;
    return renderNode == QStringLiteral("(none)") ? 1 : 0;
}

}  // namespace

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("hurutube"));
    std::signal(SIGTERM, requestQuit);
    std::signal(SIGINT, requestQuit);
    app.setApplicationDisplayName(QStringLiteral("hurutube"));
    app.setOrganizationName(QStringLiteral("hurunagus"));
    // Ties the Wayland app_id to hurutube.desktop so the compositor and the
    // launcher find the right icon and window rules.
    app.setDesktopFileName(QStringLiteral("hurutube"));
    app.setWindowIcon(QIcon::fromTheme(QStringLiteral("hurutube")));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("A small YouTube player. Video plays in mpv; Shorts are refused."));
    parser.addHelpOption();
    QCommandLineOption searchOption({QStringLiteral("s"), QStringLiteral("search")},
                                    QStringLiteral("search and print the results"), QStringLiteral("query"));
    QCommandLineOption jsonOption(QStringLiteral("json"), QStringLiteral("with --search/--history: print JSON"));
    QCommandLineOption noWindowOption(QStringLiteral("no-window"),
                                      QStringLiteral("do not open the window"));
    QCommandLineOption historyOption(QStringLiteral("history"),
                                     QStringLiteral("print what has been watched, then exit"));
    QCommandLineOption statusOption(QStringLiteral("status"),
                                    QStringLiteral("print the playback stack on this machine, then exit"));
    QCommandLineOption audioOnlyOption(QStringLiteral("audio-only"),
                                       QStringLiteral("start in audio-only mode (no video window)"));
    parser.addOption(searchOption);
    parser.addOption(jsonOption);
    parser.addOption(noWindowOption);
    parser.addOption(historyOption);
    parser.addOption(statusOption);
    parser.addOption(audioOnlyOption);
    parser.addPositionalArgument(QStringLiteral("what"),
                                 QStringLiteral("a YouTube URL or id to play, a playlist/channel URL, "
                                                "or a search phrase"));
    parser.process(app);

    if (parser.isSet(statusOption))
        return printStatus(QStringLiteral("mpv"), QStringLiteral("yt-dlp"));

    History history;
    if (parser.isSet(historyOption) && parser.isSet(noWindowOption)) {
        printHistory(history, parser.isSet(jsonOption));
        return 0;
    }

    Theme theme;
    Search youtube;
    Player player;

    // One cookie source, given to both: the inner API does not need cookies, but
    // yt-dlp does for restricted videos, and mpv resolves through yt-dlp when it
    // has to. Configured in ~/.config/hurutube/config.toml.
    const QString cookiesFrom = Config::read(QStringLiteral("cookies_from"));
    const QString cookiesFile = Config::read(QStringLiteral("cookies_file"));
    youtube.setCookies(cookiesFile, cookiesFrom);
    player.setCookieSource(cookiesFile, cookiesFrom);

    const QString query = parser.value(searchOption).trimmed();
    const bool asJson = parser.isSet(jsonOption);
    if (!query.isEmpty() && parser.isSet(noWindowOption)) {
        const QString wanted = query;
        return runHeadless(youtube, [wanted](Search &s) { s.open(wanted); }, asJson);
    }

    const QStringList positional = parser.positionalArguments();
    const QString argument = positional.isEmpty() ? QString() : positional.first();
    const QString kind = Search::classify(argument);
    const bool argumentIsSearch = !argument.isEmpty() && kind == QStringLiteral("search");

    if (!argument.isEmpty() && parser.isSet(noWindowOption)) {
        const QString wanted = argument;
        return runHeadless(youtube, [wanted](Search &s) { s.open(wanted); }, asJson);
    }

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("player"), &player);
    engine.rootContext()->setContextProperty(QStringLiteral("youtube"), &youtube);
    engine.rootContext()->setContextProperty(QStringLiteral("history"), &history);
    engine.rootContext()->setContextProperty(QStringLiteral("initialQuery"),
                                             argumentIsSearch ? argument : QString());
    engine.rootContext()->setContextProperty(QStringLiteral("initialUrl"),
                                             argumentIsSearch ? QString() : argument);
    engine.rootContext()->setContextProperty(QStringLiteral("startAudioOnly"),
                                             parser.isSet(audioOnlyOption));

    // Loaded through the QML module, not by a qrc:/ URL: the module's files are
    // compiled at build time, which keeps a stale per-user QML cache from serving
    // the previous build's UI.
    engine.loadFromModule("hurutube", "Main");
    if (engine.rootObjects().isEmpty())
        return 1;

    return app.exec();
}
