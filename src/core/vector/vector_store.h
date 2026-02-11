#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace bs {

class VectorStore {
public:
    struct GenerationState {
        std::string generationId;
        std::string modelId;
        int dimensions = 0;
        std::string provider;
        std::string state;
        double progressPct = 0.0;
        bool active = false;
    };

    explicit VectorStore(sqlite3* db);
    ~VectorStore();

    VectorStore(const VectorStore&) = delete;
    VectorStore& operator=(const VectorStore&) = delete;

    bool addMapping(int64_t itemId, uint64_t hnswLabel, const std::string& modelId,
                    const std::string& generationId = "v1",
                    int dimensions = 0,
                    const std::string& provider = "cpu",
                    int passageOrdinal = 0,
                    const std::string& migrationState = "active");
    bool removeMapping(int64_t itemId);
    bool removeGeneration(const std::string& generationId);
    std::optional<uint64_t> getLabel(int64_t itemId, const std::string& generationId = "");
    std::optional<int64_t> getItemId(uint64_t hnswLabel, const std::string& generationId = "");
    int countMappings();
    int countMappingsForGeneration(const std::string& generationId);
    std::vector<std::pair<int64_t, uint64_t>> getAllMappings(const std::string& generationId = "");
    bool upsertGenerationState(const GenerationState& state);
    std::vector<GenerationState> listGenerationStates() const;
    std::optional<GenerationState> activeGenerationState() const;
    bool setActiveGeneration(const std::string& generationId);
    std::string activeGenerationId() const;
    bool clearAll();

private:
    bool ensureVectorMapSchema();
    bool ensureGenerationStateTable() const;
    bool migrateLegacyVectorMap();
    bool hasVectorMapColumns(const std::vector<std::string>& expectedColumns) const;
    std::string activeGenerationIdUnlocked() const;

    bool prepareStatements();
    static void resetStatement(sqlite3_stmt* stmt);

    sqlite3* m_db = nullptr;
    sqlite3_stmt* m_addStmt = nullptr;
    sqlite3_stmt* m_removeStmt = nullptr;
    sqlite3_stmt* m_removeGenerationStmt = nullptr;
    sqlite3_stmt* m_getLabelStmt = nullptr;
    sqlite3_stmt* m_getItemIdStmt = nullptr;
    sqlite3_stmt* m_countStmt = nullptr;
    sqlite3_stmt* m_countByGenerationStmt = nullptr;
    sqlite3_stmt* m_getAllStmt = nullptr;
    sqlite3_stmt* m_getAllByGenerationStmt = nullptr;
    sqlite3_stmt* m_clearStmt = nullptr;
    bool m_ready = false;
};

} // namespace bs
