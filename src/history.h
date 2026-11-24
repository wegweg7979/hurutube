#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

// What was watched, and where it got to.
//
// A plain JSON file in the user's state directory (~/.local/state/hurutube/
// history.json), written atomically: the file is a record of the user's viewing,
// so a crash mid-write must never leave a truncated one. Positions are kept per
// video id, newest first, capped so the file cannot grow forever.
class History : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList entries READ entries NOTIFY changed)
    Q_PROPERTY(int count READ count NOTIFY changed)

public:
    // The path is injectable so the tests need no real home directory.
    explicit History(const QString &filePath = {}, QObject *parent = nullptr);

    QVariantList entries() const { return m_entries; }
    int count() const { return m_entries.size(); }

    // Pure, so the resume rule is tested without touching a file:
    // "0" means start at the beginning.
    static double resumePoint(double position, double duration);
    static QVariantList parse(const QByteArray &json);
    static QByteArray serialize(const QVariantList &entries);

    // Remembers a video as watched, keeping any position already recorded for it
    // and moving it to the front.
    Q_INVOKABLE void remember(const QVariantMap &row);
    // Records how far in the user got. Ignores a video that was never
    // remembered, and a position that means "finished".
    Q_INVOKABLE void setPosition(const QString &id, double position, double duration);
    Q_INVOKABLE double positionFor(const QString &id) const;
    Q_INVOKABLE void remove(const QString &id);
    Q_INVOKABLE void clear();

signals:
    void changed();

private:
    void load();
    void save();

    QString m_path;
    QVariantList m_entries;
};
