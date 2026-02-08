#pragma once

#include <QDateTime>
#include <QString>

struct sqlite3;

namespace bs {

class TypeAffinity {
public:
    struct AffinityStats {
        int codeOpens = 0;
        int documentOpens = 0;
        int mediaOpens = 0;
        int otherOpens = 0;
        QString primaryAffinity;
    };

    explicit TypeAffinity(sqlite3* db);

    AffinityStats getAffinityStats();
    double getBoost(const QString& filePath);
    void invalidateCache();

private:
    bool shouldRefreshCache() const;
    void refreshCacheIfNeeded();
    static QString fileExtension(const QString& filePath);
    static bool extensionMatchesCategory(const QString& extension, const QString& category);

    sqlite3* m_db = nullptr;
    AffinityStats m_cachedStats;
    bool m_cacheValid = false;
    QDateTime m_lastRefresh;
};

} // namespace bs
