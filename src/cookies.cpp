#include "cookies.h"

#include "config.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QStringList>

namespace {

// A cookies file this app reads if it is there, next to huruwave's
// (~/.cache/huruwave/cookies.txt). Nothing here writes it -- exporting cookies
// is the user's own move -- and a file that exists is not the same thing as a
// session, which is why the ladder checks what is inside it.
QString cookiesPath()
{
    return QStringLiteral("/.cache/hurutube/cookies.txt");
}

// The cookies that mean "this is a session", by NAME only: a value never leaves
// the file it lives in.
bool isSessionCookie(const QString &name)
{
    static const QStringList sessionNames{
        QStringLiteral("SID"),          QStringLiteral("SSID"),
        QStringLiteral("HSID"),         QStringLiteral("SAPISID"),
        QStringLiteral("__Secure-1PSID"), QStringLiteral("__Secure-3PSID"),
    };
    return sessionNames.contains(name);
}

// Firefox, because its cookies are a plain sqlite file: none of the keyring
// failures that make the Chromium path fragile can apply, which is exactly why
// it sits above the session-less cookies file.
bool firefoxProfileExists(const QString &home)
{
    const QStringList roots{
        home + QStringLiteral("/.mozilla/firefox"),
        home + QStringLiteral("/.var/app/org.mozilla.firefox/config/mozilla/firefox"),
        home + QStringLiteral("/.var/app/org.mozilla.firefox/.mozilla/firefox"),
    };
    for (const QString &root : roots) {
        QDir directory(root);
        if (!directory.exists())
            continue;
        if (QFileInfo::exists(root + QStringLiteral("/profiles.ini")))
            return true;
        // A profile directory is recognisable by its cookie database.
        const QStringList entries = directory.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &entry : entries) {
            if (QFileInfo::exists(root + QLatin1Char('/') + entry
                                  + QStringLiteral("/cookies.sqlite")))
                return true;
        }
    }
    return false;
}

}  // namespace

namespace Cookies {

void count(const QString &path, int *cookies, int *sessions)
{
    if (cookies)
        *cookies = 0;
    if (sessions)
        *sessions = 0;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return;

    int total = 0;
    int session = 0;
    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine());
        // `#HttpOnly_` rows are real cookies, not comments -- which matters
        // because the __Secure-*PSID session cookies are exactly the HttpOnly
        // ones, so skipping every `#` line would read a live session as empty.
        if (line.startsWith(QLatin1Char('#')) && !line.startsWith(QStringLiteral("#HttpOnly_")))
            continue;
        const QStringList fields = line.trimmed().split(QLatin1Char('\t'));
        if (fields.size() < 7)
            continue;
        ++total;
        if (isSessionCookie(fields.at(5)))
            ++session;
    }
    if (cookies)
        *cookies = total;
    if (sessions)
        *sessions = session;
}

QString resolve(QString *kind, const QString &home)
{
    const QString base = home.isEmpty() ? QDir::homePath() : home;
    const QString ownConfig = base + QStringLiteral("/.config/hurutube/config.toml");
    const QString file = base + cookiesPath();

    auto answer = [kind](const QString &source, const QString &what) {
        if (kind)
            *kind = QString(what);
        return source;
    };

    // 1. This app's own config, both forms. An explicit instruction is honoured
    //    whatever it holds -- the session check below is for sources this app
    //    found by itself, not for one the user named.
    const QString namedFile = Config::readFrom(ownConfig, QStringLiteral("cookies_file"));
    if (!namedFile.isEmpty())
        return answer(namedFile, QStringLiteral("file"));
    const QString namedBrowser = Config::readFrom(ownConfig, QStringLiteral("cookies_from"));
    if (!namedBrowser.isEmpty())
        return answer(namedBrowser, QStringLiteral("browser"));

    // 2. This app's own cookies file, but ONLY when it really carries a session:
    //    a file of visitor cookies must not shadow a working browser. Measured on
    //    the machine this was written on: an export held cookies with every SID
    //    dropped, and taking such a file on faith is what lets an app say
    //    "signed in" while every restricted video still fails.
    if (QFileInfo::exists(file)) {
        int cookies = 0;
        int sessions = 0;
        count(file, &cookies, &sessions);
        if (sessions > 0)
            return answer(file, QStringLiteral("file"));
    }

    // 3. Firefox.
    if (firefoxProfileExists(base))
        return answer(QStringLiteral("firefox"), QStringLiteral("browser"));

    // 4. The session-less cookies file, when it is all there is: anonymous
    //    browsing is the same either way, and the status line says which it is.
    if (QFileInfo::exists(file))
        return answer(file, QStringLiteral("file"));

    // 5. Another player's setting, so an existing setup is inherited instead of
    //    configured twice. huruwave first: it is the sibling app on this desktop.
    const QStringList others{
        base + QStringLiteral("/.config/huruwave/config.toml"),
        base + QStringLiteral("/.config/cliamp/config.toml"),
    };
    for (const QString &other : others) {
        const QString inherited = Config::readFrom(other, QStringLiteral("cookies_from"));
        if (!inherited.isEmpty())
            return answer(inherited, QStringLiteral("browser"));
    }

    if (kind)
        kind->clear();
    return {};
}

QString describe(const QString &source, const QString &kind)
{
    if (source.isEmpty())
        return QObject::tr("no cookies — anonymous search only");

    if (kind != QStringLiteral("file"))
        return QObject::tr("browser cookies: %1").arg(source);

    int cookies = 0;
    int sessions = 0;
    count(source, &cookies, &sessions);
    if (cookies == 0)
        return QObject::tr("cookie file is empty: %1").arg(source);
    if (sessions == 0)
        return QObject::tr("cookie file: %1 cookies, no session — age-gated videos will fail")
            .arg(cookies);
    return QObject::tr("signed in — cookie file, %1 cookies, %2 of them a session")
        .arg(cookies)
        .arg(sessions);
}

}  // namespace Cookies
