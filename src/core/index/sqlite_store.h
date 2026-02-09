#pragma once

#include "core/shared/types.h"
#include "core/shared/chunk.h"
#include "core/shared/index_health.h"
#include "core/shared/search_options.h"
#include <QString>
#include <optional>
#include <unordered_map>
#include <vector>
#include <cstdint>

#include <sqlite3.h>

namespace bs {

// SQLiteStore — single-threaded owner of the SQLite database.
// Implements all CRUD operations, FTS5 indexing, and maintenance.
//
// CRITICAL INVARIANT (doc 03 Stage 7):
//   There is no code path where chunk insertion succeeds but FTS5
//   indexing is skipped. Every insertChunks() call automatically
//   populates search_index.
class SQLiteStore {
public:
    ~SQLiteStore();

    // Move-only (owns sqlite3* handle)
    SQLiteStore(SQLiteStore&& other) noexcept : m_db(other.m_db) { other.m_db = nullptr; }
    SQLiteStore& operator=(SQLiteStore&& other) noexcept {
        if (this != &other) {
            if (m_db) sqlite3_close(m_db);
            m_db = other.m_db;
            other.m_db = nullptr;
        }
        return *this;
    }
    SQLiteStore(const SQLiteStore&) = delete;
    SQLiteStore& operator=(const SQLiteStore&) = delete;

    // Open or create the database at the given path.
    // Creates schema and sets pragmas on first open.
    static std::optional<SQLiteStore> open(const QString& dbPath);

    // ── Items CRUD ──────────────────────────────────────────

    // Insert or update an item. Returns the item id.
    std::optional<int64_t> upsertItem(
        const QString& path,
        const QString& name,
        const QString& extension,
        ItemKind kind,
        int64_t size,
        double createdAt,
        double modifiedAt,
        const QString& contentHash = {},
        const QString& sensitivity = QStringLiteral("normal"),
        const QString& parentPath = {});

    bool deleteItemByPath(const QString& path);
    bool updateContentHash(int64_t itemId, const QString& contentHash);

    struct ItemRow {
        int64_t id = 0;
        QString path;
        QString name;
        QString kind;
        int64_t size = 0;
        double modifiedAt = 0.0;
        double indexedAt = 0.0;
        QString contentHash;
        bool isPinned = false;
    };

    std::optional<ItemRow> getItemByPath(const QString& path);
    std::optional<ItemRow> getItemById(int64_t id);

    struct ItemAvailability {
        bool contentAvailable = false;
        QString availabilityStatus = QStringLiteral("available");
        QString lastExtractionError;
    };

    std::optional<ItemAvailability> getItemAvailability(int64_t id);

    // ── Chunks + FTS5 (atomic) ──────────────────────────────

    // Insert chunks AND index them in FTS5 in one transaction.
    // This is the ONLY way to add content — guaranteeing the
    // critical invariant that FTS5 is always populated.
    bool insertChunks(int64_t itemId,
                      const QString& fileName,
                      const QString& filePath,
                      const std::vector<Chunk>& chunks);

    // Remove all chunks and FTS5 entries for an item.
    bool deleteChunksForItem(int64_t itemId, const QString& filePath);

    // ── FTS5 Search ─────────────────────────────────────────

    struct FtsHit {
        int64_t fileId = 0;
        QString chunkId;
        double bm25Score = 0.0;
        QString snippet;
    };

    struct FtsJoinedHit {
        int64_t fileId = 0;
        QString chunkId;
        double bm25Score = 0.0;
        QString snippet;
        // Hydrated from JOIN with items table:
        QString path;
        QString name;
        QString kind;
        int64_t size = 0;
        double modifiedAt = 0.0;
        QString parentPath;
        bool isPinned = false;
        QString contentHash;
    };

    struct NameHit {
        int64_t fileId = 0;
        QString name;
        QString path;
    };

    std::vector<FtsHit> searchFts5(const QString& query, int limit = 20, bool relaxed = false);
    std::vector<FtsHit> searchFts5(const QString& query, int limit, bool relaxed,
                                   const SearchOptions& options);

    // FTS5 search with JOIN to items table — returns hydrated results in one query.
    // Filters are applied in SQL, not post-hoc.
    std::vector<FtsJoinedHit> searchFts5Joined(
        const QString& query, int limit, bool relaxed,
        const SearchOptions& options = {});

    std::vector<NameHit> searchByNameFuzzy(const QString& query, int limit = 20);
    std::vector<NameHit> searchByNameFuzzy(const QString& query, int limit,
                                           const SearchOptions& options);

    // ── Failures ────────────────────────────────────────────

    bool recordFailure(int64_t itemId, const QString& stage,
                       const QString& errorMessage);
    bool clearFailures(int64_t itemId);

    // ── Feedback ──────────────────────────────────────────

    bool recordFeedback(int64_t itemId, const QString& action,
                        const QString& query, int position);

    // ── Frequencies ─────────────────────────────────────────

    bool incrementFrequency(int64_t itemId);

    struct FrequencyRow {
        int openCount = 0;
        double lastOpenedAt = 0.0;
        int totalInteractions = 0;
    };

    std::optional<FrequencyRow> getFrequency(int64_t itemId);

    // Batch fetch frequencies for multiple items in one query.
    std::unordered_map<int64_t, FrequencyRow> getFrequenciesBatch(
        const std::vector<int64_t>& itemIds);

    // ── Feedback aggregation ──────────────────────────────

    // Aggregate feedback into frequencies table
    bool aggregateFeedback();

    // Delete feedback entries older than retentionDays
    bool cleanupOldFeedback(int retentionDays = 90);

    // ── Settings ────────────────────────────────────────────

    std::optional<QString> getSetting(const QString& key);
    bool setSetting(const QString& key, const QString& value);

    // Apply BM25 weights from settings table, or use defaults if not found.
    bool applyBm25Weights();

    // Update BM25 weights: persists to settings + reapplies to FTS5.
    bool setBm25Weights(double nameWeight, double pathWeight, double contentWeight);

    // ── Health ──────────────────────────────────────────────

    IndexHealth getHealth();

    // ── Transactions ────────────────────────────────────────

    bool beginTransaction();
    bool commitTransaction();
    bool rollbackTransaction();

    // ── Bulk operations ────────────────────────────────────

    // Delete ALL indexed data (items, content, FTS5, failures,
    // frequencies, feedback). Used by Pipeline::rebuildAll().
    bool deleteAll();

    // ── Maintenance ─────────────────────────────────────────

    bool optimizeFts5();
    bool vacuum();

    // Returns true if database passes PRAGMA integrity_check
    bool integrityCheck() const;

    // Raw handle for tests
    sqlite3* rawDb() const { return m_db; }

private:
    SQLiteStore() = default;
    static QString sanitizeFtsQueryStrict(const QString& raw);
    static QString sanitizeFtsQueryRelaxed(const QString& raw);
    bool init(const QString& dbPath);
    bool execSql(const char* sql);

    sqlite3* m_db = nullptr;
};

} // namespace bs
