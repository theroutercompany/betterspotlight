#pragma once

#include <QDateTime>
#include <QString>
#include <QVector>

struct sqlite3;

namespace bs {

class PathPreferences {
public:
    struct DirPreference {
        QString directory;
        int selectionCount = 0;
        double boost = 0.0;
    };

    explicit PathPreferences(sqlite3* db);

    QVector<DirPreference> getTopDirectories(int limit = 50);
    double getBoost(const QString& path);
    void invalidateCache();

private:
    bool shouldRefreshCache() const;
    void refreshCacheIfNeeded();

    sqlite3* m_db = nullptr;
    QVector<DirPreference> m_cache;
    bool m_cacheValid = false;
    QDateTime m_lastRefresh;
};

} // namespace bs
