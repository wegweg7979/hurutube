#pragma once

#include <QString>

// hurutube's own settings: ~/.config/hurutube/config.toml, Omarchy-style flat
// `key = value` pairs with optional quotes.
//
// Read-only on purpose. The settings it holds are `cookies_file` and
// `cookies_from`, which are credentials-adjacent, so nothing here writes: a
// typo in a path must never be able to rewrite a line of that file.
namespace Config {

// ~/.config/hurutube/config.toml
QString path();

// The value for `key`, with surrounding quotes removed and any trailing comment
// dropped. Empty when the file or the key is missing. Never logs the value: this
// same file holds cookie settings.
QString read(const QString &key);

// The same read, against an arbitrary file -- used for another player's config,
// which is where a cookie setting can be inherited from when this app has none
// of its own.
QString readFrom(const QString &filePath, const QString &key);

}  // namespace Config
