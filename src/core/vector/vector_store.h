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
    explicit VectorStore(sqlite3* db);
    ~VectorStore();

    VectorStore(const VectorStore&) = delete;
    VectorStore& operator=(const VectorStore&) = delete;

    bool addMapping(int64_t itemId, uint64_t hnswLabel, const std::string& modelVersion);
    bool removeMapping(int64_t itemId);
    std::optional<uint64_t> getLabel(int64_t itemId);
    std::optional<int64_t> getItemId(uint64_t hnswLabel);
    int countMappings();
    std::vector<std::pair<int64_t, uint64_t>> getAllMappings();
    bool clearAll();

private:
    bool prepareStatements();
    static void resetStatement(sqlite3_stmt* stmt);

    sqlite3* m_db = nullptr;
    sqlite3_stmt* m_addStmt = nullptr;
    sqlite3_stmt* m_removeStmt = nullptr;
    sqlite3_stmt* m_getLabelStmt = nullptr;
    sqlite3_stmt* m_getItemIdStmt = nullptr;
    sqlite3_stmt* m_countStmt = nullptr;
    sqlite3_stmt* m_getAllStmt = nullptr;
    sqlite3_stmt* m_clearStmt = nullptr;
    bool m_ready = false;
};

} // namespace bs
