#include "core/index/migration.h"
#include "core/shared/logging.h"
#include <sqlite3.h>
#include <string>

namespace bs {

int currentSchemaVersion(sqlite3* db)
{
    const char* sql = "SELECT value FROM settings WHERE key = 'schema_version'";
    sqlite3_stmt* stmt = nullptr;
    int version = 0;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* val = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (val) {
                version = std::stoi(val);
            }
        }
    }
    sqlite3_finalize(stmt);
    return version;
}

bool applyMigrations(sqlite3* db, int targetVersion)
{
    int current = currentSchemaVersion(db);

    if (current > targetVersion) {
        LOG_ERROR(bsIndex, "Schema version %d is newer than app version %d — downgrade not supported",
                  current, targetVersion);
        return false;
    }

    if (current == targetVersion) {
        return true;
    }

    auto exec = [db](const char* sql) -> bool {
        char* errMsg = nullptr;
        const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);
        if (rc != SQLITE_OK) {
            LOG_ERROR(bsIndex, "Migration SQL failed: %s", errMsg ? errMsg : "unknown");
            sqlite3_free(errMsg);
            return false;
        }
        return true;
    };

    if (current < 2 && targetVersion >= 2) {
        LOG_INFO(bsIndex, "Applying schema migration 1 -> 2");

        if (!exec(R"(
            CREATE TABLE IF NOT EXISTS interactions (
                id            INTEGER PRIMARY KEY AUTOINCREMENT,
                query         TEXT NOT NULL,
                query_normalized TEXT NOT NULL,
                item_id       INTEGER NOT NULL REFERENCES items(id) ON DELETE CASCADE,
                path          TEXT NOT NULL,
                match_type    TEXT NOT NULL,
                result_position INTEGER NOT NULL,
                app_context   TEXT,
                timestamp     TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
            );
        )")) {
            return false;
        }

        if (!exec("CREATE INDEX IF NOT EXISTS idx_interactions_query ON interactions(query_normalized);")
            || !exec("CREATE INDEX IF NOT EXISTS idx_interactions_item ON interactions(item_id);")
            || !exec("CREATE INDEX IF NOT EXISTS idx_interactions_timestamp ON interactions(timestamp);")) {
            return false;
        }

        if (!exec("INSERT OR REPLACE INTO settings (key, value) VALUES ('schema_version', '2');")) {
            return false;
        }

        current = 2;
    }

    if (current != targetVersion) {
        LOG_ERROR(bsIndex, "Schema migration incomplete: current=%d target=%d",
                  current, targetVersion);
        return false;
    }

    LOG_INFO(bsIndex, "Schema migrations complete: version %d", current);
    return true;
}

} // namespace bs
