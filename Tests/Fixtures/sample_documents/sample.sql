-- Sample SQL fixture for text extraction testing.
-- Mirrors BetterSpotlight's SQLite FTS5 schema from docs/04-storage-schema.md.

PRAGMA journal_mode = WAL;
PRAGMA foreign_keys = ON;

-- Core items table
CREATE TABLE IF NOT EXISTS items (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    path        TEXT    NOT NULL UNIQUE,
    filename    TEXT    NOT NULL,
    extension   TEXT,
    size_bytes  INTEGER NOT NULL DEFAULT 0,
    mtime       REAL    NOT NULL,
    item_type   TEXT    NOT NULL CHECK (item_type IN ('file', 'directory', 'symlink')),
    is_sensitive INTEGER NOT NULL DEFAULT 0,
    created_at  REAL    NOT NULL DEFAULT (julianday('now')),
    updated_at  REAL    NOT NULL DEFAULT (julianday('now'))
);

-- FTS5 virtual table for full-text search
CREATE VIRTUAL TABLE IF NOT EXISTS chunks_fts USING fts5(
    content,
    item_id UNINDEXED,
    chunk_index UNINDEXED,
    tokenize = 'unicode61 remove_diacritics 2'
);

-- Content chunks linked to items
CREATE TABLE IF NOT EXISTS chunks (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    item_id     INTEGER NOT NULL REFERENCES items(id) ON DELETE CASCADE,
    chunk_index INTEGER NOT NULL DEFAULT 0,
    content     TEXT    NOT NULL,
    char_count  INTEGER NOT NULL,
    UNIQUE(item_id, chunk_index)
);

-- Extraction failure tracking
CREATE TABLE IF NOT EXISTS failures (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    item_id     INTEGER NOT NULL REFERENCES items(id) ON DELETE CASCADE,
    error_code  TEXT    NOT NULL,
    error_msg   TEXT,
    retry_count INTEGER NOT NULL DEFAULT 0,
    last_retry  REAL,
    UNIQUE(item_id)
);

-- User feedback for ranking adjustments
CREATE TABLE IF NOT EXISTS feedback (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    item_id     INTEGER NOT NULL REFERENCES items(id) ON DELETE CASCADE,
    query       TEXT    NOT NULL,
    action      TEXT    NOT NULL CHECK (action IN ('click', 'pin', 'hide', 'boost')),
    timestamp   REAL    NOT NULL DEFAULT (julianday('now'))
);

-- Indexes for common query patterns
CREATE INDEX IF NOT EXISTS idx_items_filename ON items(filename);
CREATE INDEX IF NOT EXISTS idx_items_extension ON items(extension);
CREATE INDEX IF NOT EXISTS idx_items_mtime ON items(mtime DESC);
CREATE INDEX IF NOT EXISTS idx_chunks_item ON chunks(item_id);
CREATE INDEX IF NOT EXISTS idx_failures_item ON failures(item_id);
CREATE INDEX IF NOT EXISTS idx_feedback_item_query ON feedback(item_id, query);

-- Sample data insertion
INSERT INTO items (path, filename, extension, size_bytes, mtime, item_type)
VALUES
    ('/Users/test/docs/readme.md', 'readme.md', 'md', 2048, julianday('now'), 'file'),
    ('/Users/test/src/main.swift', 'main.swift', 'swift', 4096, julianday('now', '-1 day'), 'file'),
    ('/Users/test/.ssh/config', 'config', NULL, 512, julianday('now', '-30 days'), 'file');

-- Mark sensitive paths
UPDATE items SET is_sensitive = 1 WHERE path LIKE '%/.ssh/%';

-- Sample FTS search query
SELECT i.path, i.filename, snippet(chunks_fts, 0, '<b>', '</b>', '...', 32) AS snippet
FROM chunks_fts
JOIN chunks c ON chunks_fts.rowid = c.rowid
JOIN items i ON c.item_id = i.id
WHERE chunks_fts MATCH 'search query'
ORDER BY rank
LIMIT 20;
