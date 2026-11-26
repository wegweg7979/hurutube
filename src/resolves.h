#pragma once

#include <QHash>
#include <QString>

// Pre-resolved direct stream URLs, kept across runs.
//
// A cold resolve costs 7-31 s (measured on studiobox2) because it is
// network-bound -- watch page, 2-3 MB of player JS, player API JSON -- while
// playing an ALREADY resolved URL reaches a picture in 1.05 s. Keeping these in
// memory only meant every launch, every replay from history and every fresh
// search paid the full price again.
//
// They are googlevideo URLs bound to this machine's address, not credentials, and
// they expire: the TTL is the 90 minutes the in-session cache already used, and an
// expired or dead URL is handled by the player's existing retry (replayWithoutCache).
class Resolves {
public:
    // 90 minutes: long enough to cover a session and the replay of what you just
    // watched, short enough that a URL is very unlikely to have expired first.
    static constexpr qint64 kTtlSeconds = 90 * 60;
    // Each entry is two long URLs; a few hundred is plenty and keeps the write
    // cheap.
    static constexpr int kMaxEntries = 200;

    struct Entry {
        QString video;
        QString audio;
        qint64 stamp = 0;
        bool cookies = false;

        bool fresh(qint64 now) const { return now - stamp < kTtlSeconds; }
    };

    // `filePath` empty means ~/.local/state/hurutube/resolves.json, beside the
    // history file.
    explicit Resolves(const QString &filePath = {});

    // Writes `entry` into `out` and answers whether it is fresh enough to use.
    bool fresh(const QString &url, Entry *out = nullptr) const;

    void insert(const QString &url, const Entry &entry);
    void forget(const QString &url);
    // Drop everything. Used when the codec preference changes: every cached URL was
    // chosen for the OLD codec, so keeping them would play exactly what the user
    // moved away from.
    void clear();
    int count() const { return m_entries.size(); }
    QString path() const { return m_path; }

private:
    void load();
    void save() const;

    QString m_path;
    QHash<QString, Entry> m_entries;
};
