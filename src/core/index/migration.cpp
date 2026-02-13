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

    if (current < 3 && targetVersion >= 3) {
        LOG_INFO(bsIndex, "Applying schema migration 2 -> 3");

        if (!exec(R"(
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
        )")) {
            return false;
        }
        if (!exec("CREATE INDEX IF NOT EXISTS idx_vector_generation_active ON vector_generation_state(is_active);")) {
            return false;
        }

        if (!exec(R"(
            INSERT OR IGNORE INTO vector_generation_state (
                generation_id, model_id, dimensions, provider, state, progress_pct, is_active, updated_at
            ) VALUES ('v1', 'legacy', 384, 'cpu', 'active', 100.0, 1, strftime('%s','now'));
        )")) {
            return false;
        }

        if (!exec("INSERT OR REPLACE INTO settings (key, value) VALUES ('activeVectorGeneration', 'v1');")
            || !exec("INSERT OR REPLACE INTO settings (key, value) VALUES ('targetVectorGeneration', 'v2');")
            || !exec("INSERT OR REPLACE INTO settings (key, value) VALUES ('vectorMigrationState', 'idle');")
            || !exec("INSERT OR REPLACE INTO settings (key, value) VALUES ('vectorMigrationProgressPct', '0');")) {
            return false;
        }

        if (!exec("INSERT OR REPLACE INTO settings (key, value) VALUES ('schema_version', '3');")) {
            return false;
        }

        current = 3;
    }

    if (current < 4 && targetVersion >= 4) {
        LOG_INFO(bsIndex, "Applying schema migration 3 -> 4");

        if (!exec(R"(
            CREATE TABLE IF NOT EXISTS behavior_events_v1 (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                event_id TEXT NOT NULL UNIQUE,
                timestamp REAL NOT NULL,
                source TEXT NOT NULL,
                event_type TEXT NOT NULL,
                app_bundle_id TEXT,
                window_title_hash TEXT,
                item_path TEXT,
                item_id INTEGER REFERENCES items(id) ON DELETE SET NULL,
                browser_host_hash TEXT,
                input_meta TEXT,
                mouse_meta TEXT,
                privacy_flags TEXT,
                attribution_confidence REAL NOT NULL DEFAULT 0.0,
                context_event_id TEXT,
                activity_digest TEXT,
                created_at REAL NOT NULL
            );
        )")) {
            return false;
        }
        if (!exec("CREATE INDEX IF NOT EXISTS idx_behavior_events_ts ON behavior_events_v1(timestamp DESC);")
            || !exec("CREATE INDEX IF NOT EXISTS idx_behavior_events_item ON behavior_events_v1(item_id);")
            || !exec("CREATE INDEX IF NOT EXISTS idx_behavior_events_app ON behavior_events_v1(app_bundle_id);")) {
            return false;
        }

        if (!exec(R"(
            CREATE TABLE IF NOT EXISTS training_examples_v1 (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                sample_id TEXT NOT NULL UNIQUE,
                created_at REAL NOT NULL,
                query TEXT,
                query_normalized TEXT NOT NULL,
                item_id INTEGER NOT NULL REFERENCES items(id) ON DELETE CASCADE,
                path TEXT NOT NULL,
                label INTEGER,
                weight REAL NOT NULL DEFAULT 1.0,
                features_json TEXT NOT NULL,
                source_event_id TEXT,
                app_bundle_id TEXT,
                context_event_id TEXT,
                activity_digest TEXT,
                attribution_confidence REAL NOT NULL DEFAULT 0.0,
                consumed INTEGER NOT NULL DEFAULT 0
            );
        )")) {
            return false;
        }
        if (!exec("CREATE INDEX IF NOT EXISTS idx_training_examples_query ON training_examples_v1(query_normalized);")
            || !exec("CREATE INDEX IF NOT EXISTS idx_training_examples_item ON training_examples_v1(item_id);")
            || !exec("CREATE INDEX IF NOT EXISTS idx_training_examples_label ON training_examples_v1(label, consumed, created_at);")
            || !exec("CREATE INDEX IF NOT EXISTS idx_training_examples_created ON training_examples_v1(created_at DESC);")) {
            return false;
        }

        if (!exec(R"(
            CREATE TABLE IF NOT EXISTS replay_reservoir_v1 (
                slot INTEGER PRIMARY KEY,
                sample_id TEXT NOT NULL,
                label INTEGER NOT NULL,
                weight REAL NOT NULL DEFAULT 1.0,
                features_json TEXT NOT NULL,
                query_normalized TEXT,
                item_id INTEGER,
                created_at REAL NOT NULL
            );
        )")) {
            return false;
        }

        if (!exec(R"(
            CREATE TABLE IF NOT EXISTS learning_model_state_v1 (
                key TEXT PRIMARY KEY,
                value TEXT NOT NULL
            );
        )")) {
            return false;
        }

        if (!exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('behaviorStreamEnabled', '0');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('learningEnabled', '0');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('behaviorCaptureAppActivityEnabled', '1');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('behaviorCaptureInputActivityEnabled', '1');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('behaviorCaptureSearchEventsEnabled', '1');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('behaviorCaptureWindowTitleHashEnabled', '1');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('behaviorCaptureBrowserHostHashEnabled', '1');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerRolloutMode', 'instrumentation_only');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerHealthWindowDays', '7');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerRecentCycleHistoryLimit', '50');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerPromotionGateMinPositives', '80');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerPromotionMinAttributedRate', '0.5');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerPromotionMinContextDigestRate', '0.1');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerPromotionLatencyUsMax', '2500');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerPromotionLatencyRegressionPctMax', '35');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerPromotionPredictionFailureRateMax', '0.05');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerPromotionSaturationRateMax', '0.995');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('behaviorRawRetentionDays', '30');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('learningIdleCpuPctMax', '35');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('learningMemMbMax', '256');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('learningThermalMax', '2');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('learningPauseOnUserInput', '1');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerBlendAlpha', '0.15');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerNegativeSampleRatio', '3.0');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerMaxTrainingBatchSize', '1200');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerReplayCapacity', '4000');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerMinExamples', '120');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerFreshTrainingLimit', '1200');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerReplaySampleLimit', '1200');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerEpochs', '3');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerLearningRate', '0.05');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerL2', '0.0001');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerNegativeStaleSeconds', '30');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerReplaySeenCount', '0');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerCyclesRun', '0');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerCyclesSucceeded', '0');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerCyclesRejected', '0');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerLastCycleStatus', 'never_run');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerLastCycleReason', '');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerLastCycleAtMs', '0');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerLastActiveLoss', '0');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerLastCandidateLoss', '0');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerLastActiveLatencyUs', '0');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerLastCandidateLatencyUs', '0');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerLastActivePredictionFailureRate', '0');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerLastCandidatePredictionFailureRate', '0');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerLastActiveSaturationRate', '0');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerLastCandidateSaturationRate', '0');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerLastSampleCount', '0');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerLastPromoted', '0');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerLastManual', '0');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerActiveVersion', '');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerFallbackMissingModel', '0');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerFallbackLearningDisabled', '0');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerFallbackResourceBudget', '0');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerFallbackRolloutMode', '0');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerLastPruneAtMs', '0');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerCoreMlReady', '0');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerCoreMlInitError', '');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('learningDenylistApps', '[]');")) {
            return false;
        }

        if (!exec("INSERT OR REPLACE INTO settings (key, value) VALUES ('schema_version', '4');")) {
            return false;
        }

        current = 4;
    }

    if (current != targetVersion) {
        LOG_ERROR(bsIndex, "Schema migration incomplete: current=%d target=%d",
                  current, targetVersion);
        return false;
    }

    if (current >= 4) {
        if (!exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerPromotionLatencyUsMax', '2500');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerPromotionLatencyRegressionPctMax', '35');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerPromotionPredictionFailureRateMax', '0.05');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerPromotionSaturationRateMax', '0.995');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerLastActiveLatencyUs', '0');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerLastCandidateLatencyUs', '0');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerLastActivePredictionFailureRate', '0');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerLastCandidatePredictionFailureRate', '0');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerLastActiveSaturationRate', '0');")
            || !exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('onlineRankerLastCandidateSaturationRate', '0');")) {
            return false;
        }
    }

    LOG_INFO(bsIndex, "Schema migrations complete: version %d", current);
    return true;
}

} // namespace bs
