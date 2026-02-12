#include "core/vector/vector_store.h"

#include <sqlite3.h>

#include <QDateTime>

#include <algorithm>
#include <limits>
#include <vector>

namespace bs {

namespace {

constexpr const char* kCreateVectorMapSql = R"(
    CREATE TABLE IF NOT EXISTS vector_map (
        item_id INTEGER NOT NULL,
        hnsw_label INTEGER NOT NULL,
        generation_id TEXT NOT NULL DEFAULT 'v1',
        model_id TEXT NOT NULL,
        dimensions INTEGER NOT NULL DEFAULT 0,
        provider TEXT NOT NULL DEFAULT 'cpu',
        passage_ordinal INTEGER NOT NULL DEFAULT 0,
        embedded_at REAL NOT NULL,
        migration_state TEXT NOT NULL DEFAULT 'active',
        PRIMARY KEY (item_id, generation_id, passage_ordinal),
        UNIQUE (generation_id, hnsw_label)
    )
)";

constexpr const char* kCreateVectorMapIndexesSql = R"(
    CREATE INDEX IF NOT EXISTS idx_vector_map_label
        ON vector_map(generation_id, hnsw_label);
    CREATE INDEX IF NOT EXISTS idx_vector_map_item_generation
        ON vector_map(item_id, generation_id);
    CREATE INDEX IF NOT EXISTS idx_vector_map_generation_state
        ON vector_map(generation_id, migration_state);
)";

constexpr const char* kCreateGenerationStateSql = R"(
    CREATE TABLE IF NOT EXISTS vector_generation_state (
        generation_id TEXT PRIMARY KEY,
        model_id TEXT NOT NULL,
        dimensions INTEGER NOT NULL,
        provider TEXT NOT NULL DEFAULT 'cpu',
        state TEXT NOT NULL DEFAULT 'building',
        progress_pct REAL NOT NULL DEFAULT 0.0,
        is_active INTEGER NOT NULL DEFAULT 0,
        updated_at REAL NOT NULL
    );
    CREATE INDEX IF NOT EXISTS idx_vector_generation_active
        ON vector_generation_state(is_active);
)";

constexpr const char* kAddSql = R"(
    INSERT OR REPLACE INTO vector_map (
        item_id, hnsw_label, generation_id, model_id, dimensions, provider,
        passage_ordinal, embedded_at, migration_state
    ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)
)";
constexpr const char* kRemoveSql =
    "DELETE FROM vector_map WHERE item_id = ?1 AND generation_id = ?2";
constexpr const char* kRemoveGenerationSql =
    "DELETE FROM vector_map WHERE generation_id = ?1";
constexpr const char* kGetLabelSql =
    "SELECT hnsw_label FROM vector_map WHERE item_id = ?1 AND generation_id = ?2 "
    "ORDER BY passage_ordinal ASC LIMIT 1";
constexpr const char* kGetItemIdSql =
    "SELECT item_id FROM vector_map WHERE hnsw_label = ?1 AND generation_id = ?2";
constexpr const char* kCountSql = "SELECT COUNT(*) FROM vector_map";
constexpr const char* kCountByGenerationSql =
    "SELECT COUNT(*) FROM vector_map WHERE generation_id = ?1";
constexpr const char* kGetAllSql = "SELECT item_id, hnsw_label FROM vector_map";
constexpr const char* kGetAllByGenerationSql =
    "SELECT item_id, hnsw_label FROM vector_map WHERE generation_id = ?1";
constexpr const char* kClearSql = "DELETE FROM vector_map";

constexpr const char* kActiveGenerationSql = R"(
    SELECT generation_id
    FROM vector_generation_state
    WHERE is_active = 1
    ORDER BY updated_at DESC
    LIMIT 1
)";

constexpr const char* kListGenerationStatesSql = R"(
    SELECT generation_id, model_id, dimensions, provider, state, progress_pct, is_active
    FROM vector_generation_state
    ORDER BY generation_id ASC
)";

constexpr const char* kUpsertGenerationStateSql = R"(
    INSERT INTO vector_generation_state (
        generation_id, model_id, dimensions, provider, state, progress_pct, is_active, updated_at
    ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)
    ON CONFLICT(generation_id) DO UPDATE SET
        model_id = excluded.model_id,
        dimensions = excluded.dimensions,
        provider = excluded.provider,
        state = excluded.state,
        progress_pct = excluded.progress_pct,
        is_active = excluded.is_active,
        updated_at = excluded.updated_at
)";

constexpr const char* kDeactivateGenerationsSql =
    "UPDATE vector_generation_state SET is_active = 0 WHERE generation_id != ?1";
constexpr const char* kSetActiveGenerationSql = R"(
    INSERT INTO vector_generation_state (
        generation_id, model_id, dimensions, provider, state, progress_pct, is_active, updated_at
    )
    VALUES (?1, COALESCE(?2, 'unknown'), COALESCE(?3, 0), COALESCE(?4, 'cpu'),
            COALESCE(?5, 'active'), COALESCE(?6, 100.0), 1, ?7)
    ON CONFLICT(generation_id) DO UPDATE SET
        is_active = 1,
        state = excluded.state,
        progress_pct = excluded.progress_pct,
        updated_at = excluded.updated_at
)";

bool execSql(sqlite3* db, const char* sql)
{
    if (!db) {
        return false;
    }
    char* errMsg = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        if (errMsg) {
            sqlite3_free(errMsg);
        }
        return false;
    }
    return true;
}

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
    sqlite3_finalize(m_removeGenerationStmt);
    sqlite3_finalize(m_getLabelStmt);
    sqlite3_finalize(m_getItemIdStmt);
    sqlite3_finalize(m_countStmt);
    sqlite3_finalize(m_countByGenerationStmt);
    sqlite3_finalize(m_getAllStmt);
    sqlite3_finalize(m_getAllByGenerationStmt);
    sqlite3_finalize(m_clearStmt);
}

bool VectorStore::addMapping(int64_t itemId, uint64_t hnswLabel, const std::string& modelId,
                             const std::string& generationId, int dimensions,
                             const std::string& provider, int passageOrdinal,
                             const std::string& migrationState)
{
    if (!m_ready
        || hnswLabel > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
        || passageOrdinal < 0) {
        return false;
    }

    const QByteArray generationUtf8(generationId.data(), static_cast<int>(generationId.size()));
    const QByteArray modelUtf8(modelId.data(), static_cast<int>(modelId.size()));
    const QByteArray providerUtf8(provider.data(), static_cast<int>(provider.size()));
    const QByteArray migrationStateUtf8(migrationState.data(), static_cast<int>(migrationState.size()));
    const double embeddedAt = static_cast<double>(QDateTime::currentSecsSinceEpoch());

    sqlite3_bind_int64(m_addStmt, 1, itemId);
    sqlite3_bind_int64(m_addStmt, 2, static_cast<int64_t>(hnswLabel));
    sqlite3_bind_text(m_addStmt, 3, generationUtf8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(m_addStmt, 4, modelUtf8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(m_addStmt, 5, dimensions);
    sqlite3_bind_text(m_addStmt, 6, providerUtf8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(m_addStmt, 7, passageOrdinal);
    sqlite3_bind_double(m_addStmt, 8, embeddedAt);
    sqlite3_bind_text(m_addStmt, 9, migrationStateUtf8.constData(), -1, SQLITE_TRANSIENT);

    const int rc = sqlite3_step(m_addStmt);
    resetStatement(m_addStmt);
    return rc == SQLITE_DONE;
}

bool VectorStore::removeMapping(int64_t itemId)
{
    if (!m_ready) {
        return false;
    }
    const std::string generationId = activeGenerationIdUnlocked();
    const QByteArray generationUtf8(generationId.data(), static_cast<int>(generationId.size()));
    sqlite3_bind_int64(m_removeStmt, 1, itemId);
    sqlite3_bind_text(m_removeStmt, 2, generationUtf8.constData(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(m_removeStmt);
    resetStatement(m_removeStmt);
    return rc == SQLITE_DONE;
}

bool VectorStore::removeGeneration(const std::string& generationId)
{
    if (!m_ready) {
        return false;
    }
    const QByteArray generationUtf8(generationId.data(), static_cast<int>(generationId.size()));
    sqlite3_bind_text(m_removeGenerationStmt, 1, generationUtf8.constData(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(m_removeGenerationStmt);
    resetStatement(m_removeGenerationStmt);
    return rc == SQLITE_DONE;
}

std::optional<uint64_t> VectorStore::getLabel(int64_t itemId, const std::string& generationId)
{
    if (!m_ready) {
        return std::nullopt;
    }

    const std::string effectiveGeneration = generationId.empty()
        ? activeGenerationIdUnlocked()
        : generationId;
    const QByteArray generationUtf8(effectiveGeneration.data(),
                                    static_cast<int>(effectiveGeneration.size()));

    sqlite3_bind_int64(m_getLabelStmt, 1, itemId);
    sqlite3_bind_text(m_getLabelStmt, 2, generationUtf8.constData(), -1, SQLITE_TRANSIENT);
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

std::optional<int64_t> VectorStore::getItemId(uint64_t hnswLabel, const std::string& generationId)
{
    if (!m_ready || hnswLabel > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return std::nullopt;
    }

    const std::string effectiveGeneration = generationId.empty()
        ? activeGenerationIdUnlocked()
        : generationId;
    const QByteArray generationUtf8(effectiveGeneration.data(),
                                    static_cast<int>(effectiveGeneration.size()));

    sqlite3_bind_int64(m_getItemIdStmt, 1, static_cast<int64_t>(hnswLabel));
    sqlite3_bind_text(m_getItemIdStmt, 2, generationUtf8.constData(), -1, SQLITE_TRANSIENT);
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

int VectorStore::countMappingsForGeneration(const std::string& generationId)
{
    if (!m_ready) {
        return 0;
    }
    const QByteArray generationUtf8(generationId.data(), static_cast<int>(generationId.size()));
    sqlite3_bind_text(m_countByGenerationStmt, 1, generationUtf8.constData(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(m_countByGenerationStmt);
    if (rc == SQLITE_ROW) {
        const int count = sqlite3_column_int(m_countByGenerationStmt, 0);
        resetStatement(m_countByGenerationStmt);
        return count;
    }
    resetStatement(m_countByGenerationStmt);
    return 0;
}

std::vector<std::pair<int64_t, uint64_t>> VectorStore::getAllMappings(const std::string& generationId)
{
    std::vector<std::pair<int64_t, uint64_t>> mappings;
    if (!m_ready) {
        return mappings;
    }

    sqlite3_stmt* stmt = nullptr;
    if (generationId.empty()) {
        stmt = m_getAllStmt;
    } else {
        stmt = m_getAllByGenerationStmt;
        const QByteArray generationUtf8(generationId.data(), static_cast<int>(generationId.size()));
        sqlite3_bind_text(stmt, 1, generationUtf8.constData(), -1, SQLITE_TRANSIENT);
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const int64_t itemId = sqlite3_column_int64(stmt, 0);
        const int64_t label = sqlite3_column_int64(stmt, 1);
        if (label >= 0) {
            mappings.emplace_back(itemId, static_cast<uint64_t>(label));
        }
    }
    resetStatement(stmt);
    return mappings;
}

bool VectorStore::upsertGenerationState(const GenerationState& state)
{
    if (!m_db || state.generationId.empty()) {
        return false;
    }

    if (state.active) {
        sqlite3_stmt* deactivateStmt = nullptr;
        if (sqlite3_prepare_v2(m_db, kDeactivateGenerationsSql, -1, &deactivateStmt, nullptr)
            != SQLITE_OK) {
            return false;
        }
        const QByteArray generationUtf8(
            state.generationId.data(), static_cast<int>(state.generationId.size()));
        sqlite3_bind_text(deactivateStmt, 1, generationUtf8.constData(), -1, SQLITE_TRANSIENT);
        const int deactivateRc = sqlite3_step(deactivateStmt);
        sqlite3_finalize(deactivateStmt);
        if (deactivateRc != SQLITE_DONE) {
            return false;
        }
    }

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, kUpsertGenerationStateSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    const QByteArray generationUtf8(state.generationId.data(),
                                    static_cast<int>(state.generationId.size()));
    const QByteArray modelUtf8(state.modelId.data(), static_cast<int>(state.modelId.size()));
    const QByteArray providerUtf8(state.provider.data(), static_cast<int>(state.provider.size()));
    const QByteArray stateUtf8(state.state.data(), static_cast<int>(state.state.size()));
    const double now = static_cast<double>(QDateTime::currentSecsSinceEpoch());

    sqlite3_bind_text(stmt, 1, generationUtf8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, modelUtf8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, state.dimensions);
    sqlite3_bind_text(stmt, 4, providerUtf8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, stateUtf8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 6, state.progressPct);
    sqlite3_bind_int(stmt, 7, state.active ? 1 : 0);
    sqlite3_bind_double(stmt, 8, now);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

std::vector<VectorStore::GenerationState> VectorStore::listGenerationStates() const
{
    std::vector<GenerationState> states;
    if (!m_db) {
        return states;
    }

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, kListGenerationStatesSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return states;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        GenerationState state;
        const char* generationId = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* modelId = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const int dimensions = sqlite3_column_int(stmt, 2);
        const char* provider = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        const char* status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        const double progressPct = sqlite3_column_double(stmt, 5);
        const bool active = sqlite3_column_int(stmt, 6) == 1;
        state.generationId = generationId ? generationId : "v1";
        state.modelId = modelId ? modelId : "unknown";
        state.dimensions = dimensions;
        state.provider = provider ? provider : "cpu";
        state.state = status ? status : "unknown";
        state.progressPct = progressPct;
        state.active = active;
        states.push_back(std::move(state));
    }
    sqlite3_finalize(stmt);
    return states;
}

std::optional<VectorStore::GenerationState> VectorStore::activeGenerationState() const
{
    for (const auto& state : listGenerationStates()) {
        if (state.active) {
            return state;
        }
    }
    return std::nullopt;
}

bool VectorStore::setActiveGeneration(const std::string& generationId)
{
    if (!m_db || generationId.empty()) {
        return false;
    }

    if (!execSql(m_db, "BEGIN IMMEDIATE TRANSACTION;")) {
        return false;
    }

    sqlite3_stmt* deactivateStmt = nullptr;
    if (sqlite3_prepare_v2(m_db, kDeactivateGenerationsSql, -1, &deactivateStmt, nullptr)
        != SQLITE_OK) {
        execSql(m_db, "ROLLBACK;");
        return false;
    }
    const QByteArray generationUtf8(generationId.data(), static_cast<int>(generationId.size()));
    sqlite3_bind_text(deactivateStmt, 1, generationUtf8.constData(), -1, SQLITE_TRANSIENT);
    const int deactivateRc = sqlite3_step(deactivateStmt);
    sqlite3_finalize(deactivateStmt);
    if (deactivateRc != SQLITE_DONE) {
        execSql(m_db, "ROLLBACK;");
        return false;
    }

    sqlite3_stmt* activateStmt = nullptr;
    if (sqlite3_prepare_v2(m_db, kSetActiveGenerationSql, -1, &activateStmt, nullptr)
        != SQLITE_OK) {
        execSql(m_db, "ROLLBACK;");
        return false;
    }

    const double now = static_cast<double>(QDateTime::currentSecsSinceEpoch());
    sqlite3_bind_text(activateStmt, 1, generationUtf8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_null(activateStmt, 2);
    sqlite3_bind_null(activateStmt, 3);
    sqlite3_bind_null(activateStmt, 4);
    sqlite3_bind_null(activateStmt, 5);
    sqlite3_bind_null(activateStmt, 6);
    sqlite3_bind_double(activateStmt, 7, now);
    const int activateRc = sqlite3_step(activateStmt);
    sqlite3_finalize(activateStmt);
    if (activateRc != SQLITE_DONE) {
        execSql(m_db, "ROLLBACK;");
        return false;
    }

    if (!execSql(m_db, "COMMIT;")) {
        execSql(m_db, "ROLLBACK;");
        return false;
    }
    return true;
}

std::string VectorStore::activeGenerationId() const
{
    return activeGenerationIdUnlocked();
}

std::string VectorStore::activeGenerationIdUnlocked() const
{
    if (!m_db) {
        return "v1";
    }
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, kActiveGenerationSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return "v1";
    }

    std::string generation = "v1";
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* generationRaw = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (generationRaw && generationRaw[0] != '\0') {
            generation = generationRaw;
        }
    }
    sqlite3_finalize(stmt);
    return generation;
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

    if (!ensureGenerationStateTable() || !ensureVectorMapSchema()) {
        return false;
    }

    if (sqlite3_prepare_v2(m_db, kAddSql, -1, &m_addStmt, nullptr) != SQLITE_OK) {
        return false;
    }
    if (sqlite3_prepare_v2(m_db, kRemoveSql, -1, &m_removeStmt, nullptr) != SQLITE_OK) {
        return false;
    }
    if (sqlite3_prepare_v2(m_db, kRemoveGenerationSql, -1, &m_removeGenerationStmt, nullptr) != SQLITE_OK) {
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
    if (sqlite3_prepare_v2(m_db, kCountByGenerationSql, -1, &m_countByGenerationStmt, nullptr) != SQLITE_OK) {
        return false;
    }
    if (sqlite3_prepare_v2(m_db, kGetAllSql, -1, &m_getAllStmt, nullptr) != SQLITE_OK) {
        return false;
    }
    if (sqlite3_prepare_v2(m_db, kGetAllByGenerationSql, -1, &m_getAllByGenerationStmt, nullptr) != SQLITE_OK) {
        return false;
    }
    if (sqlite3_prepare_v2(m_db, kClearSql, -1, &m_clearStmt, nullptr) != SQLITE_OK) {
        return false;
    }

    if (!activeGenerationState().has_value()) {
        GenerationState defaultState;
        defaultState.generationId = "v1";
        defaultState.modelId = "legacy";
        defaultState.dimensions = 384;
        defaultState.provider = "cpu";
        defaultState.state = "active";
        defaultState.progressPct = 100.0;
        defaultState.active = true;
        if (!upsertGenerationState(defaultState)) {
            return false;
        }
    }

    return true;
}

bool VectorStore::ensureVectorMapSchema()
{
    if (!m_db) {
        return false;
    }

    if (!execSql(m_db, kCreateVectorMapSql)) {
        return false;
    }

    const std::vector<std::string> requiredColumns = {
        "item_id",
        "hnsw_label",
        "generation_id",
        "model_id",
        "dimensions",
        "provider",
        "passage_ordinal",
        "embedded_at",
        "migration_state",
    };
    if (!hasVectorMapColumns(requiredColumns)) {
        if (!migrateLegacyVectorMap()) {
            return false;
        }
    }

    return execSql(m_db, kCreateVectorMapIndexesSql);
}

bool VectorStore::ensureGenerationStateTable() const
{
    return execSql(m_db, kCreateGenerationStateSql);
}

bool VectorStore::migrateLegacyVectorMap()
{
    if (!execSql(m_db, "BEGIN IMMEDIATE TRANSACTION;")) {
        return false;
    }

    if (!execSql(m_db, "ALTER TABLE vector_map RENAME TO vector_map_legacy_tmp;")) {
        execSql(m_db, "ROLLBACK;");
        return false;
    }

    if (!execSql(m_db, kCreateVectorMapSql) || !execSql(m_db, kCreateVectorMapIndexesSql)) {
        execSql(m_db, "ROLLBACK;");
        return false;
    }

    constexpr const char* kMigrateSql = R"(
        INSERT INTO vector_map (
            item_id, hnsw_label, generation_id, model_id, dimensions, provider,
            passage_ordinal, embedded_at, migration_state
        )
        SELECT
            item_id,
            hnsw_label,
            'v1',
            COALESCE(model_version, 'legacy'),
            384,
            'cpu',
            0,
            embedded_at,
            'active'
        FROM vector_map_legacy_tmp
    )";
    if (!execSql(m_db, kMigrateSql)) {
        execSql(m_db, "ROLLBACK;");
        return false;
    }
    if (!execSql(m_db, "DROP TABLE vector_map_legacy_tmp;")) {
        execSql(m_db, "ROLLBACK;");
        return false;
    }
    if (!execSql(m_db, "COMMIT;")) {
        execSql(m_db, "ROLLBACK;");
        return false;
    }

    GenerationState defaultState;
    defaultState.generationId = "v1";
    defaultState.modelId = "legacy";
    defaultState.dimensions = 384;
    defaultState.provider = "cpu";
    defaultState.state = "active";
    defaultState.progressPct = 100.0;
    defaultState.active = true;
    return upsertGenerationState(defaultState);
}

bool VectorStore::hasVectorMapColumns(const std::vector<std::string>& expectedColumns) const
{
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, "PRAGMA table_info(vector_map);", -1, &stmt, nullptr)
        != SQLITE_OK) {
        return false;
    }

    std::vector<std::string> discoveredColumns;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (name) {
            discoveredColumns.emplace_back(name);
        }
    }
    sqlite3_finalize(stmt);

    for (const std::string& expected : expectedColumns) {
        if (std::find(discoveredColumns.begin(), discoveredColumns.end(), expected)
            == discoveredColumns.end()) {
            return false;
        }
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
