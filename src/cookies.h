#pragma once

#include <QString>

// Where the YouTube session comes from, and what it can actually do.
//
// hurutube needs no account to search: the inner API it reads is anonymous. But
// yt-dlp needs cookies for everything else restricted -- age-gated videos,
// members-only content, and the cases where YouTube answers an anonymous client
// with "Sign in to confirm you're not a bot" -- so the source is resolved once,
// at startup, and handed to both engines.
//
// The ladder is huruwave's, in the same order and for the same reasons: an
// explicit setting wins, then a cookies file that really carries a session, then
// Firefox (a plain sqlite file, with no keyring to fail), then a session-less
// cookies file, then whatever another player on this machine is already using.
namespace Cookies {

// The resolved source, ready to use: either a browser name for yt-dlp's
// `--cookies-from-browser`, or the path of a Netscape cookies file for
// `--cookies`. `kind` comes back as "browser", "file", or empty when nothing
// was found (which means anonymous, not broken).
//
// `home` exists for the tests: everything in the ladder is resolved relative to
// it, so the order can be exercised without touching a real home directory.
// Empty means the real one.
QString resolve(QString *kind, const QString &home = QString());

// What the source can actually do, phrased for a human. A cookies file that
// holds no session cookie is NOT a signed-in session, and saying so is the entire
// point: huruwave once printed "signed in" while every SID had been dropped from
// its export. Counts and cookie NAMES only -- never a cookie value.
QString describe(const QString &source, const QString &kind);

// Cookie rows and session rows in a Netscape cookies file.
void count(const QString &path, int *cookies, int *sessions);

}  // namespace Cookies
