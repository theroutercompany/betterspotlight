#include "core/feedback/interaction_tracker.h"

#include <QJsonObject>
#include <QRegularExpression>
#include <QtGlobal>
#include <QDebug>

#include <sqlite3.h>

namespace bs {

namespace {

QString toDbTimestamp(const QDateTime& dt)
{
    const QDateTime utc = dt.isValid() ? dt.toUTC() : QDateTime::currentDateTimeUtc();
    return utc.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

} // namespace

InteractionTracker::InteractionTracker(sqlite3* db)
    : m_db(db)
{
}

bool InteractionTracker::recordInteraction(const Interaction& interaction)
{
    if (!m_db) {
        qWarning() << "InteractionTracker::recordInteraction called with null DB";
        return false;
    }

    static constexpr const char* kSql = R"(
        INSERT INTO interactions (
            query,
            query_normalized,
            item_id,
            path,
            match_type,
            result_position,
            app_context,
            timestamp
        ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        qWarning() << "InteractionTracker::recordInteraction prepare failed:" << sqlite3_errmsg(m_db);
        return false;
    }

    const QString normalizedQuery = normalizeQuery(interaction.query);
    const QString timestamp = toDbTimestamp(interaction.timestamp);

    const QByteArray queryUtf8 = interaction.query.toUtf8();
    const QByteArray normalizedUtf8 = normalizedQuery.toUtf8();
    const QByteArray pathUtf8 = interaction.selectedPath.toUtf8();
    const QByteArray matchTypeUtf8 = interaction.matchType.toUtf8();
    const QByteArray appUtf8 = interaction.frontmostApp.toUtf8();
    const QByteArray timestampUtf8 = timestamp.toUtf8();

    sqlite3_bind_text(stmt, 1, queryUtf8.constData(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, normalizedUtf8.constData(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, interaction.selectedItemId);
    sqlite3_bind_text(stmt, 4, pathUtf8.constData(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, matchTypeUtf8.constData(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 6, interaction.resultPosition);
    if (interaction.frontmostApp.isEmpty()) {
        sqlite3_bind_null(stmt, 7);
    } else {
        sqlite3_bind_text(stmt, 7, appUtf8.constData(), -1, SQLITE_STATIC);
    }
    sqlite3_bind_text(stmt, 8, timestampUtf8.constData(), -1, SQLITE_STATIC);

    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        qWarning() << "InteractionTracker::recordInteraction step failed:" << sqlite3_errmsg(m_db);
        return false;
    }
    return true;
}

QString InteractionTracker::normalizeQuery(const QString& query)
{
    QString normalized = query.toLower().trimmed();
    normalized.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    while (normalized.endsWith(QLatin1Char('*'))) {
        normalized.chop(1);
    }
    return normalized.trimmed();
}

int InteractionTracker::getInteractionCount(const QString& query, int64_t itemId)
{
    if (!m_db) {
        qWarning() << "InteractionTracker::getInteractionCount called with null DB";
        return 0;
    }

    static constexpr const char* kSql = R"(
        SELECT COUNT(*)
        FROM interactions
        WHERE query_normalized = ?1
          AND item_id = ?2
          AND timestamp >= datetime('now', '-90 days')
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        qWarning() << "InteractionTracker::getInteractionCount prepare failed:" << sqlite3_errmsg(m_db);
        return 0;
    }

    const QString normalized = normalizeQuery(query);
    const QByteArray normalizedUtf8 = normalized.toUtf8();
    sqlite3_bind_text(stmt, 1, normalizedUtf8.constData(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, itemId);

    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }

    sqlite3_finalize(stmt);
    return count;
}

int InteractionTracker::getInteractionBoost(const QString& query, int64_t itemId)
{
    const int count = getInteractionCount(query, itemId);
    return qMin(count * 5, 25);
}

bool InteractionTracker::cleanup(int retentionDays)
{
    if (!m_db) {
        qWarning() << "InteractionTracker::cleanup called with null DB";
        return false;
    }

    static constexpr const char* kSql =
        "DELETE FROM interactions WHERE timestamp < datetime('now', ?1)";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        qWarning() << "InteractionTracker::cleanup prepare failed:" << sqlite3_errmsg(m_db);
        return false;
    }

    const QString modifier = QStringLiteral("-%1 days").arg(retentionDays);
    const QByteArray modifierUtf8 = modifier.toUtf8();
    sqlite3_bind_text(stmt, 1, modifierUtf8.constData(), -1, SQLITE_STATIC);

    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        qWarning() << "InteractionTracker::cleanup step failed:" << sqlite3_errmsg(m_db);
        return false;
    }

    qDebug() << "InteractionTracker::cleanup removed" << sqlite3_changes(m_db) << "rows";
    return true;
}

QJsonArray InteractionTracker::exportData()
{
    QJsonArray output;
    if (!m_db) {
        qWarning() << "InteractionTracker::exportData called with null DB";
        return output;
    }

    static constexpr const char* kSql = R"(
        SELECT query, query_normalized, item_id, path, match_type, result_position, app_context, timestamp
        FROM interactions
        ORDER BY timestamp DESC
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        qWarning() << "InteractionTracker::exportData prepare failed:" << sqlite3_errmsg(m_db);
        return output;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        QJsonObject row;

        const char* query = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* queryNorm = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const char* path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        const char* matchType = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        const char* appContext = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        const char* timestamp = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));

        row.insert(QStringLiteral("query"), query ? QString::fromUtf8(query) : QString());
        row.insert(QStringLiteral("queryNormalized"), queryNorm ? QString::fromUtf8(queryNorm) : QString());
        row.insert(QStringLiteral("itemId"), static_cast<qint64>(sqlite3_column_int64(stmt, 2)));
        row.insert(QStringLiteral("path"), path ? QString::fromUtf8(path) : QString());
        row.insert(QStringLiteral("matchType"), matchType ? QString::fromUtf8(matchType) : QString());
        row.insert(QStringLiteral("resultPosition"), sqlite3_column_int(stmt, 5));
        row.insert(QStringLiteral("frontmostApp"), appContext ? QString::fromUtf8(appContext) : QString());
        row.insert(QStringLiteral("timestamp"), timestamp ? QString::fromUtf8(timestamp) : QString());
        output.push_back(row);
    }

    sqlite3_finalize(stmt);
    return output;
}

} // namespace bs
