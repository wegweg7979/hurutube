#include "history.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

namespace {

// Beyond a few hundred the file is a liability rather than a feature; nobody
// scrolls that far, and every entry costs a write.
constexpr int kMaxEntries = 300;

// Below this the user barely started, above (duration - this) they finished.
// Resuming either would be an annoyance dressed up as a feature.
constexpr double kMinResumeSeconds = 15.0;
constexpr double kEndCreditSeconds = 30.0;

}  // namespace

History::History(const QString &filePath, QObject *parent)
    : QObject(parent),
      m_path(filePath.isEmpty()
                 // GenericStateLocation, not StateLocation: the latter inserts
                 // the organization name, giving ~/.local/state/hurunagus/
                 // hurutube/history.json, while everything else about this app
                 // (its config, its engine socket) is flat under the app name.
                 ? QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation)
                       + QStringLiteral("/hurutube/history.json")
                 : filePath)
{
    load();
}

double History::resumePoint(double position, double duration)
{
    if (position < kMinResumeSeconds)
        return 0;
    if (duration > 0 && position > duration - kEndCreditSeconds)
        return 0;
    return position;
}

QVariantList History::parse(const QByteArray &json)
{
    const QJsonObject root = QJsonDocument::fromJson(json).object();
    const QJsonArray array = root.value(QStringLiteral("entries")).toArray();
    QVariantList entries;
    for (const QJsonValue &value : array) {
        const QVariantMap row = value.toObject().toVariantMap();
        // An entry with no id cannot be resumed, matched, or removed: drop it
        // rather than carry a row the rest of the file cannot refer to.
        if (row.value(QStringLiteral("id")).toString().size() != 11)
            continue;
        entries.append(row);
    }
    return entries;
}

QByteArray History::serialize(const QVariantList &entries)
{
    QJsonArray array;
    for (const QVariant &value : entries)
        array.append(QJsonObject::fromVariantMap(value.toMap()));
    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("entries"), array);
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

void History::load()
{
    QFile file(m_path);
    if (!file.exists())
        return;
    if (!file.open(QIODevice::ReadOnly)) {
        // A history that cannot be read is not worth refusing to start over.
        return;
    }
    m_entries = parse(file.readAll());
    emit changed();
}

void History::save()
{
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    // QSaveFile writes beside the target and renames, so an interrupted write
    // leaves the previous history intact instead of half a file.
    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning("history: cannot write %s", qPrintable(m_path));
        return;
    }
    file.write(serialize(m_entries));
    if (!file.commit())
        qWarning("history: cannot commit %s", qPrintable(m_path));
}

void History::remember(const QVariantMap &row)
{
    const QString id = row.value(QStringLiteral("id")).toString();
    if (id.size() != 11)
        return;

    QVariantMap entry;
    entry.insert(QStringLiteral("id"), id);
    entry.insert(QStringLiteral("title"), row.value(QStringLiteral("title")).toString());
    entry.insert(QStringLiteral("channel"), row.value(QStringLiteral("channel")).toString());
    entry.insert(QStringLiteral("duration"), row.value(QStringLiteral("duration")));
    entry.insert(QStringLiteral("seconds"), row.value(QStringLiteral("seconds")));
    entry.insert(QStringLiteral("updated"), QDateTime::currentSecsSinceEpoch());

    double position = 0;
    int plays = 0;
    for (int i = 0; i < m_entries.size(); ++i) {
        const QVariantMap existing = m_entries.at(i).toMap();
        if (existing.value(QStringLiteral("id")).toString() != id)
            continue;
        position = existing.value(QStringLiteral("position")).toDouble();
        plays = existing.value(QStringLiteral("plays")).toInt();
        m_entries.removeAt(i);
        break;
    }
    entry.insert(QStringLiteral("position"), position);
    entry.insert(QStringLiteral("plays"), plays + 1);

    m_entries.prepend(entry);
    while (m_entries.size() > kMaxEntries)
        m_entries.removeLast();
    save();
    emit changed();
}

void History::setPosition(const QString &id, double position, double duration)
{
    if (id.size() != 11)
        return;
    for (int i = 0; i < m_entries.size(); ++i) {
        QVariantMap entry = m_entries.at(i).toMap();
        if (entry.value(QStringLiteral("id")).toString() != id)
            continue;
        // Past the end credits the entry is finished: remembering "1 s left"
        // would offer to resume a video the user just watched.
        const double stored = resumePoint(position, duration);
        entry.insert(QStringLiteral("position"), stored);
        if (duration > 0)
            entry.insert(QStringLiteral("seconds"), duration);
        entry.insert(QStringLiteral("updated"), QDateTime::currentSecsSinceEpoch());
        m_entries[i] = entry;
        save();
        emit changed();
        return;
    }
}

double History::positionFor(const QString &id) const
{
    for (const QVariant &value : m_entries) {
        const QVariantMap entry = value.toMap();
        if (entry.value(QStringLiteral("id")).toString() == id)
            return entry.value(QStringLiteral("position")).toDouble();
    }
    return 0;
}

void History::remove(const QString &id)
{
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries.at(i).toMap().value(QStringLiteral("id")).toString() != id)
            continue;
        m_entries.removeAt(i);
        save();
        emit changed();
        return;
    }
}

void History::clear()
{
    if (m_entries.isEmpty())
        return;
    m_entries.clear();
    save();
    emit changed();
}
