#pragma once

namespace bs {

// Per-connection pragmas — no write lock required, safe on every open.
// busy_timeout is set high (30 s) so a second process (e.g. QueryService)
// waits out any long batch transaction held by the indexer.
constexpr const char* kConnectionPragmas = R"(
PRAGMA busy_timeout = 30000;
PRAGMA synchronous = NORMAL;
PRAGMA foreign_keys = ON;
PRAGMA wal_autocheckpoint = 10000;
PRAGMA cache_size = -65536;
PRAGMA journal_size_limit = 33554432;
PRAGMA mmap_size = 30000000;
)";

// Database-level pragmas — require write lock, run once when creating the DB.
constexpr const char* kDatabasePragmas = R"(
PRAGMA journal_mode = WAL;
PRAGMA application_id = 0x425354;
PRAGMA user_version = 1;
)";

// Schema v1 CREATE statements (doc 04 Section 3, canonical)
constexpr const char* kSchemaV1 = R"(
CREATE TABLE IF NOT EXISTS items (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    path TEXT NOT NULL UNIQUE,
    name TEXT NOT NULL,
    extension TEXT,
    kind TEXT NOT NULL,
    size INTEGER NOT NULL DEFAULT 0,
    created_at REAL NOT NULL,
    modified_at REAL NOT NULL,
    indexed_at REAL NOT NULL,
    content_hash TEXT,
    classification TEXT,
    sensitivity TEXT NOT NULL DEFAULT 'normal',
    is_pinned INTEGER NOT NULL DEFAULT 0,
    parent_path TEXT
);

CREATE INDEX IF NOT EXISTS idx_items_parent_path ON items(parent_path);
CREATE INDEX IF NOT EXISTS idx_items_kind ON items(kind);
CREATE INDEX IF NOT EXISTS idx_items_modified_at ON items(modified_at DESC);
CREATE INDEX IF NOT EXISTS idx_items_name ON items(name);
CREATE INDEX IF NOT EXISTS idx_items_sensitivity ON items(sensitivity);
CREATE INDEX IF NOT EXISTS idx_items_indexed_at ON items(indexed_at DESC);

CREATE TABLE IF NOT EXISTS content (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    item_id INTEGER NOT NULL REFERENCES items(id) ON DELETE CASCADE,
    chunk_index INTEGER NOT NULL,
    chunk_text TEXT NOT NULL,
    chunk_hash TEXT NOT NULL,
    UNIQUE(item_id, chunk_index)
);

CREATE INDEX IF NOT EXISTS idx_content_item_id ON content(item_id);
CREATE INDEX IF NOT EXISTS idx_content_chunk_hash ON content(chunk_hash);

CREATE TABLE IF NOT EXISTS tags (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    item_id INTEGER NOT NULL REFERENCES items(id) ON DELETE CASCADE,
    tag TEXT NOT NULL,
    source TEXT NOT NULL DEFAULT 'system',
    UNIQUE(item_id, tag)
);

CREATE INDEX IF NOT EXISTS idx_tags_item_id ON tags(item_id);
CREATE INDEX IF NOT EXISTS idx_tags_tag ON tags(tag);
CREATE INDEX IF NOT EXISTS idx_tags_source ON tags(source);

CREATE TABLE IF NOT EXISTS failures (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    item_id INTEGER NOT NULL REFERENCES items(id) ON DELETE CASCADE,
    stage TEXT NOT NULL,
    error_message TEXT,
    failure_count INTEGER NOT NULL DEFAULT 1,
    first_failed_at REAL NOT NULL,
    last_failed_at REAL NOT NULL,
    UNIQUE(item_id, stage)
);

CREATE INDEX IF NOT EXISTS idx_failures_item_id ON failures(item_id);
CREATE INDEX IF NOT EXISTS idx_failures_stage ON failures(stage);
CREATE INDEX IF NOT EXISTS idx_failures_last_failed_at ON failures(last_failed_at DESC);

CREATE TABLE IF NOT EXISTS settings (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS feedback (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    item_id INTEGER NOT NULL REFERENCES items(id) ON DELETE CASCADE,
    action TEXT NOT NULL,
    query TEXT,
    result_position INTEGER,
    timestamp REAL NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_feedback_item_id ON feedback(item_id);
CREATE INDEX IF NOT EXISTS idx_feedback_action ON feedback(action);
CREATE INDEX IF NOT EXISTS idx_feedback_timestamp ON feedback(timestamp DESC);
CREATE INDEX IF NOT EXISTS idx_feedback_query ON feedback(query);

CREATE TABLE IF NOT EXISTS frequencies (
    item_id INTEGER PRIMARY KEY REFERENCES items(id) ON DELETE CASCADE,
    open_count INTEGER NOT NULL DEFAULT 0,
    last_opened_at REAL,
    total_interactions INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_frequencies_open_count ON frequencies(open_count DESC);
CREATE INDEX IF NOT EXISTS idx_frequencies_last_opened_at ON frequencies(last_opened_at DESC);

CREATE TABLE IF NOT EXISTS vector_map (
    item_id INTEGER PRIMARY KEY REFERENCES items(id) ON DELETE CASCADE,
    hnsw_label INTEGER NOT NULL UNIQUE,
    model_version TEXT NOT NULL,
    embedded_at REAL NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_vector_map_label ON vector_map(hnsw_label);

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

CREATE INDEX IF NOT EXISTS idx_interactions_query ON interactions(query_normalized);
CREATE INDEX IF NOT EXISTS idx_interactions_item ON interactions(item_id);
CREATE INDEX IF NOT EXISTS idx_interactions_timestamp ON interactions(timestamp);

CREATE VIRTUAL TABLE IF NOT EXISTS search_index USING fts5(
    file_name,
    file_path,
    content,
    chunk_id UNINDEXED,
    file_id UNINDEXED,
    tokenize = 'porter unicode61 remove_diacritics 2'
);
)";

// BM25 weight configuration (doc 04 Section 3.9)
constexpr const char* kFts5WeightConfig =
    "INSERT INTO search_index(search_index, rank) VALUES('rank', 'bm25(10.0, 5.0, 0.5)')";

// Default settings rows (doc 04 Section 10.1)
constexpr const char* kDefaultSettings = R"(
INSERT OR IGNORE INTO settings (key, value) VALUES ('schema_version', '2');
INSERT OR IGNORE INTO settings (key, value) VALUES ('last_full_index_at', '0');
INSERT OR IGNORE INTO settings (key, value) VALUES ('last_vacuum_at', '0');
INSERT OR IGNORE INTO settings (key, value) VALUES ('max_file_size', '104857600');
INSERT OR IGNORE INTO settings (key, value) VALUES ('extraction_timeout_ms', '5000');
INSERT OR IGNORE INTO settings (key, value) VALUES ('chunk_size_bytes', '4096');
INSERT OR IGNORE INTO settings (key, value) VALUES ('exactNameWeight', '200');
INSERT OR IGNORE INTO settings (key, value) VALUES ('prefixNameWeight', '150');
INSERT OR IGNORE INTO settings (key, value) VALUES ('containsNameWeight', '100');
INSERT OR IGNORE INTO settings (key, value) VALUES ('exactPathWeight', '90');
INSERT OR IGNORE INTO settings (key, value) VALUES ('prefixPathWeight', '80');
INSERT OR IGNORE INTO settings (key, value) VALUES ('contentMatchWeight', '0.6');
INSERT OR IGNORE INTO settings (key, value) VALUES ('fuzzyMatchWeight', '30');
INSERT OR IGNORE INTO settings (key, value) VALUES ('recencyWeight', '30');
INSERT OR IGNORE INTO settings (key, value) VALUES ('recencyDecayDays', '7');
INSERT OR IGNORE INTO settings (key, value) VALUES ('frequencyTier1Boost', '10');
INSERT OR IGNORE INTO settings (key, value) VALUES ('frequencyTier2Boost', '20');
INSERT OR IGNORE INTO settings (key, value) VALUES ('frequencyTier3Boost', '30');
INSERT OR IGNORE INTO settings (key, value) VALUES ('cwdBoostWeight', '25');
INSERT OR IGNORE INTO settings (key, value) VALUES ('appContextBoostWeight', '15');
INSERT OR IGNORE INTO settings (key, value) VALUES ('semanticWeight', '40');
INSERT OR IGNORE INTO settings (key, value) VALUES ('semanticSimilarityThreshold', '0.7');
INSERT OR IGNORE INTO settings (key, value) VALUES ('pinnedBoostWeight', '200');
INSERT OR IGNORE INTO settings (key, value) VALUES ('junkPenaltyWeight', '50');
INSERT OR IGNORE INTO settings (key, value) VALUES ('bm25WeightName', '10.0');
INSERT OR IGNORE INTO settings (key, value) VALUES ('bm25WeightPath', '5.0');
INSERT OR IGNORE INTO settings (key, value) VALUES ('bm25WeightContent', '0.5');
INSERT OR IGNORE INTO settings (key, value) VALUES ('lexicalWeight', '0.6');
INSERT OR IGNORE INTO settings (key, value) VALUES ('knnK', '50');
INSERT OR IGNORE INTO settings (key, value) VALUES ('efSearch', '50');
INSERT OR IGNORE INTO settings (key, value) VALUES ('embeddingBatchSize', '32');
INSERT OR IGNORE INTO settings (key, value) VALUES ('enableFeedbackLogging', '1');
INSERT OR IGNORE INTO settings (key, value) VALUES ('enableInteractionTracking', '1');
INSERT OR IGNORE INTO settings (key, value) VALUES ('enablePathPreferences', '1');
INSERT OR IGNORE INTO settings (key, value) VALUES ('enableFileTypeAffinity', '1');
INSERT OR IGNORE INTO settings (key, value) VALUES ('feedbackRetentionDays', '90');
INSERT OR IGNORE INTO settings (key, value) VALUES ('interactionRetentionDays', '180');
INSERT OR IGNORE INTO settings (key, value) VALUES ('nextHnswLabel', '0');
INSERT OR IGNORE INTO settings (key, value) VALUES ('hnswDeletedCount', '0');
INSERT OR IGNORE INTO settings (key, value) VALUES ('embeddingEnabled', '0');
INSERT OR IGNORE INTO settings (key, value) VALUES ('onboardingCompleted', '0');
INSERT OR IGNORE INTO settings (key, value) VALUES ('lastFeedbackAggregation', '0');
)";

constexpr int kCurrentSchemaVersion = 2;

} // namespace bs
