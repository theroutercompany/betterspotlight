#include "core/feedback/path_preferences.h"

#include <QDebug>
#include <QtGlobal>

#include <sqlite3.h>

namespace {

constexpr qint64 kRefreshIntervalMs = 10 * 60 * 1000;

QString extractParentDirectory(const QString& path)
{
    const int lastSlash = path.lastIndexOf(QLatin1Char('/'));
    if (lastSlash <= 0) {
        return QString();
    }
    return path.left(lastSlash);
}

double directoryBoost(int selectionCount)
{
    if (selectionCount <= 0) {
        return 0.0;
    }
    return static_cast<double>(qMin(selectionCount / 5, 15));
}

} // namespace

namespace bs {

PathPreferences::PathPreferences(sqlite3* db)
    : m_db(db)
{
}

QVector<PathPreferences::DirPreference> PathPreferences::getTopDirectories(int limit)
{
    QVector<DirPreference> output;
    if (!m_db) {
        qWarning() << "PathPreferences::getTopDirectories called with null DB";
        return output;
    }

    if (limit <= 0) {
        return output;
    }

    static constexpr const char* kSql = R"(
        WITH RECURSIVE
        recent_paths(path) AS (
            SELECT path
            FROM interactions
            WHERE timestamp >= datetime('now', '-90 days')
              AND path LIKE '%/%'
        ),
        slash_scan(path, idx, last_idx) AS (
            SELECT path, 1, 0
            FROM recent_paths
            UNION ALL
            SELECT
                path,
                idx + 1,
                CASE WHEN substr(path, idx, 1) = '/' THEN idx ELSE last_idx END
            FROM slash_scan
            WHERE idx <= length(path)
        )
        SELECT
            substr(path, 1, last_idx - 1) AS parent_dir,
            COUNT(*) AS selection_count
        FROM slash_scan
        WHERE idx = length(path) + 1
          AND last_idx > 0
          AND substr(path, 1, last_idx - 1) != ''
        GROUP BY parent_dir
        ORDER BY selection_count DESC, parent_dir ASC
        LIMIT ?1
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        qWarning() << "PathPreferences::getTopDirectories prepare failed:" << sqlite3_errmsg(m_db);
        return output;
    }

    sqlite3_bind_int(stmt, 1, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* rawDir = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const int selectionCount = sqlite3_column_int(stmt, 1);
        if (rawDir == nullptr || selectionCount <= 0) {
            continue;
        }

        DirPreference preference;
        preference.directory = QString::fromUtf8(rawDir);
        preference.selectionCount = selectionCount;
        preference.boost = directoryBoost(selectionCount);
        output.push_back(preference);
    }

    sqlite3_finalize(stmt);
    qDebug() << "PathPreferences::getTopDirectories loaded" << output.size() << "directories";
    return output;
}

double PathPreferences::getBoost(const QString& path)
{
    if (!m_db || path.isEmpty()) {
        return 0.0;
    }

    refreshCacheIfNeeded();

    const QString parentDirectory = extractParentDirectory(path);
    if (parentDirectory.isEmpty()) {
        return 0.0;
    }

    for (const DirPreference& preference : m_cache) {
        if (preference.directory == parentDirectory) {
            return preference.boost;
        }
    }
    return 0.0;
}

void PathPreferences::invalidateCache()
{
    m_cache.clear();
    m_cacheValid = false;
    m_lastRefresh = QDateTime();
    qDebug() << "PathPreferences::invalidateCache cleared cache";
}

bool PathPreferences::shouldRefreshCache() const
{
    if (!m_cacheValid || !m_lastRefresh.isValid()) {
        return true;
    }
    return m_lastRefresh.msecsTo(QDateTime::currentDateTimeUtc()) >= kRefreshIntervalMs;
}

void PathPreferences::refreshCacheIfNeeded()
{
    if (!shouldRefreshCache()) {
        return;
    }

    m_cache = getTopDirectories(50);
    m_cacheValid = true;
    m_lastRefresh = QDateTime::currentDateTimeUtc();
}

} // namespace bs
