#include "core/feedback/feedback_aggregator.h"

#include <QDebug>
#include <QTimeZone>

#include <sqlite3.h>

namespace bs {

namespace {

constexpr const char* kTimestampExpr =
    "CASE WHEN typeof(timestamp)='text' THEN CAST(strftime('%s', timestamp) AS REAL) ELSE CAST(timestamp AS REAL) END";

bool execSql(sqlite3* db, const char* sql)
{
    char* errMsg = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        qWarning() << "FeedbackAggregator SQL failed:" << (errMsg ? errMsg : "unknown");
        sqlite3_free(errMsg);
        return false;
    }
    return true;
}

bool beginTransaction(sqlite3* db)
{
    return execSql(db, "BEGIN TRANSACTION");
}

bool commitTransaction(sqlite3* db)
{
    return execSql(db, "COMMIT");
}

void rollbackTransaction(sqlite3* db)
{
    execSql(db, "ROLLBACK");
}

} // namespace

FeedbackAggregator::FeedbackAggregator(sqlite3* db)
    : m_db(db)
{
}

bool FeedbackAggregator::runAggregation()
{
    if (!m_db) {
        qWarning() << "FeedbackAggregator::runAggregation called with null DB";
        return false;
    }

    const QDateTime previousAggregation = lastAggregationTime();
    const double sinceEpoch = previousAggregation.isValid()
        ? static_cast<double>(previousAggregation.toUTC().toSecsSinceEpoch())
        : 0.0;
    const qint64 nowEpoch = QDateTime::currentSecsSinceEpoch();

    if (!beginTransaction(m_db)) {
        return false;
    }

    sqlite3_stmt* itemStmt = nullptr;
    sqlite3_stmt* statsStmt = nullptr;
    sqlite3_stmt* pinStmt = nullptr;
    sqlite3_stmt* upsertFreqStmt = nullptr;
    sqlite3_stmt* updatePinnedStmt = nullptr;
    sqlite3_stmt* updateSettingStmt = nullptr;

    bool ok = true;

    const QString itemSql = QStringLiteral(
        "SELECT DISTINCT item_id FROM feedback "
        "WHERE (%1) > ?1").arg(QString::fromLatin1(kTimestampExpr));
    const QString statsSql = QStringLiteral(
        "SELECT "
        "  SUM(CASE WHEN action='open' THEN 1 ELSE 0 END), "
        "  MAX(CASE WHEN action='open' THEN (%1) END), "
        "  COUNT(*) "
        "FROM feedback "
        "WHERE item_id = ?1 AND (%1) > ?2").arg(QString::fromLatin1(kTimestampExpr));
    const QString pinSql = QStringLiteral(
        "SELECT action "
        "FROM feedback "
        "WHERE item_id = ?1 AND action IN ('pin', 'unpin') "
        "ORDER BY (%1) DESC, id DESC "
        "LIMIT 1").arg(QString::fromLatin1(kTimestampExpr));

    static constexpr const char* kUpsertFreqSql = R"(
        INSERT INTO frequencies (item_id, open_count, last_opened_at, total_interactions)
        VALUES (?1, ?2, ?3, ?4)
        ON CONFLICT(item_id) DO UPDATE SET
            open_count = frequencies.open_count + excluded.open_count,
            last_opened_at = CASE
                WHEN frequencies.last_opened_at IS NULL THEN excluded.last_opened_at
                WHEN excluded.last_opened_at IS NULL THEN frequencies.last_opened_at
                ELSE MAX(frequencies.last_opened_at, excluded.last_opened_at)
            END,
            total_interactions = frequencies.total_interactions + excluded.total_interactions
    )";

    static constexpr const char* kUpdatePinnedSql =
        "UPDATE items SET is_pinned = ?1 WHERE id = ?2";

    static constexpr const char* kUpdateSettingSql = R"(
        INSERT INTO settings (key, value)
        VALUES ('lastFeedbackAggregation', ?1)
        ON CONFLICT(key) DO UPDATE SET value = excluded.value
    )";

    if (sqlite3_prepare_v2(m_db, itemSql.toUtf8().constData(), -1, &itemStmt, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(m_db, statsSql.toUtf8().constData(), -1, &statsStmt, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(m_db, pinSql.toUtf8().constData(), -1, &pinStmt, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(m_db, kUpsertFreqSql, -1, &upsertFreqStmt, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(m_db, kUpdatePinnedSql, -1, &updatePinnedStmt, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(m_db, kUpdateSettingSql, -1, &updateSettingStmt, nullptr) != SQLITE_OK) {
        qWarning() << "FeedbackAggregator::runAggregation prepare failed:" << sqlite3_errmsg(m_db);
        ok = false;
    }

    if (ok) {
        sqlite3_bind_double(itemStmt, 1, sinceEpoch);
        while (sqlite3_step(itemStmt) == SQLITE_ROW) {
            const int64_t itemId = sqlite3_column_int64(itemStmt, 0);

            sqlite3_reset(statsStmt);
            sqlite3_clear_bindings(statsStmt);
            sqlite3_bind_int64(statsStmt, 1, itemId);
            sqlite3_bind_double(statsStmt, 2, sinceEpoch);

            int openCount = 0;
            int totalInteractions = 0;
            bool hasLastOpened = false;
            double lastOpened = 0.0;

            if (sqlite3_step(statsStmt) == SQLITE_ROW) {
                openCount = sqlite3_column_int(statsStmt, 0);
                if (sqlite3_column_type(statsStmt, 1) != SQLITE_NULL) {
                    lastOpened = sqlite3_column_double(statsStmt, 1);
                    hasLastOpened = true;
                }
                totalInteractions = sqlite3_column_int(statsStmt, 2);
            }

            sqlite3_reset(upsertFreqStmt);
            sqlite3_clear_bindings(upsertFreqStmt);
            sqlite3_bind_int64(upsertFreqStmt, 1, itemId);
            sqlite3_bind_int(upsertFreqStmt, 2, openCount);
            if (hasLastOpened) {
                sqlite3_bind_double(upsertFreqStmt, 3, lastOpened);
            } else {
                sqlite3_bind_null(upsertFreqStmt, 3);
            }
            sqlite3_bind_int(upsertFreqStmt, 4, totalInteractions);

            if (sqlite3_step(upsertFreqStmt) != SQLITE_DONE) {
                qWarning() << "FeedbackAggregator frequencies upsert failed for item" << itemId << ":" << sqlite3_errmsg(m_db);
                ok = false;
                break;
            }

            sqlite3_reset(pinStmt);
            sqlite3_clear_bindings(pinStmt);
            sqlite3_bind_int64(pinStmt, 1, itemId);

            if (sqlite3_step(pinStmt) == SQLITE_ROW) {
                const char* latestAction = reinterpret_cast<const char*>(sqlite3_column_text(pinStmt, 0));
                const bool isPinned = latestAction != nullptr && QString::fromUtf8(latestAction) == QStringLiteral("pin");

                sqlite3_reset(updatePinnedStmt);
                sqlite3_clear_bindings(updatePinnedStmt);
                sqlite3_bind_int(updatePinnedStmt, 1, isPinned ? 1 : 0);
                sqlite3_bind_int64(updatePinnedStmt, 2, itemId);

                if (sqlite3_step(updatePinnedStmt) != SQLITE_DONE) {
                    qWarning() << "FeedbackAggregator pin update failed for item" << itemId << ":" << sqlite3_errmsg(m_db);
                    ok = false;
                    break;
                }
            }
        }
    }

    if (ok) {
        sqlite3_reset(updateSettingStmt);
        sqlite3_clear_bindings(updateSettingStmt);
        const QByteArray nowUtf8 = QByteArray::number(nowEpoch);
        sqlite3_bind_text(updateSettingStmt, 1, nowUtf8.constData(), -1, SQLITE_STATIC);

        if (sqlite3_step(updateSettingStmt) != SQLITE_DONE) {
            qWarning() << "FeedbackAggregator setting update failed:" << sqlite3_errmsg(m_db);
            ok = false;
        }
    }

    sqlite3_finalize(itemStmt);
    sqlite3_finalize(statsStmt);
    sqlite3_finalize(pinStmt);
    sqlite3_finalize(upsertFreqStmt);
    sqlite3_finalize(updatePinnedStmt);
    sqlite3_finalize(updateSettingStmt);

    if (!ok) {
        rollbackTransaction(m_db);
        return false;
    }

    if (!commitTransaction(m_db)) {
        rollbackTransaction(m_db);
        return false;
    }

    qDebug() << "FeedbackAggregator::runAggregation completed at" << nowEpoch;
    return true;
}

bool FeedbackAggregator::cleanup(int feedbackRetentionDays, int interactionRetentionDays)
{
    if (!m_db) {
        qWarning() << "FeedbackAggregator::cleanup called with null DB";
        return false;
    }

    static constexpr const char* kFeedbackSql =
        "DELETE FROM feedback WHERE timestamp < datetime('now', ?1)";
    static constexpr const char* kInteractionSql =
        "DELETE FROM interactions WHERE timestamp < datetime('now', ?1)";

    sqlite3_stmt* feedbackStmt = nullptr;
    sqlite3_stmt* interactionStmt = nullptr;

    if (sqlite3_prepare_v2(m_db, kFeedbackSql, -1, &feedbackStmt, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(m_db, kInteractionSql, -1, &interactionStmt, nullptr) != SQLITE_OK) {
        qWarning() << "FeedbackAggregator::cleanup prepare failed:" << sqlite3_errmsg(m_db);
        sqlite3_finalize(feedbackStmt);
        sqlite3_finalize(interactionStmt);
        return false;
    }

    const QByteArray feedbackModifier = QStringLiteral("-%1 days").arg(feedbackRetentionDays).toUtf8();
    sqlite3_bind_text(feedbackStmt, 1, feedbackModifier.constData(), -1, SQLITE_STATIC);
    const int feedbackRc = sqlite3_step(feedbackStmt);

    const QByteArray interactionModifier = QStringLiteral("-%1 days").arg(interactionRetentionDays).toUtf8();
    sqlite3_bind_text(interactionStmt, 1, interactionModifier.constData(), -1, SQLITE_STATIC);
    const int interactionRc = sqlite3_step(interactionStmt);

    sqlite3_finalize(feedbackStmt);
    sqlite3_finalize(interactionStmt);

    if (feedbackRc != SQLITE_DONE || interactionRc != SQLITE_DONE) {
        qWarning() << "FeedbackAggregator::cleanup step failed:" << sqlite3_errmsg(m_db);
        return false;
    }

    qDebug() << "FeedbackAggregator::cleanup complete";
    return true;
}

QDateTime FeedbackAggregator::lastAggregationTime()
{
    if (!m_db) {
        qWarning() << "FeedbackAggregator::lastAggregationTime called with null DB";
        return {};
    }

    static constexpr const char* kSql =
        "SELECT value FROM settings WHERE key = 'lastFeedbackAggregation'";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        qWarning() << "FeedbackAggregator::lastAggregationTime prepare failed:" << sqlite3_errmsg(m_db);
        return {};
    }

    QDateTime result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* value = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (value != nullptr) {
            const QString strValue = QString::fromUtf8(value).trimmed();
            bool ok = false;
            const qint64 epoch = strValue.toLongLong(&ok);
            if (ok) {
                result = QDateTime::fromSecsSinceEpoch(epoch, QTimeZone::UTC);
            } else {
                result = QDateTime::fromString(strValue, Qt::ISODate);
                if (!result.isValid()) {
                    result = QDateTime::fromString(strValue, Qt::ISODateWithMs);
                }
                result = result.toUTC();
            }
        }
    }

    sqlite3_finalize(stmt);
    return result;
}

} // namespace bs
