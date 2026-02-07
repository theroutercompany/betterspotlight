#pragma once

struct sqlite3;

namespace bs {

// Check current schema version and apply migrations as needed (doc 04 Section 8)
bool applyMigrations(sqlite3* db, int targetVersion);

// Read the current schema_version from the settings table.
// Returns 0 if the table does not exist yet (fresh database).
int currentSchemaVersion(sqlite3* db);

} // namespace bs
