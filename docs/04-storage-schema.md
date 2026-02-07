# BetterSpotlight Storage Schema Specification

**Version**: 1.0
**Last Updated**: 2026-02-06
**Database Engine**: SQLite 3.45+
**Location**: `~/Library/Application Support/BetterSpotlight/index.db`
**Scope**: Complete database schema, configuration, maintenance, and migration strategy for macOS file search indexing.

---

## 1. Overview

BetterSpotlight uses SQLite with FTS5 (Full-Text Search) as its local, single-machine index. The schema is optimized for:

- **High cardinality indexing**: Scale to 1M+ indexed files without performance degradation
- **Content searchability**: Chunked content storage with full-text search via FTS5
- **User interaction tracking**: Ranking signals via open frequency and user feedback
- **Reliability**: Crash-safe with WAL mode, Foreign Key constraints, and transaction safety
- **Health monitoring**: Failure tracking for extraction/indexing pipeline visibility

The database is **always local** (no replication), using NORMAL synchronous mode for optimal performance/safety tradeoff on a single machine.

---

## 2. Pragma Configuration

### 2.1 Initialization Script

All pragmas must be set when the database is first opened. This configuration is critical for correct behavior:

```sql
-- Enable Write-Ahead Logging for concurrent read/write
PRAGMA journal_mode = WAL;

-- NORMAL: good balance of safety and performance for local apps
--   Safer than FULL (no blocking), faster than DEFERRED
PRAGMA synchronous = NORMAL;

-- Enable foreign key constraint enforcement
PRAGMA foreign_keys = ON;

-- WAL-specific optimization: checkpoint every 10,000 pages (~40MB default page size)
PRAGMA wal_autocheckpoint = 10000;

-- Cache size: negative values = megabytes, -65536 = 64MB for fast lookups
PRAGMA cache_size = -65536;

-- Journal size limit: 32MB max for WAL files before auto-checkpoint
PRAGMA journal_size_limit = 33554432;

-- Memory-mapped I/O: 30MB mmap region for better sequential read performance
PRAGMA mmap_size = 30000000;

-- Optimize for faster queries
PRAGMA query_only = OFF;  -- Allow writes (default)

-- Strictness: tables must be created with explicit type definitions
-- (This is advisory; we follow good practices regardless)
PRAGMA application_id = 0x425354;  -- "BST" in hex: BetterSpotlight identifier
PRAGMA user_version = 1;           -- Schema version for migrations
```

### 2.2 Rationale

- **WAL mode**: Allows readers to access the database while writes are being applied. Critical for a search UI that must remain responsive during background indexing.
- **NORMAL synchronous**: Trades off one fsync() per transaction (vs FULL's two) for a safe-enough guarantee on modern filesystems. Acceptable for local data.
- **Foreign keys**: Enforce referential integrity; prevents orphaned content rows when items are deleted.
- **Memory mapping**: Improves sequential scan performance (common in FTS5 queries).
- **Cache size**: 64MB in-memory cache reduces disk I/O; appropriate for interactive desktop app.

---

## 3. Core Schema: CREATE TABLE Statements

### 3.1 Items Table (Core Metadata)

```sql
CREATE TABLE items (
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

-- Indexes for common queries
CREATE INDEX idx_items_parent_path ON items(parent_path);
CREATE INDEX idx_items_kind ON items(kind);
CREATE INDEX idx_items_modified_at ON items(modified_at DESC);
CREATE INDEX idx_items_name ON items(name);
CREATE INDEX idx_items_sensitivity ON items(sensitivity);
CREATE INDEX idx_items_indexed_at ON items(indexed_at DESC);
```

**Column Definitions:**

| Column | Type | Constraints | Description |
|--------|------|-------------|-------------|
| `id` | INTEGER | PRIMARY KEY AUTOINCREMENT | Stable internal identifier |
| `path` | TEXT | NOT NULL UNIQUE | Absolute file path; primary lookup key |
| `name` | TEXT | NOT NULL | Basename of file (display name) |
| `extension` | TEXT | NULL | File extension without dot (e.g., "pdf", "txt") |
| `kind` | TEXT | NOT NULL | **ItemKind**: directory, text, code, pdf, image, archive, binary, unknown |
| `size` | INTEGER | DEFAULT 0 | File size in bytes |
| `created_at` | REAL | NOT NULL | Unix timestamp (seconds since epoch); immutable after insert |
| `modified_at` | REAL | NOT NULL | Unix timestamp; updated by file watcher |
| `indexed_at` | REAL | NOT NULL | When extraction/chunking last completed |
| `content_hash` | TEXT | NULL | SHA-256 of extracted content; NULL if no content extracted |
| `classification` | TEXT | NULL | **FolderClassification** from parent directory analysis |
| `sensitivity` | TEXT | DEFAULT 'normal' | **Sensitivity**: normal, sensitive, hidden |
| `is_pinned` | INTEGER | DEFAULT 0 | 1 = user pinned (always ranks high in results) |
| `parent_path` | TEXT | NULL | Directory containing this item; denormalized for filtering |

**ItemKind Enum Values:**
- `directory`: Folder
- `text`: Plain text files (.txt, .md, .rst, etc.)
- `code`: Source code (.py, .js, .swift, .cpp, etc.)
- `pdf`: PDF documents
- `image`: Images (.jpg, .png, .gif, .svg, etc.)
- `archive`: Compressed files (.zip, .tar.gz, .7z, etc.)
- `binary`: Executable or other binary (.app, .o, .dylib, etc.)
- `unknown`: Unrecognized type

**Sensitivity Enum Values:**
- `normal`: Standard file, include in search results
- `sensitive`: Mark as sensitive but indexable (e.g., .env files in projects)
- `hidden`: Do not display in results; index for internal ranking only

---

### 3.2 Content Table (Chunked Text Extraction)

```sql
CREATE TABLE content (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    item_id INTEGER NOT NULL REFERENCES items(id) ON DELETE CASCADE,
    chunk_index INTEGER NOT NULL,
    chunk_text TEXT NOT NULL,
    chunk_hash TEXT NOT NULL,
    UNIQUE(item_id, chunk_index)
);

CREATE INDEX idx_content_item_id ON content(item_id);
CREATE INDEX idx_content_chunk_hash ON content(chunk_hash);
```

**Column Definitions:**

| Column | Type | Constraints | Description |
|--------|------|-------------|-------------|
| `id` | INTEGER | PRIMARY KEY | Unique row identifier (not used for FK, but helpful for pagination) |
| `item_id` | INTEGER | FOREIGN KEY → items(id) ON DELETE CASCADE | Parent file item |
| `chunk_index` | INTEGER | NOT NULL | 0-based chunk sequence number within file |
| `chunk_text` | TEXT | NOT NULL | Extracted text for this chunk (target 500-2000 chars, 1000 default; see [03-indexing-pipeline.md, Stage 6]) |
| `chunk_hash` | TEXT | NOT NULL | SHA-256 of `path + "#" + chunk_index` for stable IDs (must match pipeline computation) |

**Rationale for Chunking:**
- Large files (500MB+ documents, logs) are split into 500-2000 character chunks (1000 default) for granular FTS5 relevance
- Each chunk is indexed separately in FTS5 for granular relevance
- `chunk_hash` provides stable identifiers across database rebuilds
- ON DELETE CASCADE ensures cleanup when parent item is deleted

---

### 3.3 Tags Table (User and System Tags)

```sql
CREATE TABLE tags (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    item_id INTEGER NOT NULL REFERENCES items(id) ON DELETE CASCADE,
    tag TEXT NOT NULL,
    source TEXT NOT NULL DEFAULT 'system',
    UNIQUE(item_id, tag)
);

CREATE INDEX idx_tags_item_id ON tags(item_id);
CREATE INDEX idx_tags_tag ON tags(tag);
CREATE INDEX idx_tags_source ON tags(source);
```

**Column Definitions:**

| Column | Type | Constraints | Description |
|--------|------|-------------|-------------|
| `id` | INTEGER | PRIMARY KEY | Unique row identifier |
| `item_id` | INTEGER | FOREIGN KEY → items(id) ON DELETE CASCADE | Tagged item |
| `tag` | TEXT | NOT NULL | Tag label (e.g., "project:myapp", "language:python") |
| `source` | TEXT | DEFAULT 'system' | **Source**: 'system' (inferred) or 'user' (manually added) |

**Examples of System Tags:**
- `kind:code` (inferred from file extension)
- `extension:py` (inferred from file metadata)
- `language:python` (inferred from content analysis)
- `project:myapp` (inferred from directory structure)

**User Tags:**
- Added via UI; persistent across re-indexing
- Used for saved searches ("show me all #myproject files")

---

### 3.4 Failures Table (Indexing Health Tracking)

```sql
CREATE TABLE failures (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    item_id INTEGER NOT NULL REFERENCES items(id) ON DELETE CASCADE,
    stage TEXT NOT NULL,
    error_message TEXT,
    failure_count INTEGER NOT NULL DEFAULT 1,
    first_failed_at REAL NOT NULL,
    last_failed_at REAL NOT NULL,
    UNIQUE(item_id, stage)
);

CREATE INDEX idx_failures_item_id ON failures(item_id);
CREATE INDEX idx_failures_stage ON failures(stage);
CREATE INDEX idx_failures_last_failed_at ON failures(last_failed_at DESC);
```

**Column Definitions:**

| Column | Type | Constraints | Description |
|--------|------|-------------|-------------|
| `id` | INTEGER | PRIMARY KEY | Unique row identifier |
| `item_id` | INTEGER | FOREIGN KEY → items(id) ON DELETE CASCADE | Failed item |
| `stage` | TEXT | NOT NULL | **Stage**: 'extraction', 'chunking', 'indexing', 'embedding' |
| `error_message` | TEXT | NULL | Error description for diagnostics |
| `failure_count` | INTEGER | DEFAULT 1 | How many times this stage has failed |
| `first_failed_at` | REAL | NOT NULL | Unix timestamp of first failure |
| `last_failed_at` | REAL | NOT NULL | Unix timestamp of most recent attempt |

**Failure Stages:**
- `extraction`: Failed to read/parse file content (e.g., permission denied, unsupported format)
- `chunking`: Failed to split content into chunks (e.g., encoding error)
- `indexing`: Failed to insert into FTS5 (e.g., corrupt FTS5 index)
- `embedding`: Reserved for future ML-based embeddings

**Health Monitoring Use Cases:**
- Query failures where `failure_count > 3` → items to investigate
- Find recently-failed items → retry extraction with improved parsers
- Categorize errors by stage → tune extraction pipeline

---

### 3.5 Settings Table (Key-Value Configuration)

```sql
CREATE TABLE settings (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

-- No indexes needed; PK is primary lookup
```

**Column Definitions:**

| Column | Type | Constraints | Description |
|--------|------|-------------|-------------|
| `key` | TEXT | PRIMARY KEY | Configuration key (e.g., "last_full_index_at") |
| `value` | TEXT | NOT NULL | Configuration value (always TEXT; app parses as needed) |

**Standard Configuration Keys:**

| Key | Example Value | Purpose |
|-----|---------------|---------|
| `schema_version` | "1" | Current schema version (for migrations) |
| `last_full_index_at` | "1707225600.0" | Unix timestamp of last full filesystem scan |
| `last_vacuum_at` | "1707225600.0" | Last VACUUM operation |
| `index_paths` | "/Users/alice/Documents;/Users/alice/Code" | Semicolon-separated list of indexed paths |
| `exclude_patterns` | ".git;node_modules;.cache" | Semicolon-separated patterns to skip |
| `max_file_size` | "104857600" | Max file size to index (bytes; 100MB default) |
| `extraction_timeout_ms` | "5000" | Max milliseconds to spend extracting one file |
| `chunk_size_bytes` | "4096" | Target chunk size for content splitting |

---

### 3.6 Feedback Table (User Interaction Tracking)

```sql
CREATE TABLE feedback (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    item_id INTEGER NOT NULL REFERENCES items(id) ON DELETE CASCADE,
    action TEXT NOT NULL,
    query TEXT,
    result_position INTEGER,
    timestamp REAL NOT NULL
);

CREATE INDEX idx_feedback_item_id ON feedback(item_id);
CREATE INDEX idx_feedback_action ON feedback(action);
CREATE INDEX idx_feedback_timestamp ON feedback(timestamp DESC);
CREATE INDEX idx_feedback_query ON feedback(query);
```

**Column Definitions:**

| Column | Type | Constraints | Description |
|--------|------|-------------|-------------|
| `id` | INTEGER | PRIMARY KEY | Unique row identifier |
| `item_id` | INTEGER | FOREIGN KEY → items(id) ON DELETE CASCADE | Item user interacted with |
| `action` | TEXT | NOT NULL | **Action**: 'open', 'reveal', 'copy_path', 'dismiss', 'pin', 'unpin' |
| `query` | TEXT | NULL | Search query that led to this result (for ranking analysis) |
| `result_position` | INTEGER | NULL | Where in result list (1-indexed); NULL if pinned result |
| `timestamp` | REAL | NOT NULL | Unix timestamp of interaction |

**Action Types:**
- `open`: User double-clicked to open file
- `reveal`: User clicked "Reveal in Finder"
- `copy_path`: User clicked "Copy Path"
- `dismiss`: User dismissed result from dropdown (signal: low relevance)
- `pin`: User pinned item to top of results
- `unpin`: User un-pinned item

**Ranking Application:**
Frequently-opened files and files dismissed less often rank higher. Pinned files always rank at top.

---

### 3.7 Frequencies Table (Aggregated Open Counts)

```sql
CREATE TABLE frequencies (
    item_id INTEGER PRIMARY KEY REFERENCES items(id) ON DELETE CASCADE,
    open_count INTEGER NOT NULL DEFAULT 0,
    last_opened_at REAL,
    total_interactions INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX idx_frequencies_open_count ON frequencies(open_count DESC);
CREATE INDEX idx_frequencies_last_opened_at ON frequencies(last_opened_at DESC);
```

**Column Definitions:**

| Column | Type | Constraints | Description |
|--------|------|-------------|-------------|
| `item_id` | INTEGER | PRIMARY KEY FOREIGN KEY | Reference to items table |
| `open_count` | INTEGER | DEFAULT 0 | Total times this file was opened |
| `last_opened_at` | REAL | NULL | Unix timestamp of most recent open |
| `total_interactions` | INTEGER | DEFAULT 0 | Total interactions (open + reveal + copy) |

**Population:**
- Aggregate sums from `feedback` table daily (via maintenance job)
- Lightweight denormalization: faster ranking queries than JOIN with feedback

---

### 3.8 Vector Map Table (M2+ Vector Index Metadata)

```sql
-- Table 8: vector_map (maps items to hnswlib vector labels, M2+)
CREATE TABLE vector_map (
    item_id INTEGER PRIMARY KEY REFERENCES items(id) ON DELETE CASCADE,
    hnsw_label INTEGER NOT NULL UNIQUE,
    model_version TEXT NOT NULL,
    embedded_at REAL NOT NULL
);
CREATE INDEX idx_vector_map_label ON vector_map(hnsw_label);
```

**Column Definitions:**

| Column | Type | Constraints | Description |
|--------|------|-------------|-------------|
| `item_id` | INTEGER | PRIMARY KEY FOREIGN KEY | Reference to items table |
| `hnsw_label` | INTEGER | NOT NULL UNIQUE | Position label in hnswlib index |
| `model_version` | TEXT | NOT NULL | Version/hash of embedding model used |
| `embedded_at` | REAL | NOT NULL | Unix timestamp when embedding was computed |

**Purpose**: Maps indexed files to their positions in the hnswlib HNSW vector index. Only populated when embedding is enabled (M2+). CASCADE delete ensures embeddings are cleaned up when files are removed.

---

### 3.9 FTS5 Virtual Table (Canonical Definition)

> **This is the canonical FTS5 schema. All other documents reference this section.**

```sql
CREATE VIRTUAL TABLE IF NOT EXISTS search_index USING fts5(
    file_name,            -- BM25 weight 10.0 (fully indexed)
    file_path,            -- BM25 weight 5.0 (fully indexed)
    content,              -- BM25 weight 1.0 (fully indexed)
    chunk_id UNINDEXED,   -- stored for result identification only
    file_id UNINDEXED,    -- stored for FK resolution only
    tokenize = 'porter unicode61 remove_diacritics 2'
);

-- Set BM25 column weights
INSERT INTO search_index(search_index, rank)
VALUES('fts5', 'bm25(10.0, 5.0, 1.0)');
```

**Column Definitions:**

| Column | Type | Indexed | Purpose |
|--------|------|---------|---------|
| `file_name` | TEXT | Yes (BM25 10.0) | File basename (e.g., "report.pdf"). High weight so filename matches rank highest. |
| `file_path` | TEXT | Yes (BM25 5.0) | Full path (for context/navigation). Medium weight for project-name matches. |
| `content` | TEXT | Yes (BM25 1.0) | Chunked extracted text. Base weight; lower to reduce noise from large files. |
| `chunk_id` | TEXT | No (UNINDEXED) | SHA-256 hash of `filePath + "#" + chunkIndex` for stable identification. |
| `file_id` | INTEGER | No (UNINDEXED) | FK to items table for resolving search hits to files. |

**FTS5 Configuration:**

- **Content storage**: Direct (text stored in FTS5 rows). This supports `snippet()` and `highlight()` extraction for search result previews. Trades ~30% more disk space for simpler queries and full snippet support.
- **`tokenize = 'porter unicode61 remove_diacritics 2'`**:
  - `porter`: applies Porter stemming (e.g., "running", "runs", "run" all stem to "run")
  - `unicode61`: standard Unicode tokenizer (splits on whitespace/punctuation, case-folds)
  - `remove_diacritics 2`: always removes diacritics ("Cafe" matches "cafe" for better UX)

**BM25 Weights:**
- `file_name=10.0`: Strong boost for filename matches (user likely searching for "myfile.txt")
- `file_path=5.0`: Moderate boost for path matches (e.g., searching project name in path)
- `content=1.0`: Lower weight for content matches (reduce noise from large text files)

**Index Population:**
- Populated by C++ extraction pipeline when `content` rows are inserted (see [03-indexing-pipeline.md, Stage 7])
- Triggered via INSERT/REPLACE after chunking completes
- Queries use MATCH with Porter stemmer: `MATCH 'python* AND code*'`

---

## 4. Data Lifecycle

### 4.1 File Indexing (Insert/Update)

**Trigger: File System Event (New/Modified File)**

```
1. File watcher detects new/modified file at path /path/to/file
   ↓
2. Query items table: SELECT id FROM items WHERE path = ?
   ↓
3. If row exists AND content_hash unchanged → SKIP (no re-index)
   If row exists AND content_hash changed OR new file:
   ↓
4. BEGIN TRANSACTION
   ↓
5. INSERT OR REPLACE INTO items (
     path, name, extension, kind, size, created_at, modified_at, indexed_at,
     content_hash, classification, sensitivity, parent_path
   ) VALUES (...)
   ↓
6. DELETE FROM content WHERE item_id = ? (clear old chunks)
   DELETE FROM tags WHERE item_id = ? AND source = 'system' (clear system tags)
   ↓
7. Extract text → split into chunks
   FOR EACH chunk:
     INSERT INTO content (item_id, chunk_index, chunk_text, chunk_hash) VALUES (...)
     INSERT INTO search_index (file_name, file_path, content) VALUES (...)
   ↓
8. INSERT INTO tags (item_id, tag, source) VALUES ('system', ...) [inferred tags]
   ↓
9. DELETE FROM failures WHERE item_id = ? [clear past failures on success]
   ↓
10. COMMIT
```

**Failure Handling:**
- If extraction fails at step 7: INSERT/UPDATE failures table
- Retry logic: exponential backoff, max 3 retries per stage

### 4.2 File Deletion (Cascading Delete)

**Trigger: File Watcher Detects Deleted File**

```
1. DELETE FROM items WHERE path = ?
   ↓
2. Cascade delete via ON DELETE CASCADE:
   - All content rows for this item_id
   - All tag rows for this item_id
   - All failure rows for this item_id
   - All feedback rows for this item_id
   - frequencies row (if exists)
   - FTS5 index entry (manual trigger required)
```

**FTS5 Cleanup:**
- FTS5 does NOT auto-delete via CASCADE (virtual table limitation)
- Manually delete after main table delete:
  ```sql
  DELETE FROM search_index WHERE docid = ? AND file_path = ?;
  ```

### 4.3 User Actions (Feedback Recording)

**Trigger: User Opens File, Clicks Reveal, etc.**

```
1. User action detected (file opened, Reveal clicked, etc.)
   ↓
2. INSERT INTO feedback (item_id, action, query, result_position, timestamp)
   VALUES (?, ?, ?, ?, datetime('now'))
   ↓
3. Daily aggregation job (e.g., midnight):
   - SUM(action = 'open') → UPDATE frequencies.open_count
   - MAX(timestamp WHERE action = 'open') → UPDATE frequencies.last_opened_at
   - COUNT(*) → UPDATE frequencies.total_interactions
```

---

## 5. Consistency Guarantees

### 5.1 Crash Recovery (WAL Mode)

**Scenario: App crash during indexing**

With WAL mode enabled:

1. **Before crash**: Writes are accumulated in `-wal` file (not yet applied to main database)
2. **After restart**:
   - SQLite automatically replays WAL during next open
   - All complete transactions are applied
   - Incomplete transactions (at time of crash) are rolled back
3. **Result**: Database is consistent; no data loss of committed transactions

**Limitations:**
- Incomplete extractions (mid-chunking) are lost; file will be re-indexed on next index cycle
- This is acceptable: extracting a file is idempotent

### 5.2 Dirty Shutdown

**Scenario: Force quit (kill -9) or power loss**

With `PRAGMA synchronous = NORMAL`:

1. **Data guarantee**: Any complete transaction written to `-wal` file is safe
2. **Partial guarantee**: Last transaction in flight may be lost (but not earlier ones)
3. **Database integrity**: Always recoverable; WAL ensures atom transaction boundaries

**vs. PRAGMA synchronous = FULL:**
- FULL syncs to disk after every transaction (safer but slower)
- NORMAL syncs at checkpoint (good enough for local app with frequent backups)

---

## 6. Maintenance Operations

### 6.1 VACUUM: Reclaim Unused Space

**Purpose**: Defragment database, reclaim space from deleted rows

```sql
-- Full VACUUM (stops all readers for ~1-5 seconds depending on size)
VACUUM;

-- WAL-specific: perform checkpoint before VACUUM
PRAGMA wal_checkpoint(FULL);
VACUUM;
```

**Schedule**: Monthly or after bulk deletion (e.g., when removing project folder)

**Expected Space Reclaimed**: 20-40% for databases with high churn

### 6.2 FTS5 Optimize: Rebuild Inverted Index

**Purpose**: Consolidate FTS5 index for faster queries

```sql
-- Optimize FTS5 index
INSERT INTO search_index(search_index, rank) VALUES('fts5', 'optimize');
```

**Schedule**: After large bulk inserts (e.g., initial indexing of new directory)

**Query Impact**: MATCH queries ~10-20% faster after optimize

### 6.3 FTS5 Rebuild: Complete Index Reconstruction

**Purpose**: Fix corruption or improve index structure after significant schema changes

```sql
-- Backup first
VACUUM INTO '/backup/index_backup.db';

-- Rebuild FTS5 table
DROP TABLE IF EXISTS search_index_new;
CREATE VIRTUAL TABLE search_index_new USING fts5(
    file_name,
    file_path,
    content,
    chunk_id UNINDEXED,
    file_id UNINDEXED,
    tokenize = 'porter unicode61 remove_diacritics 2'
);

-- Repopulate from content table
INSERT INTO search_index_new (rowid, file_name, file_path, content)
SELECT c.id, i.name, i.path, c.chunk_text
FROM content c
JOIN items i ON c.item_id = i.id;

-- Set BM25 weights
INSERT INTO search_index_new(search_index, rank)
VALUES('fts5', 'bm25(10.0, 5.0, 1.0)');

-- Swap tables
DROP TABLE search_index;
ALTER TABLE search_index_new RENAME TO search_index;
```

**Schedule**: If corruption detected (rare), or annually as part of major version upgrade

### 6.4 Integrity Check: Detect Corruption

```sql
-- SQLite integrity check
PRAGMA integrity_check;
-- Returns 'ok' if database is sound; otherwise error messages

-- FTS5-specific check
INSERT INTO search_index(search_index) VALUES('integrity-check');
```

**Schedule**: Weekly (automated, non-blocking)

**Action**: If integrity_check fails:
1. Stop all indexing
2. Alert user to integrity issue
3. Offer rebuild from checkpoint/backup

### 6.5 WAL Checkpoint: Collapse Write-Ahead Log

**Purpose**: Move all changes from `-wal` file into main database file

```sql
-- Passive checkpoint: let readers finish, then checkpoint
PRAGMA wal_checkpoint(PASSIVE);

-- Restart checkpoint: interrupt readers, then checkpoint
PRAGMA wal_checkpoint(RESTART);

-- Full checkpoint: block all access until done
PRAGMA wal_checkpoint(FULL);
```

**Automatic**: Happens at `wal_autocheckpoint = 10000` (every ~40MB)

**Manual**: Call before archiving or backup:
```sql
PRAGMA wal_checkpoint(FULL);
-- Ensures main DB file contains all committed data
```

### 6.6 Aggregation Job: Daily Frequency Update

**Purpose**: Denormalize feedback data for faster ranking

**Frequency**: Nightly (e.g., 2 AM)

```sql
BEGIN TRANSACTION;

-- For each item with feedback, update frequencies
INSERT INTO frequencies (item_id, open_count, last_opened_at, total_interactions)
SELECT
    item_id,
    COALESCE((SELECT COUNT(*) FROM feedback WHERE item_id = f.item_id AND action = 'open'), 0),
    (SELECT MAX(timestamp) FROM feedback WHERE item_id = f.item_id AND action = 'open'),
    (SELECT COUNT(*) FROM feedback WHERE item_id = f.item_id)
FROM (SELECT DISTINCT item_id FROM feedback WHERE timestamp > datetime('now', '-1 day')) f
ON CONFLICT(item_id) DO UPDATE SET
    open_count = excluded.open_count,
    last_opened_at = excluded.last_opened_at,
    total_interactions = excluded.total_interactions;

-- Clean old feedback (keep 90 days for ranking learning)
DELETE FROM feedback WHERE timestamp < datetime('now', '-90 days');

COMMIT;
```

---

## 7. Size Estimates

### 7.1 Database Size Projections

Assuming average file metadata (150 bytes) + content extraction (2KB per file):

| File Count | Avg File Size | items table | content table | search_index | vector index | Total Size | Notes |
|-----------|---------------|------------|---------------|-------------|-------------|-----------|-------|
| 100K | 50KB | 15MB | 200MB | 180MB | 15-20MB | 410-415MB | Small-medium codebase |
| 500K | 50KB | 75MB | 1GB | 900MB | 75-100MB | 2.05-2.1GB | Large codebase + documents |
| 1M | 50KB | 150MB | 2GB | 1.8GB | 150-200MB | 4.1-4.15GB | Entire user home directory |

**Assumptions:**
- Average metadata row: 150 bytes
- Average extracted content: 2KB per file (includes images, PDFs, code, docs)
- FTS5 index: ~0.9x content table size (tokenization + compression)
- Vector index (vectors.hnsw): ~100-150 MB at 500K files with int8 quantized 384-dim embeddings (separate file, not in SQLite)
- Database file bloat: ~10% overhead

### 7.2 Query Performance Estimates

| Query Type | Index Used | Estimated Time | Note |
|-----------|-----------|----------------|------|
| Exact path lookup | `idx_items_path` (UNIQUE) | <1ms | Single row |
| Filename search (FTS5) | `search_index` | 5-50ms | Full-text match, 1M files |
| Recent files | `idx_items_modified_at` | 10-100ms | Range scan |
| Kind filter + FTS5 | `idx_items_kind` + `search_index` | 20-200ms | Combined index |
| Feedback aggregation | `idx_feedback_timestamp` | 100-500ms | Daily aggregation job |

### 7.3 Disk Usage Example: 500K Files

```
index.db (main)           ~1.2GB
index.db-wal              ~50-100MB (active during indexing)
index.db-shm              ~13MB (shared memory; transient)

Total persistent:         ~1.2GB
Total with active WAL:    ~1.35GB
```

---

## 8. Migration Strategy

### 8.1 Schema Versioning

All schema changes are tracked via `settings.schema_version`:

```sql
-- Initialize on first run
INSERT INTO settings (key, value) VALUES ('schema_version', '1');
```

### 8.2 Migration Execution Flow

**On App Startup:**

```
1. Open database
2. Read current_schema_version = SELECT value FROM settings WHERE key = 'schema_version'
3. Read app_schema_version = 1 (hardcoded in app)
4. If current_schema_version < app_schema_version:
   → Run migration scripts sequentially
   → Update schema_version after each migration
   → Log migration completion
5. If current_schema_version > app_schema_version:
   → Error: user attempted downgrade (unsupported)
```

### 8.3 Migration 1.0 → 1.1 Example: Add User Notes Column

**Migration Script: `migrations/001_add_notes.sql`**

```sql
-- Add optional notes column to items
ALTER TABLE items ADD COLUMN user_notes TEXT;

-- Update schema version
UPDATE settings SET value = '1.1' WHERE key = 'schema_version';

-- Checkpoint to ensure durable write
PRAGMA wal_checkpoint(FULL);
```

**C++ Migration Handler (pseudocode):**

```cpp
bool applyMigration(sqlite3* db, int fromVersion, int toVersion) {
    if (fromVersion == 1 && toVersion >= 2) {
        // Read migration SQL
        std::string migrationSQL = readResource("migrations/001_add_notes.sql");

        // Execute
        char* error = nullptr;
        int rc = sqlite3_exec(db, migrationSQL.c_str(), nullptr, nullptr, &error);

        if (rc != SQLITE_OK) {
            logError("Migration failed: " + std::string(error));
            sqlite3_free(error);
            return false;
        }

        logInfo("Migration 1→2 completed successfully");
        return true;
    }
    return true;
}
```

### 8.4 Zero-Downtime Migration Strategy

**For large databases (1M+ files), alterations can block:**

**Safe Pattern for Additions:**
```sql
-- Adding a column is fast (meta-only, no data rewrite)
ALTER TABLE items ADD COLUMN new_field TEXT;
-- Safe; no downtime needed
```

**Safe Pattern for Indexes:**
```sql
-- Creating indexes can be slow but non-blocking
CREATE INDEX idx_items_new_field ON items(new_field);
-- App remains responsive during creation
```

**Risky Pattern (Avoid):**
```sql
-- Removing columns requires table rebuild (blocks app)
ALTER TABLE items DROP COLUMN sensitive_field;
-- Not recommended without downtime window
```

### 8.5 Rollback Strategy

**Problem**: Schema downgrades not supported (app version mismatch)

**Solution: Backup Before Migrations**

```cpp
bool migrateDatabase(const std::string& dbPath) {
    // Step 1: Backup current database
    std::string backupPath = dbPath + ".pre-migration-v" + currentVersion();
    if (!copyFile(dbPath, backupPath)) {
        logError("Failed to backup database; aborting migration");
        return false;
    }

    // Step 2: Run migrations (if they fail, user can restore from backup)
    sqlite3* db;
    if (sqlite3_open(dbPath.c_str(), &db) != SQLITE_OK) {
        logError("Failed to open database");
        return false;
    }

    // Step 3: Apply migrations (on failure, user restores backup manually)
    bool success = applyMigrations(db);

    sqlite3_close(db);

    if (!success) {
        logError("Migration failed. Restore from: " + backupPath);
        return false;
    }

    return true;
}
```

### 8.6 Existing Migration Examples

**Migration 1.0 (Initial Release):**
- Creates all tables listed in section 3
- Populates initial settings (schema_version='1', etc.)
- Initializes FTS5 index

**Migration 1.1 (Hypothetical: Add Sensitivity Column):**
```sql
ALTER TABLE items ADD COLUMN sensitivity TEXT NOT NULL DEFAULT 'normal';
CREATE INDEX idx_items_sensitivity ON items(sensitivity);
UPDATE settings SET value = '1.1' WHERE key = 'schema_version';
PRAGMA wal_checkpoint(FULL);
```

**Migration 1.2 (Hypothetical: Remove Unused Column):**
```sql
-- SQLite doesn't support DROP COLUMN on WAL databases pre-3.35.0
-- Safer approach: create new table, copy data, swap
CREATE TABLE items_new (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    path TEXT NOT NULL UNIQUE,
    -- ... all columns except deprecated one
);
INSERT INTO items_new SELECT id, path, ... FROM items;
DROP TABLE items;
ALTER TABLE items_new RENAME TO items;
-- Recreate all indexes
CREATE INDEX idx_items_parent_path ON items(parent_path);
-- ... etc
UPDATE settings SET value = '1.2' WHERE key = 'schema_version';
PRAGMA wal_checkpoint(FULL);
```

---

## 9. Implementation Checklist

Use this checklist when implementing the storage layer:

- [ ] **Database Initialization**
  - [ ] Create `~/Library/Application Support/BetterSpotlight/` directory
  - [ ] Create `index.db` with all pragma settings (section 2)
  - [ ] Create all tables (section 3) in correct order (items → content → tags → failures → settings → feedback → frequencies → vector_map → FTS5)
  - [ ] Populate initial `settings` rows (schema_version, index_paths, etc.)

- [ ] **Core Operations**
  - [ ] Implement file insert/update logic with transaction wrapping (section 4.1)
  - [ ] Implement file delete with FTS5 cleanup (section 4.2)
  - [ ] Implement feedback recording (section 4.3)

- [ ] **FTS5 Integration**
  - [ ] Insert file_name, file_path, content into search_index after chunking
  - [ ] Implement MATCH queries with stemming (e.g., `MATCH 'python*'`)
  - [ ] Test column weight behavior (file_name=10.0 should rank higher)

- [ ] **Error Handling**
  - [ ] Populate failures table on extraction/chunking/indexing errors
  - [ ] Implement retry logic with exponential backoff
  - [ ] Log all SQL errors with context

- [ ] **Maintenance**
  - [ ] Implement daily aggregation job (frequencies update + old feedback cleanup)
  - [ ] Implement monthly VACUUM
  - [ ] Implement integrity check (weekly)
  - [ ] Implement WAL checkpoint after bulk operations

- [ ] **Migrations**
  - [ ] Create migration infrastructure (version table, runner)
  - [ ] Write migration 1.0 (initial schema)
  - [ ] Test migration flow on fresh and existing databases

- [ ] **Testing**
  - [ ] Test crash recovery (insert, kill process, restart → verify data intact)
  - [ ] Test FTS5 search with stemming and column weights
  - [ ] Benchmark queries at 100K, 500K, 1M file counts
  - [ ] Verify foreign key constraints prevent orphans
  - [ ] Test index rebuilds and integrity checks

---

## 10. SQL Reference: Quick Copy-Paste

### 10.1 Full Schema Creation Script

```sql
-- BetterSpotlight Schema v1.0
-- Run this script to initialize a new database

-- Pragmas
PRAGMA journal_mode = WAL;
PRAGMA synchronous = NORMAL;
PRAGMA foreign_keys = ON;
PRAGMA wal_autocheckpoint = 10000;
PRAGMA cache_size = -65536;
PRAGMA journal_size_limit = 33554432;
PRAGMA mmap_size = 30000000;
PRAGMA application_id = 0x425354;
PRAGMA user_version = 1;

-- Items Table
CREATE TABLE items (
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

CREATE INDEX idx_items_parent_path ON items(parent_path);
CREATE INDEX idx_items_kind ON items(kind);
CREATE INDEX idx_items_modified_at ON items(modified_at DESC);
CREATE INDEX idx_items_name ON items(name);
CREATE INDEX idx_items_sensitivity ON items(sensitivity);
CREATE INDEX idx_items_indexed_at ON items(indexed_at DESC);

-- Content Table
CREATE TABLE content (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    item_id INTEGER NOT NULL REFERENCES items(id) ON DELETE CASCADE,
    chunk_index INTEGER NOT NULL,
    chunk_text TEXT NOT NULL,
    chunk_hash TEXT NOT NULL,
    UNIQUE(item_id, chunk_index)
);

CREATE INDEX idx_content_item_id ON content(item_id);
CREATE INDEX idx_content_chunk_hash ON content(chunk_hash);

-- Tags Table
CREATE TABLE tags (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    item_id INTEGER NOT NULL REFERENCES items(id) ON DELETE CASCADE,
    tag TEXT NOT NULL,
    source TEXT NOT NULL DEFAULT 'system',
    UNIQUE(item_id, tag)
);

CREATE INDEX idx_tags_item_id ON tags(item_id);
CREATE INDEX idx_tags_tag ON tags(tag);
CREATE INDEX idx_tags_source ON tags(source);

-- Failures Table
CREATE TABLE failures (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    item_id INTEGER NOT NULL REFERENCES items(id) ON DELETE CASCADE,
    stage TEXT NOT NULL,
    error_message TEXT,
    failure_count INTEGER NOT NULL DEFAULT 1,
    first_failed_at REAL NOT NULL,
    last_failed_at REAL NOT NULL,
    UNIQUE(item_id, stage)
);

CREATE INDEX idx_failures_item_id ON failures(item_id);
CREATE INDEX idx_failures_stage ON failures(stage);
CREATE INDEX idx_failures_last_failed_at ON failures(last_failed_at DESC);

-- Settings Table
CREATE TABLE settings (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

-- Feedback Table
CREATE TABLE feedback (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    item_id INTEGER NOT NULL REFERENCES items(id) ON DELETE CASCADE,
    action TEXT NOT NULL,
    query TEXT,
    result_position INTEGER,
    timestamp REAL NOT NULL
);

CREATE INDEX idx_feedback_item_id ON feedback(item_id);
CREATE INDEX idx_feedback_action ON feedback(action);
CREATE INDEX idx_feedback_timestamp ON feedback(timestamp DESC);
CREATE INDEX idx_feedback_query ON feedback(query);

-- Frequencies Table
CREATE TABLE frequencies (
    item_id INTEGER PRIMARY KEY REFERENCES items(id) ON DELETE CASCADE,
    open_count INTEGER NOT NULL DEFAULT 0,
    last_opened_at REAL,
    total_interactions INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX idx_frequencies_open_count ON frequencies(open_count DESC);
CREATE INDEX idx_frequencies_last_opened_at ON frequencies(last_opened_at DESC);

-- Vector Map Table (M2+)
CREATE TABLE vector_map (
    item_id INTEGER PRIMARY KEY REFERENCES items(id) ON DELETE CASCADE,
    hnsw_label INTEGER NOT NULL UNIQUE,
    model_version TEXT NOT NULL,
    embedded_at REAL NOT NULL
);

CREATE INDEX idx_vector_map_label ON vector_map(hnsw_label);

-- FTS5 Virtual Table (canonical definition: Section 3.9)
CREATE VIRTUAL TABLE IF NOT EXISTS search_index USING fts5(
    file_name,            -- BM25 weight 10.0
    file_path,            -- BM25 weight 5.0
    content,              -- BM25 weight 1.0
    chunk_id UNINDEXED,
    file_id UNINDEXED,
    tokenize = 'porter unicode61 remove_diacritics 2'
);

-- Set BM25 weights
INSERT INTO search_index(search_index, rank)
VALUES('fts5', 'bm25(10.0, 5.0, 1.0)');

-- Initialize settings
INSERT INTO settings (key, value) VALUES ('schema_version', '1');
INSERT INTO settings (key, value) VALUES ('last_full_index_at', '0');
INSERT INTO settings (key, value) VALUES ('last_vacuum_at', '0');
INSERT INTO settings (key, value) VALUES ('index_paths', '/Users/username/Documents;/Users/username/Code');
INSERT INTO settings (key, value) VALUES ('exclude_patterns', '.git;node_modules;.cache;__pycache__;.DS_Store');
INSERT INTO settings (key, value) VALUES ('max_file_size', '104857600');
INSERT INTO settings (key, value) VALUES ('extraction_timeout_ms', '5000');
INSERT INTO settings (key, value) VALUES ('chunk_size_bytes', '4096');
```

### 10.2 Essential Query Patterns

**Search for files by name:**
```sql
SELECT i.id, i.path, i.name, i.kind
FROM items i
WHERE i.path LIKE ? OR i.name LIKE ?
ORDER BY i.modified_at DESC
LIMIT 20;
```

**Full-text search:**
```sql
SELECT DISTINCT i.id, i.path, i.name, i.kind
FROM search_index s
JOIN content c ON s.rowid = c.id
JOIN items i ON c.item_id = i.id
WHERE search_index MATCH ?
ORDER BY rank
LIMIT 20;
```

**Get frequently opened files:**
```sql
SELECT i.id, i.path, i.name, f.open_count
FROM items i
JOIN frequencies f ON i.id = f.item_id
WHERE f.open_count > 0
ORDER BY f.open_count DESC, f.last_opened_at DESC
LIMIT 10;
```

**Get recent items by kind:**
```sql
SELECT id, path, name, modified_at
FROM items
WHERE kind = ?
ORDER BY modified_at DESC
LIMIT 50;
```

**Track indexing failures:**
```sql
SELECT i.path, f.stage, f.error_message, f.failure_count, f.last_failed_at
FROM items i
JOIN failures f ON i.id = f.item_id
WHERE f.failure_count > 2
ORDER BY f.last_failed_at DESC;
```

---

## 11. Appendix: Glossary

| Term | Definition |
|------|-----------|
| **WAL** | Write-Ahead Logging; SQLite's crash-safe transaction journal |
| **FTS5** | Full-Text Search version 5; SQLite's built-in search engine |
| **BM25** | Ranking algorithm used by FTS5 (field-based relevance scoring) |
| **Contentless FTS5** | FTS5 table that stores only tokenized index, not original text |
| **Tokenization** | Breaking text into searchable tokens (words) |
| **Stemming** | Reducing words to root form (e.g., "running" → "run") |
| **Pragma** | SQLite configuration directive affecting database behavior |
| **Checkpoint** | Process of applying WAL changes to main database file |
| **Cascading Delete** | Automatic deletion of related rows when parent is deleted |
| **UNIQUE Constraint** | Database enforces no duplicate values in specified column(s) |
| **Index** | Data structure for fast lookups (B-tree) |
| **Vacuum** | Defragmentation operation; reclaims space from deleted rows |

---

## 12. Version History

| Version | Date | Changes |
|---------|------|---------|
| 1.0 | 2026-02-06 | Initial release; core schema with FTS5, WAL, feedback tracking |

---

**Document End**

*For questions or updates, contact the BetterSpotlight development team.*
