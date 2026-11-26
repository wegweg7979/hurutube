#include "resolves.h"

#include <algorithm>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

Resolves::Resolves(const QString &filePath)
    : m_path(filePath.isEmpty()
                 // GenericStateLocation, like history.json: everything about this
                 // app lives flat under its own name, not under an organisation.
                 ? QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation)
                       + QStringLiteral("/hurutube/resolves.json")
                 : filePath)
{
    load();
}

bool Resolves::fresh(const QString &url, Entry *out) const
{
    const auto it = m_entries.constFind(url);
    if (it == m_entries.constEnd())
        return false;
    if (!it->fresh(QDateTime::currentSecsSinceEpoch()))
        return false;
    if (out)
        *out = *it;
    return true;
}

void Resolves::insert(const QString &url, const Entry &entry)
{
    if (url.isEmpty() || entry.video.isEmpty())
        return;
    m_entries.insert(url, entry);
    save();
}

void Resolves::forget(const QString &url)
{
    if (m_entries.remove(url) > 0)
        save();
}

void Resolves::clear()
{
    if (m_entries.isEmpty())
        return;
    m_entries.clear();
    save();
}

void Resolves::load()
{
    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly))
        return;

    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    // A corrupt file is not worth a warning: it holds only cached URLs, and the
    // cost of ignoring it is one resolve.
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return;

    const QJsonObject root = document.object();
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
        const QJsonObject value = it.value().toObject();
        Entry entry;
        entry.video = value.value(QStringLiteral("video")).toString();
        entry.audio = value.value(QStringLiteral("audio")).toString();
        entry.stamp = static_cast<qint64>(value.value(QStringLiteral("stamp")).toDouble());
        entry.cookies = value.value(QStringLiteral("cookies")).toBool();
        // Expired entries are dropped on load rather than kept and filtered later:
        // a URL that old is not going to become useful again.
        if (entry.video.isEmpty() || !entry.fresh(now))
            continue;
        m_entries.insert(it.key(), entry);
    }
}

void Resolves::save() const
{
    QDir().mkpath(QFileInfo(m_path).absolutePath());

    // Keep the newest kMaxEntries, the same reasoning as history's cap: the oldest
    // are the least likely to be played again, and every entry costs a write.
    QList<QPair<QString, Entry>> entries;
    entries.reserve(m_entries.size());
    for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it)
        entries.append({it.key(), it.value()});
    std::sort(entries.begin(), entries.end(),
              [](const QPair<QString, Entry> &a, const QPair<QString, Entry> &b) {
                  return a.second.stamp > b.second.stamp;
              });

    QJsonObject root;
    for (int i = 0; i < entries.size() && i < kMaxEntries; ++i) {
        const Entry &entry = entries.at(i).second;
        QJsonObject value;
        value.insert(QStringLiteral("video"), entry.video);
        if (!entry.audio.isEmpty())
            value.insert(QStringLiteral("audio"), entry.audio);
        value.insert(QStringLiteral("stamp"), double(entry.stamp));
        if (entry.cookies)
            value.insert(QStringLiteral("cookies"), true);
        root.insert(entries.at(i).first, value);
    }

    // QSaveFile writes beside the target and renames, so an interrupted write
    // leaves the previous cache intact instead of half a file.
    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning("resolves: cannot write %s", qPrintable(m_path));
        return;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    file.commit();
}
