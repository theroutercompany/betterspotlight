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

    // Future migrations go here as sequential if-blocks:
    // if (current < 2) { ... apply migration 1→2 ... }
    // if (current < 3) { ... apply migration 2→3 ... }

    LOG_INFO(bsIndex, "Schema is at version %d (target %d), no pending migrations",
             current, targetVersion);
    return true;
}

} // namespace bs
