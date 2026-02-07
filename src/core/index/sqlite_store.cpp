#include "core/index/sqlite_store.h"
#include "core/index/schema.h"
#include "core/index/migration.h"
#include "core/shared/logging.h"
#include <sqlite3.h>
#include <QDateTime>
#include <QFile>

#include <cstring>

namespace bs {

SQLiteStore::~SQLiteStore()
{
    if (m_db) {
        sqlite3_close(m_db);
        m_db = nullptr;
    }
}

std::optional<SQLiteStore> SQLiteStore::open(const QString& dbPath)
{
    SQLiteStore store;
    if (!store.init(dbPath)) {
        return std::nullopt;
    }
    return store;
}

bool SQLiteStore::init(const QString& dbPath)
{
    int rc = sqlite3_open(dbPath.toUtf8().constData(), &m_db);
    if (rc != SQLITE_OK) {
        LOG_ERROR(bsIndex, "Failed to open database: %s", sqlite3_errmsg(m_db));
        return false;
    }

    // Apply pragmas
    if (!execSql(kSchemaPragmas)) {
        LOG_ERROR(bsIndex, "Failed to set pragmas");
        return false;
    }

    // Verify WAL mode is active
    {
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(m_db, "PRAGMA journal_mode", -1, &stmt, nullptr);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* mode = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (mode && QString::fromUtf8(mode) != QLatin1String("wal")) {
                LOG_WARN(bsIndex, "Expected WAL journal mode, got: %s", mode);
            }
        }
        sqlite3_finalize(stmt);
    }

    // Create schema
    if (!execSql(kSchemaV1)) {
        LOG_ERROR(bsIndex, "Failed to create schema");
        return false;
    }

    // Set BM25 weights — ignore error if already set
    sqlite3_exec(m_db, kFts5WeightConfig, nullptr, nullptr, nullptr);

    // Insert default settings
    if (!execSql(kDefaultSettings)) {
        LOG_ERROR(bsIndex, "Failed to insert default settings");
        return false;
    }

    // Apply any pending migrations
    if (!applyMigrations(m_db, kCurrentSchemaVersion)) {
        LOG_ERROR(bsIndex, "Migration failed");
        return false;
    }

    // Restrict database file permissions to owner-only (0600)
    QFile dbFile(dbPath);
    dbFile.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    QFile walFile(dbPath + QStringLiteral("-wal"));
    if (walFile.exists()) {
        walFile.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    }
    QFile shmFile(dbPath + QStringLiteral("-shm"));
    if (shmFile.exists()) {
        shmFile.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    }

    LOG_INFO(bsIndex, "Database opened successfully: %s",
             dbPath.toUtf8().constData());
    return true;
}

bool SQLiteStore::execSql(const char* sql)
{
    char* errMsg = nullptr;
    int rc = sqlite3_exec(m_db, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        LOG_ERROR(bsIndex, "SQL error: %s", errMsg ? errMsg : "unknown");
        sqlite3_free(errMsg);
        return false;
    }
    return true;
}

// ── Items CRUD ──────────────────────────────────────────────

std::optional<int64_t> SQLiteStore::upsertItem(
    const QString& path,
    const QString& name,
    const QString& extension,
    ItemKind kind,
    int64_t size,
    double createdAt,
    double modifiedAt,
    const QString& contentHash,
    const QString& sensitivity,
    const QString& parentPath)
{
    const char* sql = R"(
        INSERT INTO items (path, name, extension, kind, size, created_at,
                           modified_at, indexed_at, content_hash, sensitivity, parent_path)
        VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)
        ON CONFLICT(path) DO UPDATE SET
            name = excluded.name,
            extension = excluded.extension,
            kind = excluded.kind,
            size = excluded.size,
            modified_at = excluded.modified_at,
            indexed_at = excluded.indexed_at,
            content_hash = excluded.content_hash,
            sensitivity = excluded.sensitivity,
            parent_path = excluded.parent_path
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        LOG_ERROR(bsIndex, "upsertItem prepare failed: %s", sqlite3_errmsg(m_db));
        return std::nullopt;
    }

    const double now = static_cast<double>(QDateTime::currentSecsSinceEpoch());
    const QByteArray pathUtf8 = path.toUtf8();
    const QByteArray nameUtf8 = name.toUtf8();
    const QByteArray extUtf8 = extension.toUtf8();
    const QByteArray kindStr = itemKindToString(kind).toUtf8();
    const QByteArray hashUtf8 = contentHash.toUtf8();
    const QByteArray sensUtf8 = sensitivity.toUtf8();
    const QByteArray parentUtf8 = parentPath.toUtf8();

    sqlite3_bind_text(stmt, 1, pathUtf8.constData(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, nameUtf8.constData(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, extUtf8.constData(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, kindStr.constData(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 5, size);
    sqlite3_bind_double(stmt, 6, createdAt);
    sqlite3_bind_double(stmt, 7, modifiedAt);
    sqlite3_bind_double(stmt, 8, now);
    if (contentHash.isEmpty()) {
        sqlite3_bind_null(stmt, 9);
    } else {
        sqlite3_bind_text(stmt, 9, hashUtf8.constData(), -1, SQLITE_STATIC);
    }
    sqlite3_bind_text(stmt, 10, sensUtf8.constData(), -1, SQLITE_STATIC);
    if (parentPath.isEmpty()) {
        sqlite3_bind_null(stmt, 11);
    } else {
        sqlite3_bind_text(stmt, 11, parentUtf8.constData(), -1, SQLITE_STATIC);
    }

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        LOG_ERROR(bsIndex, "upsertItem step failed: %s", sqlite3_errmsg(m_db));
        return std::nullopt;
    }

    // Retrieve the actual row id via SELECT rather than sqlite3_last_insert_rowid,
    // which can return a stale value when ON CONFLICT DO UPDATE fires inside a
    // batch transaction with interleaved INSERTs on other rows.
    auto row = getItemByPath(path);
    if (!row.has_value()) {
        LOG_ERROR(bsIndex, "upsertItem: row not found after successful upsert for %s",
                  path.toUtf8().constData());
        return std::nullopt;
    }
    return row->id;
}

bool SQLiteStore::deleteItemByPath(const QString& path)
{
    // First remove FTS5 entries (virtual tables don't cascade)
    {
        const char* fts = "DELETE FROM search_index WHERE file_path = ?1";
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(m_db, fts, -1, &stmt, nullptr);
        const QByteArray pathUtf8 = path.toUtf8();
        sqlite3_bind_text(stmt, 1, pathUtf8.constData(), -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    // Delete item (cascades to content, tags, failures, feedback, frequencies)
    const char* sql = "DELETE FROM items WHERE path = ?1";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);
    const QByteArray pathUtf8 = path.toUtf8();
    sqlite3_bind_text(stmt, 1, pathUtf8.constData(), -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

std::optional<SQLiteStore::ItemRow> SQLiteStore::getItemByPath(const QString& path)
{
    const char* sql = R"(
        SELECT id, path, name, kind, size, modified_at, indexed_at, content_hash, is_pinned
        FROM items WHERE path = ?1
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    const QByteArray pathUtf8 = path.toUtf8();
    sqlite3_bind_text(stmt, 1, pathUtf8.constData(), -1, SQLITE_STATIC);

    std::optional<ItemRow> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        ItemRow row;
        row.id = sqlite3_column_int64(stmt, 0);
        row.path = QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
        row.name = QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)));
        row.kind = QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)));
        row.size = sqlite3_column_int64(stmt, 4);
        row.modifiedAt = sqlite3_column_double(stmt, 5);
        row.indexedAt = sqlite3_column_double(stmt, 6);
        const char* hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        row.contentHash = hash ? QString::fromUtf8(hash) : QString();
        row.isPinned = sqlite3_column_int(stmt, 8) != 0;
        result = row;
    }
    sqlite3_finalize(stmt);
    return result;
}

std::optional<SQLiteStore::ItemRow> SQLiteStore::getItemById(int64_t id)
{
    const char* sql = R"(
        SELECT id, path, name, kind, size, modified_at, indexed_at, content_hash, is_pinned
        FROM items WHERE id = ?1
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(stmt, 1, id);

    std::optional<ItemRow> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        ItemRow row;
        row.id = sqlite3_column_int64(stmt, 0);
        row.path = QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
        row.name = QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)));
        row.kind = QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)));
        row.size = sqlite3_column_int64(stmt, 4);
        row.modifiedAt = sqlite3_column_double(stmt, 5);
        row.indexedAt = sqlite3_column_double(stmt, 6);
        const char* hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        row.contentHash = hash ? QString::fromUtf8(hash) : QString();
        row.isPinned = sqlite3_column_int(stmt, 8) != 0;
        result = row;
    }
    sqlite3_finalize(stmt);
    return result;
}

// ── Chunks + FTS5 (atomic — THE critical path) ─────────────

bool SQLiteStore::insertChunks(
    int64_t itemId,
    const QString& fileName,
    const QString& filePath,
    const std::vector<Chunk>& chunks)
{
    // CRITICAL: chunks + FTS5 are inserted atomically.
    // Use SAVEPOINT instead of BEGIN TRANSACTION so this works
    // both standalone and inside the pipeline's batch transaction.
    if (!execSql("SAVEPOINT insert_chunks")) return false;

    // Clear old chunks for this item
    {
        const char* sql = "DELETE FROM content WHERE item_id = ?1";
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);
        sqlite3_bind_int64(stmt, 1, itemId);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    // Clear old FTS5 entries
    {
        const char* sql = "DELETE FROM search_index WHERE file_id = ?1";
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);
        sqlite3_bind_int64(stmt, 1, itemId);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    // Insert each chunk into content AND search_index
    const char* contentSql = R"(
        INSERT INTO content (item_id, chunk_index, chunk_text, chunk_hash)
        VALUES (?1, ?2, ?3, ?4)
    )";
    const char* ftsSql = R"(
        INSERT INTO search_index (file_name, file_path, content, chunk_id, file_id)
        VALUES (?1, ?2, ?3, ?4, ?5)
    )";

    const QByteArray fileNameUtf8 = fileName.toUtf8();
    const QByteArray filePathUtf8 = filePath.toUtf8();

    for (const auto& chunk : chunks) {
        const QByteArray textUtf8 = chunk.content.toUtf8();
        const QByteArray hashUtf8 = chunk.chunkId.toUtf8();

        // Insert into content table
        {
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(m_db, contentSql, -1, &stmt, nullptr) != SQLITE_OK) {
                LOG_ERROR(bsIndex, "content insert prepare: %s", sqlite3_errmsg(m_db));
                execSql("ROLLBACK TO SAVEPOINT insert_chunks");
                execSql("RELEASE SAVEPOINT insert_chunks");
                return false;
            }
            sqlite3_bind_int64(stmt, 1, itemId);
            sqlite3_bind_int(stmt, 2, chunk.chunkIndex);
            sqlite3_bind_text(stmt, 3, textUtf8.constData(), -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 4, hashUtf8.constData(), -1, SQLITE_STATIC);
            int rc = sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            if (rc != SQLITE_DONE) {
                LOG_ERROR(bsIndex, "content insert failed: %s", sqlite3_errmsg(m_db));
                execSql("ROLLBACK TO SAVEPOINT insert_chunks");
                execSql("RELEASE SAVEPOINT insert_chunks");
                return false;
            }
        }

        // Insert into FTS5 search_index — MUST succeed for invariant
        {
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(m_db, ftsSql, -1, &stmt, nullptr) != SQLITE_OK) {
                LOG_ERROR(bsIndex, "FTS5 insert prepare: %s", sqlite3_errmsg(m_db));
                execSql("ROLLBACK TO SAVEPOINT insert_chunks");
                execSql("RELEASE SAVEPOINT insert_chunks");
                return false;
            }
            sqlite3_bind_text(stmt, 1, fileNameUtf8.constData(), -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, filePathUtf8.constData(), -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 3, textUtf8.constData(), -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 4, hashUtf8.constData(), -1, SQLITE_STATIC);
            sqlite3_bind_int64(stmt, 5, itemId);
            int rc = sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            if (rc != SQLITE_DONE) {
                LOG_ERROR(bsIndex, "CRITICAL: FTS5 insert failed: %s", sqlite3_errmsg(m_db));
                execSql("ROLLBACK TO SAVEPOINT insert_chunks");
                execSql("RELEASE SAVEPOINT insert_chunks");
                return false;
            }
        }
    }

    return execSql("RELEASE SAVEPOINT insert_chunks");
}

bool SQLiteStore::deleteChunksForItem(int64_t itemId, const QString& filePath)
{
    // Delete FTS5 entries first (no cascade on virtual tables)
    {
        const char* sql = "DELETE FROM search_index WHERE file_id = ?1";
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);
        sqlite3_bind_int64(stmt, 1, itemId);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    // Delete content rows (could also cascade from item delete)
    {
        const char* sql = "DELETE FROM content WHERE item_id = ?1";
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);
        sqlite3_bind_int64(stmt, 1, itemId);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    Q_UNUSED(filePath);
    return true;
}

// ── FTS5 Search ─────────────────────────────────────────────

std::vector<SQLiteStore::FtsHit> SQLiteStore::searchFts5(
    const QString& query, int limit)
{
    const char* sql = R"(
        SELECT file_id, chunk_id, rank,
               snippet(search_index, 2, '<b>', '</b>', '...', 32)
        FROM search_index
        WHERE search_index MATCH ?1
        ORDER BY rank
        LIMIT ?2
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        LOG_ERROR(bsIndex, "FTS5 search prepare: %s", sqlite3_errmsg(m_db));
        return {};
    }

    const QByteArray queryUtf8 = query.toUtf8();
    sqlite3_bind_text(stmt, 1, queryUtf8.constData(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, limit);

    std::vector<FtsHit> hits;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        FtsHit hit;
        hit.fileId = sqlite3_column_int64(stmt, 0);
        const char* cid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        hit.chunkId = cid ? QString::fromUtf8(cid) : QString();
        hit.bm25Score = sqlite3_column_double(stmt, 2);
        const char* snip = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        hit.snippet = snip ? QString::fromUtf8(snip) : QString();
        hits.push_back(hit);
    }
    sqlite3_finalize(stmt);
    return hits;
}

// ── Failures ────────────────────────────────────────────────

bool SQLiteStore::recordFailure(int64_t itemId, const QString& stage,
                                 const QString& errorMessage)
{
    const char* sql = R"(
        INSERT INTO failures (item_id, stage, error_message, failure_count, first_failed_at, last_failed_at)
        VALUES (?1, ?2, ?3, 1, ?4, ?4)
        ON CONFLICT(item_id, stage) DO UPDATE SET
            failure_count = failure_count + 1,
            last_failed_at = excluded.last_failed_at,
            error_message = excluded.error_message
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    const double now = static_cast<double>(QDateTime::currentSecsSinceEpoch());
    const QByteArray stageUtf8 = stage.toUtf8();
    const QByteArray errUtf8 = errorMessage.toUtf8();

    sqlite3_bind_int64(stmt, 1, itemId);
    sqlite3_bind_text(stmt, 2, stageUtf8.constData(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, errUtf8.constData(), -1, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 4, now);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool SQLiteStore::clearFailures(int64_t itemId)
{
    const char* sql = "DELETE FROM failures WHERE item_id = ?1";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, itemId);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

// ── Feedback ────────────────────────────────────────────────

bool SQLiteStore::recordFeedback(int64_t itemId, const QString& action,
                                  const QString& query, int position)
{
    const char* sql = R"(
        INSERT INTO feedback (item_id, action, query, result_position, timestamp)
        VALUES (?1, ?2, ?3, ?4, datetime('now'))
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        LOG_ERROR(bsIndex, "recordFeedback prepare failed: %s", sqlite3_errmsg(m_db));
        return false;
    }

    const QByteArray actionUtf8 = action.toUtf8();
    const QByteArray queryUtf8 = query.toUtf8();

    sqlite3_bind_int64(stmt, 1, itemId);
    sqlite3_bind_text(stmt, 2, actionUtf8.constData(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, queryUtf8.constData(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 4, position);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        LOG_ERROR(bsIndex, "recordFeedback step failed: %s", sqlite3_errmsg(m_db));
        return false;
    }
    return true;
}

// ── Frequencies ─────────────────────────────────────────────

bool SQLiteStore::incrementFrequency(int64_t itemId)
{
    const char* sql = R"(
        INSERT INTO frequencies (item_id, open_count, last_opened_at, total_interactions)
        VALUES (?1, 1, ?2, 1)
        ON CONFLICT(item_id) DO UPDATE SET
            open_count = open_count + 1,
            last_opened_at = excluded.last_opened_at,
            total_interactions = total_interactions + 1
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    const double now = static_cast<double>(QDateTime::currentSecsSinceEpoch());
    sqlite3_bind_int64(stmt, 1, itemId);
    sqlite3_bind_double(stmt, 2, now);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

std::optional<SQLiteStore::FrequencyRow> SQLiteStore::getFrequency(int64_t itemId)
{
    const char* sql = R"(
        SELECT open_count, last_opened_at, total_interactions
        FROM frequencies WHERE item_id = ?1
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(stmt, 1, itemId);

    std::optional<FrequencyRow> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        FrequencyRow row;
        row.openCount = sqlite3_column_int(stmt, 0);
        row.lastOpenedAt = sqlite3_column_double(stmt, 1);
        row.totalInteractions = sqlite3_column_int(stmt, 2);
        result = row;
    }
    sqlite3_finalize(stmt);
    return result;
}

// ── Feedback aggregation ────────────────────────────────────

bool SQLiteStore::aggregateFeedback()
{
    if (!m_db) return false;

    const char* sql =
        "INSERT OR REPLACE INTO frequencies (item_id, open_count, last_opened_at, total_interactions) "
        "SELECT f.item_id, "
        "       COALESCE(freq.open_count, 0) + COUNT(*), "
        "       MAX(f.timestamp), "
        "       COALESCE(freq.total_interactions, 0) + COUNT(*) "
        "FROM feedback f "
        "LEFT JOIN frequencies freq ON freq.item_id = f.item_id "
        "WHERE f.action = 'opened' "
        "GROUP BY f.item_id;";

    char* errMsg = nullptr;
    int rc = sqlite3_exec(m_db, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        LOG_ERROR(bsIndex, "aggregateFeedback failed: %s", errMsg ? errMsg : "unknown");
        sqlite3_free(errMsg);
        return false;
    }
    return true;
}

bool SQLiteStore::cleanupOldFeedback(int retentionDays)
{
    if (!m_db) return false;

    QString sql = QStringLiteral("DELETE FROM feedback WHERE timestamp < datetime('now', '-%1 days');")
                      .arg(retentionDays);

    char* errMsg = nullptr;
    int rc = sqlite3_exec(m_db, sql.toUtf8().constData(), nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        LOG_ERROR(bsIndex, "cleanupOldFeedback failed: %s", errMsg ? errMsg : "unknown");
        sqlite3_free(errMsg);
        return false;
    }
    return true;
}

// ── Settings ────────────────────────────────────────────────

std::optional<QString> SQLiteStore::getSetting(const QString& key)
{
    const char* sql = "SELECT value FROM settings WHERE key = ?1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    const QByteArray keyUtf8 = key.toUtf8();
    sqlite3_bind_text(stmt, 1, keyUtf8.constData(), -1, SQLITE_STATIC);

    std::optional<QString> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* val = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        result = val ? QString::fromUtf8(val) : QString();
    }
    sqlite3_finalize(stmt);
    return result;
}

bool SQLiteStore::setSetting(const QString& key, const QString& value)
{
    const char* sql = R"(
        INSERT INTO settings (key, value) VALUES (?1, ?2)
        ON CONFLICT(key) DO UPDATE SET value = excluded.value
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    const QByteArray keyUtf8 = key.toUtf8();
    const QByteArray valUtf8 = value.toUtf8();
    sqlite3_bind_text(stmt, 1, keyUtf8.constData(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, valUtf8.constData(), -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

// ── Health ──────────────────────────────────────────────────

IndexHealth SQLiteStore::getHealth()
{
    IndexHealth health;

    // Total indexed items
    {
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(m_db, "SELECT COUNT(*) FROM items", -1, &stmt, nullptr);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            health.totalIndexedItems = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }

    // Total chunks
    {
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(m_db, "SELECT COUNT(*) FROM content", -1, &stmt, nullptr);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            health.totalChunks = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }

    // Total failures
    {
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(m_db, "SELECT COUNT(*) FROM failures", -1, &stmt, nullptr);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            health.totalFailures = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }

    // Last index time
    {
        auto lastIndex = getSetting(QStringLiteral("last_full_index_at"));
        if (lastIndex) {
            health.lastIndexTime = lastIndex->toDouble();
        }
    }

    health.isHealthy = (health.totalFailures == 0);
    return health;
}

// ── Transactions ────────────────────────────────────────────

bool SQLiteStore::beginTransaction()
{
    return execSql("BEGIN TRANSACTION");
}

bool SQLiteStore::commitTransaction()
{
    return execSql("COMMIT");
}

bool SQLiteStore::rollbackTransaction()
{
    return execSql("ROLLBACK");
}

// ── Bulk operations ─────────────────────────────────────

bool SQLiteStore::deleteAll()
{
    // FTS5 virtual table must be cleared explicitly (no CASCADE)
    if (!execSql("DELETE FROM search_index")) {
        LOG_ERROR(bsIndex, "deleteAll: failed to clear search_index");
        return false;
    }
    // Delete items — cascades to content, tags, failures, feedback, frequencies
    if (!execSql("DELETE FROM items")) {
        LOG_ERROR(bsIndex, "deleteAll: failed to clear items");
        return false;
    }
    LOG_INFO(bsIndex, "deleteAll: all indexed data cleared");
    return true;
}

// ── Maintenance ─────────────────────────────────────────────

bool SQLiteStore::optimizeFts5()
{
    return execSql("INSERT INTO search_index(search_index) VALUES('optimize')");
}

bool SQLiteStore::vacuum()
{
    return execSql("VACUUM");
}

bool SQLiteStore::integrityCheck() const
{
    if (!m_db) return false;

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(m_db, "PRAGMA integrity_check;", -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    bool ok = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* result = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        ok = (result && strcmp(result, "ok") == 0);
    }
    sqlite3_finalize(stmt);
    return ok;
}

} // namespace bs
