#include "core/vector/vector_store.h"

#include <sqlite3.h>

#include <QDateTime>

#include <limits>

namespace bs {

namespace {

constexpr const char* kCreateTableSql = R"(
    CREATE TABLE IF NOT EXISTS vector_map (
        item_id INTEGER PRIMARY KEY,
        hnsw_label INTEGER UNIQUE NOT NULL,
        model_version TEXT NOT NULL,
        embedded_at REAL NOT NULL
    )
)";

constexpr const char* kAddSql = R"(
    INSERT OR REPLACE INTO vector_map (item_id, hnsw_label, model_version, embedded_at)
    VALUES (?1, ?2, ?3, ?4)
)";
constexpr const char* kRemoveSql = "DELETE FROM vector_map WHERE item_id = ?1";
constexpr const char* kGetLabelSql = "SELECT hnsw_label FROM vector_map WHERE item_id = ?1";
constexpr const char* kGetItemIdSql = "SELECT item_id FROM vector_map WHERE hnsw_label = ?1";
constexpr const char* kCountSql = "SELECT COUNT(*) FROM vector_map";
constexpr const char* kGetAllSql = "SELECT item_id, hnsw_label FROM vector_map";
constexpr const char* kClearSql = "DELETE FROM vector_map";

} // namespace

VectorStore::VectorStore(sqlite3* db)
    : m_db(db)
{
    m_ready = prepareStatements();
}

VectorStore::~VectorStore()
{
    sqlite3_finalize(m_addStmt);
    sqlite3_finalize(m_removeStmt);
    sqlite3_finalize(m_getLabelStmt);
    sqlite3_finalize(m_getItemIdStmt);
    sqlite3_finalize(m_countStmt);
    sqlite3_finalize(m_getAllStmt);
    sqlite3_finalize(m_clearStmt);
}

bool VectorStore::addMapping(int64_t itemId, uint64_t hnswLabel, const std::string& modelVersion)
{
    if (!m_ready || hnswLabel > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return false;
    }

    const QByteArray modelVersionUtf8(modelVersion.data(), static_cast<int>(modelVersion.size()));
    const double embeddedAt = static_cast<double>(QDateTime::currentSecsSinceEpoch());

    sqlite3_bind_int64(m_addStmt, 1, itemId);
    sqlite3_bind_int64(m_addStmt, 2, static_cast<int64_t>(hnswLabel));
    sqlite3_bind_text(m_addStmt, 3, modelVersionUtf8.constData(), -1, SQLITE_STATIC);
    sqlite3_bind_double(m_addStmt, 4, embeddedAt);

    const int rc = sqlite3_step(m_addStmt);
    resetStatement(m_addStmt);
    return rc == SQLITE_DONE;
}

bool VectorStore::removeMapping(int64_t itemId)
{
    if (!m_ready) {
        return false;
    }

    sqlite3_bind_int64(m_removeStmt, 1, itemId);
    const int rc = sqlite3_step(m_removeStmt);
    resetStatement(m_removeStmt);
    return rc == SQLITE_DONE;
}

std::optional<uint64_t> VectorStore::getLabel(int64_t itemId)
{
    if (!m_ready) {
        return std::nullopt;
    }

    sqlite3_bind_int64(m_getLabelStmt, 1, itemId);
    const int rc = sqlite3_step(m_getLabelStmt);
    if (rc == SQLITE_ROW) {
        const int64_t label = sqlite3_column_int64(m_getLabelStmt, 0);
        resetStatement(m_getLabelStmt);
        if (label < 0) {
            return std::nullopt;
        }
        return static_cast<uint64_t>(label);
    }

    resetStatement(m_getLabelStmt);
    return std::nullopt;
}

std::optional<int64_t> VectorStore::getItemId(uint64_t hnswLabel)
{
    if (!m_ready || hnswLabel > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return std::nullopt;
    }

    sqlite3_bind_int64(m_getItemIdStmt, 1, static_cast<int64_t>(hnswLabel));
    const int rc = sqlite3_step(m_getItemIdStmt);
    if (rc == SQLITE_ROW) {
        const int64_t itemId = sqlite3_column_int64(m_getItemIdStmt, 0);
        resetStatement(m_getItemIdStmt);
        return itemId;
    }

    resetStatement(m_getItemIdStmt);
    return std::nullopt;
}

int VectorStore::countMappings()
{
    if (!m_ready) {
        return 0;
    }

    const int rc = sqlite3_step(m_countStmt);
    if (rc == SQLITE_ROW) {
        const int count = sqlite3_column_int(m_countStmt, 0);
        resetStatement(m_countStmt);
        return count;
    }

    resetStatement(m_countStmt);
    return 0;
}

std::vector<std::pair<int64_t, uint64_t>> VectorStore::getAllMappings()
{
    std::vector<std::pair<int64_t, uint64_t>> mappings;
    if (!m_ready) {
        return mappings;
    }

    while (sqlite3_step(m_getAllStmt) == SQLITE_ROW) {
        const int64_t itemId = sqlite3_column_int64(m_getAllStmt, 0);
        const int64_t label = sqlite3_column_int64(m_getAllStmt, 1);
        if (label >= 0) {
            mappings.emplace_back(itemId, static_cast<uint64_t>(label));
        }
    }
    resetStatement(m_getAllStmt);
    return mappings;
}

bool VectorStore::clearAll()
{
    if (!m_ready) {
        return false;
    }

    const int rc = sqlite3_step(m_clearStmt);
    resetStatement(m_clearStmt);
    return rc == SQLITE_DONE;
}

bool VectorStore::prepareStatements()
{
    if (!m_db) {
        return false;
    }

    char* errMsg = nullptr;
    if (sqlite3_exec(m_db, kCreateTableSql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
        if (errMsg) {
            sqlite3_free(errMsg);
        }
        return false;
    }

    if (sqlite3_prepare_v2(m_db, kAddSql, -1, &m_addStmt, nullptr) != SQLITE_OK) {
        return false;
    }
    if (sqlite3_prepare_v2(m_db, kRemoveSql, -1, &m_removeStmt, nullptr) != SQLITE_OK) {
        return false;
    }
    if (sqlite3_prepare_v2(m_db, kGetLabelSql, -1, &m_getLabelStmt, nullptr) != SQLITE_OK) {
        return false;
    }
    if (sqlite3_prepare_v2(m_db, kGetItemIdSql, -1, &m_getItemIdStmt, nullptr) != SQLITE_OK) {
        return false;
    }
    if (sqlite3_prepare_v2(m_db, kCountSql, -1, &m_countStmt, nullptr) != SQLITE_OK) {
        return false;
    }
    if (sqlite3_prepare_v2(m_db, kGetAllSql, -1, &m_getAllStmt, nullptr) != SQLITE_OK) {
        return false;
    }
    if (sqlite3_prepare_v2(m_db, kClearSql, -1, &m_clearStmt, nullptr) != SQLITE_OK) {
        return false;
    }

    return true;
}

void VectorStore::resetStatement(sqlite3_stmt* stmt)
{
    if (!stmt) {
        return;
    }
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
}

} // namespace bs
