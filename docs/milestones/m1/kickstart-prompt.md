# BetterSpotlight M1 Development Kickstart

**For: Lead Engineers + AI-Assisted Development ( Code / Sub-Agents)**
**Date: 2026-02-06**
**Scope: M1 - Basic Spotlight Replacement**

---

## How To Use This Prompt

This document is designed to be fed directly to  Code (or similar AI coding assistants) as a master prompt. It defines **10 independent work streams** organized into 4 waves. Each work stream can be assigned to a separate sub-agent running in parallel. The dependency constraints between waves must be respected, but all streams within a wave are independent.

**Execution strategy:**
1. Read the referenced documentation files BEFORE writing any code
2. Launch all sub-agents within a wave simultaneously
3. Wait for a wave to complete before starting the next wave
4. Each sub-agent produces a self-contained module with unit tests
5. Integration happens in Wave 4

---

## Project Context

BetterSpotlight is a macOS desktop file search application targeting power users and prosumers. It replaces/supplements macOS Spotlight with faster indexing, full-text search via FTS5, configurable exclusion rules, and deterministic ranking. It is written in **Qt 6.10+ / C++** (not Swift), uses **SQLite FTS5** for lexical search, **FSEvents** for file monitoring, and **Unix domain sockets** for IPC between isolated service processes.

**Distribution model:** Proprietary dual-license (free personal use with limited features, commercial license for full functionality). Not open source. All dependencies must be compatible with proprietary distribution.

**Target:** macOS 12.0+, Universal binary (arm64 + x86_64).

---

## Documentation Suite (READ BEFORE CODING)

Every sub-agent MUST read its referenced docs before writing any code. The docs are authoritative.

| Doc | Path | Summary |
|-----|------|---------|
| Architecture Overview | `../../foundation/architecture-overview.md` | System layers, data flows, resolved decisions |
| ADR-001: Qt/C++ | `docs/adrs/adr-001-qt-cpp-over-swift.md` | Why Qt 6/C++ over Swift |
| ADR-002: FTS5 | `docs/adrs/adr-002-fts5-lexical-search.md` | Why FTS5 over Xapian/Tantivy |
| ADR-003: FSEvents | `docs/adrs/adr-003-fsevents-file-watching.md` | Why FSEvents over kqueue/polling |
| ADR-004: Process Isolation | `docs/adrs/adr-004-process-isolation.md` | Why Unix sockets over XPC/gRPC |
| ADR-005: Tesseract | `docs/adrs/adr-005-tesseract-ocr.md` | Why Tesseract over Vision framework |
| ADR-006: PDF Library | `docs/adrs/adr-006-pdf-library-selection.md` | Poppler (dev) / PDFium (release) |
| Indexing Pipeline | `../../foundation/indexing-pipeline.md` | 8-stage pipeline with C++ code samples |
| Storage Schema | `../../foundation/storage-schema.md` | 8 tables + FTS5, canonical SQL, migrations |
| IPC Boundaries | `../../foundation/ipc-service-boundaries.md` | Service protocols, message format, lifecycle |
| Ranking & Scoring | `../../foundation/ranking-scoring.md` | Formula, weights, match types, context signals |
| Dependency Audit | `../../operations/dependency-audit.md` | All deps with licensing analysis |
| Build System | `../../foundation/build-system.md` | CMake config, targets, packaging |
| Security | `../../foundation/security-data-handling.md` | Data scope, threat model, sensitive paths |
| Migration Mapping | `../../operations/migration-mapping.md` | Swift scaffold to Qt/C++ module map |
| Milestone Criteria | `../acceptance-criteria.md` | M1 pass/fail gates |

---

## Key Architectural Decisions (Non-Negotiable)

These are settled. Do not re-litigate or choose alternatives.

1. **Language/Framework:** Qt 6.10+ / C++ (CMake build system)
2. **Full-text search:** SQLite FTS5, table named `search_index`, tokenizer `porter unicode61 remove_diacritics 2`
3. **File monitoring:** FSEvents C API (CoreServices framework)
4. **IPC:** Unix domain sockets at `/tmp/betterspotlight-{uid}/{service}.sock`, length-prefixed JSON messages
5. **Process model:** 3 service binaries (IndexerService, ExtractorService, QueryService) + 1 UI app
6. **PDF extraction:** Poppler for development, PDFium for release builds. PdfExtractor interface abstracts the backend.
7. **OCR:** Tesseract via libtesseract C API
8. **Exclusion rules:** `~/.bsignore` file (gitignore-style), 30+ default patterns
9. **Chunk size:** 500-2000 characters, 1000 default, no overlap for FTS5. Hash: SHA-256 of `filePath + "#" + chunkIndex`
10. **Ranking:** Deterministic scoring with 16 configurable weights. Formula: `finalScore = baseMatchScore + recencyBoost + frequencyBoost + contextBoost + pinnedBoost - junkPenalty`

---

## Build Target Structure

```
betterspotlight-core-shared    (static lib)  Tier 0  - shared types, enums, models
betterspotlight-core           (static lib)  Tier 1  - SQLiteStore, extractors, ranking, pipeline
betterspotlight-indexer        (executable)   Tier 2  - IndexerService binary
betterspotlight-extractor      (executable)   Tier 2  - ExtractorService binary
betterspotlight-query          (executable)   Tier 2  - QueryService binary
betterspotlight                (app bundle)   Tier 3  - Qt UI application
betterspotlight-tests          (test binary)  Tier 4  - unit + integration tests
```

---

## Wave 0: Foundation (All Parallel)

### Sub-Agent 0A: Project Scaffold + CMake Build System

**Read:** `../../foundation/build-system.md`, `../../operations/dependency-audit.md`

**Deliverables:**
- Complete directory structure matching doc 08's project layout
- Root `CMakeLists.txt` with project-level settings (C++17, macOS 12.0+ deployment target, universal binary)
- `CMakeLists.txt` for each build target (core-shared, core, indexer, extractor, query, app, tests)
- `FindONNXRuntime.cmake` custom module (M2 prep, stub for now)
- `cmake/` directory with toolchain files
- `data/default-bsignore.txt` with 30+ default exclusion patterns (gitignore syntax, to be copied to `~/.bsignore` on first run)
- `packaging/macos/Info.plist` and `packaging/macos/entitlements.plist` templates
- Verify: `cmake -B build && cmake --build build` compiles (with stub main.cpp files)

**Key constraints:**
- Qt 6.10+ found via `CMAKE_PREFIX_PATH`
- SQLite compiled with `-DSQLITE_ENABLE_FTS5=1 -DSQLITE_ENABLE_JSON1=1`
- Tesseract + Leptonica found via pkg-config
- Poppler found via pkg-config (dev builds only)
- ONNX Runtime is optional, gated by `BETTERSPOTLIGHT_WITH_ONNX` compile definition
- All libraries dynamically linked (LGPL/GPL compliance)

**Acceptance:** Clean cmake configure + build with no errors on macOS arm64. All targets produce binaries (even if stubbed).

---

### Sub-Agent 0B: Shared Types & Models Library

**Read:** `../../foundation/indexing-pipeline.md` (Stage data types), `../../foundation/ipc-service-boundaries.md` (protocol messages), `../../foundation/ranking-scoring.md` (scoring types), `../../foundation/storage-schema.md` (table row types)

**Deliverables:** `src/core/shared/` static library containing:

```
src/core/shared/
  types.h            - WorkItem, FileMetadata, ItemKind (enum), ValidationResult (enum)
  chunk.h            - Chunk struct, computeChunkId() function
  search_result.h    - SearchResult, QueryContext, MatchType (enum)
  scoring_types.h    - ScoringWeights (16 weights), ScoreBreakdown
  index_health.h     - IndexHealth (stats, queue depth, error count)
  settings.h         - Settings struct (all configurable parameters)
  ipc_messages.h     - Message base class, all request/response types for 3 services
  logging.h          - Structured logging macros (LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR)
```

**Key constraints from docs:**
- `ItemKind` enum: Text, Code, PDF, Image, Archive, Binary, Unknown (doc 03, Stage 4)
- `ValidationResult` enum: Include, Exclude, MetadataOnly (doc 03, Stage 3, note: MetadataOnly only for Sensitive paths, NOT cloud folders)
- `MatchType` enum: exactName(200), prefixName(150), containsName(100), exactPath(90), prefixPath(80), content(50), fuzzy(30) (doc 06)
- `WorkItem::Type` enum: Delete, ModifiedContent, NewFile, RescanDirectory (doc 03, Stage 2)
- `ScoringWeights`: all 16 weights from doc 06 with their default values
- IPC messages: length-prefixed JSON, 4-byte big-endian uint32 length header, then UTF-8 JSON payload (doc 05)
- `computeChunkId(filePath, chunkIndex)`: SHA-256 of `filePath + "#" + std::to_string(chunkIndex)` (doc 03, Stage 6)

**Acceptance:** Compiles as static library. All types have proper copy/move semantics. Header-only where appropriate.

---

### Sub-Agent 0C: SQLite Integration + Schema Bootstrap

**Read:** `../../foundation/storage-schema.md` (canonical, entire document), `../../foundation/indexing-pipeline.md` (Stage 7 FTS5 section)

**Deliverables:** `src/core/index/` module containing:

```
src/core/index/
  sqlite_store.h/.cpp    - SQLiteStore class wrapping all DB operations
  schema.h               - Schema version constant, CREATE TABLE statements
  migration.h/.cpp       - Schema migration logic (version table based)
```

**SQLiteStore interface (minimum):**
```cpp
class SQLiteStore {
public:
    bool open(const std::string& dbPath);
    void close();

    // Items CRUD
    int64_t insertItem(const FileMetadata& meta);
    bool updateItem(int64_t id, const FileMetadata& meta);
    bool deleteItem(int64_t id);
    std::optional<FileMetadata> getItemByPath(const std::string& path);

    // Content/Chunks
    bool insertChunks(int64_t itemId, const std::vector<Chunk>& chunks);
    bool deleteChunksForItem(int64_t itemId);

    // FTS5 (CRITICAL - this is what was missing in the Swift scaffold)
    bool indexInFts5(int64_t itemId, const FileMetadata& meta, const std::vector<Chunk>& chunks);
    bool removeFromFts5(const std::string& filePath);
    std::vector<SearchResult> searchFts5(const std::string& query, int limit);

    // Failures
    bool recordFailure(int64_t itemId, const std::string& stage, const std::string& error);

    // Frequencies
    bool recordAccess(int64_t itemId);
    int getAccessCount(int64_t itemId, int dayWindow);

    // Settings
    std::string getSetting(const std::string& key);
    bool setSetting(const std::string& key, const std::string& value);

    // Health
    IndexHealth getHealth();
};
```

**Key constraints from doc 04:**
- 8 tables: items, content, tags, failures, settings, feedback, frequencies, vector_map
- FTS5 virtual table named `search_index` with tokenizer `porter unicode61 remove_diacritics 2`
- FTS5 columns: file_name, file_path, content (fully indexed), chunk_id and file_id (UNINDEXED)
- BM25 weights: 10.0, 5.0, 1.0
- Pragmas: WAL mode, synchronous=NORMAL, foreign_keys=ON, journal_size_limit=67108864, cache_size=-65536, mmap_size=268435456
- Content storage is DIRECT (text stored in FTS5 rows, NOT contentless)
- Chunk hash separator is `#` (must match pipeline computation)

**CRITICAL INVARIANT (doc 03, Stage 7):** Every file that passes validation and reaches chunking MUST be inserted into FTS5 or be explicitly logged as failed. There must be no code path where extraction succeeds but FTS5 insertion is skipped. This was the bug that made the entire Swift scaffold non-functional.

**Acceptance:** Unit tests demonstrating: insert item + chunks + FTS5, search returns results, delete cascades correctly, FTS5 snippet extraction works, WAL mode confirmed, schema migration from v0 to v1.

---

## Wave 1: Core Modules (All Parallel, After Wave 0)

### Sub-Agent 1A: FSEvents + FileScanner + PathRules + .bsignore

**Read:** `../../foundation/indexing-pipeline.md` (Stages 1-3), `../../foundation/security-data-handling.md` (exclusion rules), ADR-003

**Deliverables:**
```
src/core/fs/
  file_monitor.h         - Platform-agnostic FileMonitor interface
  file_monitor_macos.h/.cpp  - FSEvents implementation
  file_scanner.h/.cpp    - Async directory walker
  path_rules.h/.cpp      - 30+ exclusion patterns, sensitivity classification, cloud detection
  bsignore_parser.h/.cpp - gitignore-style pattern parser for ~/.bsignore
```

**Key constraints:**
- FSEvents C API (CoreServices): `FSEventStreamCreate`, `FSEventStreamScheduleWithRunLoop`, latency 0.5s
- FileMonitor emits `WorkItem` structs with Type (Delete/ModifiedContent/NewFile/RescanDirectory)
- PathRules decision table (doc 03, Stage 3): .bsignore match -> Exclude, PathRules match -> Exclude, cloud artifact match -> FullIndex (NOT MetadataOnly), hidden -> Exclude, sensitive -> MetadataOnly, size > 5GB -> Exclude, otherwise -> Include
- Cloud folders (iCloud Drive, Dropbox, OneDrive, Google Drive) are FULLY INDEXED
- .bsignore at `~/.bsignore` only (single location), gitignore glob syntax, loaded at startup
- Sensitive paths: .ssh/, .gnupg/, .aws/, Library/Preferences/ (metadata only, no content extraction)
- 30+ default exclusion patterns: node_modules, .git objects, build dirs, caches, etc. (see doc 09)

**Acceptance:** Unit tests covering: FSEvents callback fires on file create/modify/delete, PathRules correctly classifies 20+ path patterns, .bsignore parser handles globs/negations/comments, cloud folders return Include (not MetadataOnly), sensitive paths return MetadataOnly.

---

### Sub-Agent 1B: Content Extraction (Text + PDF + OCR)

**Read:** `../../foundation/indexing-pipeline.md` (Stages 4-5), `../../foundation/ipc-service-boundaries.md` (ExtractorService protocol), ADR-005, ADR-006

**Deliverables:**
```
src/core/extraction/
  extractor.h            - FileExtractor interface (pure virtual)
  text_extractor.h/.cpp  - Plain text, code, markdown, config files
  pdf_extractor.h/.cpp   - Poppler backend (dev), PdfExtractor interface for swapping to PDFium
  ocr_extractor.h/.cpp   - Tesseract C API wrapper
  extraction_manager.h/.cpp - Coordinates extractors, concurrency control, timeout handling
```

**Key constraints:**
- TextExtractor handles 100+ file types via extension mapping (doc 03, Stage 5)
- PdfExtractor: 1000-page limit per file, timeout 30s, returns raw text per page
- OcrExtractor: Tesseract C API, English LSTM model (~25-30MB), requires image preprocessing (grayscale, binarization via Leptonica)
- ExtractionManager: max 4 concurrent extractions (configurable), 30-second timeout per file, retry up to 2 times on transient failure
- All extractors return `std::optional<std::string>` (nullopt on failure)
- PdfExtractor interface must be abstract enough that Poppler can be swapped for PDFium without changing callers

**Acceptance:** Unit tests: extract text from .txt/.md/.py/.json, extract text from a 10-page PDF, handle corrupt PDF gracefully (no crash, returns nullopt), OCR a simple image with known text. Integration: ExtractionManager respects concurrency limit, timeout fires correctly.

---

### Sub-Agent 1C: IPC Socket Layer

**Read:** `../../foundation/ipc-service-boundaries.md` (entire document), ADR-004

**Deliverables:**
```
src/core/ipc/
  socket_server.h/.cpp   - Unix domain socket server (binds, accepts, dispatches)
  socket_client.h/.cpp   - Unix domain socket client (connects, sends, receives)
  message.h/.cpp         - Length-prefixed JSON message encode/decode
  service_base.h/.cpp    - Base class for service processes (lifecycle, signal handling)
  supervisor.h/.cpp      - Monitors child services from UI process (heartbeat, restart)
```

**Key constraints from doc 05:**
- Socket path: `/tmp/betterspotlight-{uid}/{service}.sock` (uid = getuid())
- Message format: 4-byte big-endian uint32 length prefix + UTF-8 JSON payload
- Each message has: `id` (uint64, monotonic), `method` (string), `params` (object), and responses have `result` or `error`
- Heartbeat: UI sends `ping()` every 10s to each service, expects `pong()` within 5s
- Crash restart: exponential backoff (immediate, 1s, 2s, 4s, max 30s). Stop after 3 crashes in 60s.
- Service startup: bind socket -> send `ready` message -> begin processing
- Graceful shutdown: `shutdown()` message -> flush pending work -> close socket -> exit(0)
- Max message size: 16MB (for large extraction results)

**Acceptance:** Unit tests: encode/decode roundtrip for all message types, server accepts multiple clients, client reconnects after server restart. Integration: launch a mock service process, exchange ping/pong, simulate crash and verify supervisor restarts it with backoff.

---

### Sub-Agent 1D: Ranking & Scoring Engine

**Read:** `../../foundation/ranking-scoring.md` (entire document), `../../foundation/storage-schema.md` (frequencies table, settings table)

**Deliverables:**
```
src/core/ranking/
  scorer.h/.cpp          - BM25Ranker implementing full scoring formula
  context_signals.h/.cpp - CWD proximity, frontmost app detection (40+ bundle IDs)
  match_classifier.h/.cpp - Determines MatchType from query vs result
```

**Key constraints from doc 06:**
- Formula: `finalScore = baseMatchScore + recencyBoost + frequencyBoost + contextBoost + pinnedBoost - junkPenalty`
- 7 match types with base points: exactName(200), prefixName(150), containsName(100), exactPath(90), prefixPath(80), content(50), fuzzy(30)
- Recency boost: `recencyWeight * max(0, 1.0 - daysSinceModified / recencyHalfLifeDays)`
- Frequency boost: `frequencyWeight * log2(1 + accessCount30Days)`
- Context boost: CWD proximity (`cwdProximityWeight * (1.0 - depth / maxDepth)`), app-context (`appContextWeight` if file type matches frontmost app)
- Junk penalty: `junkPenaltyWeight` applied to files in known junk directories (node_modules, .cache, build/, etc.)
- `contentMatchWeight` is a POST-PROCESSING multiplier on FTS5 BM25 raw scores (not an FTS5 column weight)
- All 16 weights stored in `settings` table with defaults
- M1 context signals: CWD proximity + recency only. App-context detection is M1 stretch goal.

**Acceptance:** Unit tests verifying: exact name match scores higher than content match, recency boost decays correctly, frequency boost scales logarithmically, junk penalty reduces score, overall ordering matches expected ranking for 10+ test queries with known files.

---

## Wave 2: Pipeline + Services (All Parallel, After Wave 1)

### Sub-Agent 2A: Indexing Pipeline Orchestrator

**Read:** `../../foundation/indexing-pipeline.md` (entire document, especially Stages 6-7 and the CRITICAL STAGE warning)

**Deliverables:**
```
src/core/indexing/
  pipeline.h/.cpp        - Orchestrates all 8 stages
  work_queue.h/.cpp      - Priority queue with pause/resume, CPU throttling
  chunker.h/.cpp         - Text chunking (500-2000 chars, paragraph/sentence/word/char boundaries)
  indexer.h/.cpp         - Coordinates: metadata extraction -> content extraction -> chunking -> FTS5 indexing
```

**Key constraints:**
- WorkQueue: priority ordering (Created > Modified > Deleted > MetadataChanged), pause/resume, drain on shutdown
- Chunking (doc 03, Stage 6): target 1000 chars default, split at paragraph `\n\n` first, then sentence `. `, then word ` `, then force at 2000 chars. NO overlap for FTS5.
- Chunk ID: SHA-256 of `filePath + "#" + chunkIndex` (stable across re-extractions)
- Delta updates: compare chunk hashes, only re-index changed chunks
- **CRITICAL**: Every file passing Stage 3 and reaching Stage 6 MUST be inserted into FTS5 (`search_index` table) or logged as failed. Zero tolerance for silent drops.
- CPU throttling: when user is active (frontmost app is not BetterSpotlight), limit to < 50% of one core
- Batch FTS5 insertions: wrap in transactions, commit every 100 items

**Dependencies (from Wave 0/1):**
- `SQLiteStore` (Wave 0C) for database writes
- `FileMonitor` + `PathRules` (Wave 1A) for input events and validation
- `ExtractionManager` (Wave 1B) for content extraction
- `WorkItem`, `Chunk`, `FileMetadata` (Wave 0B) shared types

**Acceptance:** Integration test: create 100 test files in a temp directory, run pipeline, verify all 100 are in `items` table, all chunks in `content` table, all searchable via FTS5 `MATCH`. Then modify 10 files, re-run, verify only 10 are re-indexed. Then delete 5 files, verify removed from all tables.

---

### Sub-Agent 2B: Service Binaries (Indexer + Extractor + Query)

**Read:** `../../foundation/ipc-service-boundaries.md` (service protocols), `../../foundation/indexing-pipeline.md` (IndexerService responsibilities), `../../foundation/ranking-scoring.md` (QueryService responsibilities)

**Deliverables:**
```
src/services/indexer/main.cpp     - IndexerService entry point
src/services/extractor/main.cpp   - ExtractorService entry point
src/services/query/main.cpp       - QueryService entry point
```

Each service binary:
1. Inherits from `ServiceBase` (Wave 1C)
2. Binds its Unix socket
3. Dispatches incoming messages to handler methods
4. Sends responses back via the socket

**IndexerService methods (doc 05):**
- `startIndexing(roots: string[])` - begins full index of directories
- `pauseIndexing()` / `resumeIndexing()`
- `reindexPath(path: string)` - re-index single file/directory
- `rebuildAll()` - drop and rebuild entire index
- `getQueueStatus()` - returns queue depth, active items, error count
- Emits: `indexingProgress`, `indexingComplete`, `indexingError`

**ExtractorService methods (doc 05):**
- `extractText(path: string, kind: string)` - returns extracted text + metadata
- `extractMetadata(path: string)` - returns file metadata only
- `isSupported(extension: string)` - returns bool
- `cancelExtraction(path: string)` - cancels in-progress extraction
- Stateless: no persistent state, processes one request at a time

**QueryService methods (doc 05):**
- `search(query: string, limit: int, context: QueryContext)` - returns ranked SearchResults
- `getHealth()` - returns IndexHealth
- `recordFeedback(itemId: int, action: string, query: string, position: int)` - records user interaction

**Acceptance:** Each service starts, binds socket, responds to ping/pong, handles at least one real method call, shuts down cleanly on `shutdown()` message.

---

## Wave 3: UI + Integration (After Wave 2)

### Sub-Agent 3A: Qt UI Application

**Read:** `../../foundation/architecture-overview.md` (UI layer), `../../foundation/build-system.md` (app bundle), `../../operations/migration-mapping.md` (UI module mapping), `../acceptance-criteria.md` (M1 functional criteria)

**Deliverables:**
```
src/app/
  main.cpp               - Qt application entry point, service lifecycle management
  qml/Main.qml           - Root window
  qml/SearchPanel.qml    - Floating search panel (text input + results list)
  qml/ResultItem.qml     - Single result row (icon, name, path, snippet)
  qml/SettingsPanel.qml  - Settings view (indexing roots, hotkey, exclusions)
  qml/StatusBar.qml      - System tray icon with status indicator
  service_manager.h/.cpp - Spawns/monitors 3 service processes via Supervisor
  hotkey_manager.h/.cpp  - Global hotkey registration (Cmd+Space default, configurable)
  search_controller.h/.cpp - Bridges QML to QueryService IPC
```

**Key M1 UI requirements (doc 11):**
- Global hotkey (Cmd+Space) opens floating search panel over current workspace
- Search panel: text input at top, scrollable results list below, closes on Esc or focus loss
- Results update incrementally as user types (debounce 100ms)
- Keyboard navigation: up/down arrows, Enter to open file, Cmd+R to reveal in Finder, Cmd+Shift+C to copy path
- Status bar icon: idle (default), indexing (animated), error (red)
- Status bar click opens Index Health view
- Settings panel: configure indexed directories, hotkey, view .bsignore

**Performance (doc 11):**
- Metadata search P95 < 200ms
- Full-text search P95 < 500ms
- UI memory at rest < 80MB
- No memory growth > 2MB over 30-minute idle session

**Acceptance:** App launches, shows status bar icon, registers hotkey, opens search panel, sends search query to QueryService, displays results, opens file on Enter. Full end-to-end flow works.

---

### Sub-Agent 3B: Test Infrastructure + Integration Tests

**Read:** `../../foundation/indexing-pipeline.md` (Testing Strategy section), `../acceptance-criteria.md` (Testing Criteria)

**Deliverables:**
```
tests/
  unit/
    test_path_rules.cpp       - 20+ path classification tests
    test_scoring.cpp           - Ranking formula verification
    test_chunker.cpp           - Chunk boundary detection, stable IDs
    test_sqlite_store.cpp      - CRUD operations, FTS5 queries, migrations
    test_bsignore_parser.cpp   - Glob parsing, negation, comments
    test_ipc_messages.cpp      - Encode/decode roundtrip
  integration/
    test_full_pipeline.cpp     - Create -> Detect -> Extract -> Index -> Search
    test_crash_isolation.cpp   - Extractor crash doesn't kill UI
    test_index_persistence.cpp - Index survives app restart
    test_incremental_update.cpp - Modified files re-indexed within 5s
  fixtures/
    sample_documents/          - 50+ test files of various types (.txt, .md, .py, .pdf, .json, .cpp, etc.)
```

**Key test thresholds from doc 11:**
- FSEvents detection to searchable: < 5 seconds
- PathRules test coverage: > 80%
- Corrupt/malformed file handling: no crashes (PDF, truncated UTF-8, files > 500MB reference)
- FTS5 persistence across restarts
- Porter stemmer: "running" matches "run", "runs", "running"
- BM25 ordering: README files rank higher than files with "readme" in content

**Acceptance:** All unit tests pass. Integration tests demonstrate full create-detect-extract-index-search cycle. Crash isolation test proves extractor crash doesn't kill UI.

---

## Integration Points (Cross-Stream Contracts)

These are the critical interfaces where modules connect. Mismatches here cause integration failures.

| Producer | Consumer | Interface | Contract |
|----------|----------|-----------|----------|
| FileMonitor (1A) | Pipeline (2A) | `WorkItem` struct | Emitted via callback, contains path + type + known metadata |
| PathRules (1A) | Pipeline (2A) | `ValidationResult` enum | Include/Exclude/MetadataOnly. Cloud = Include, Sensitive = MetadataOnly |
| ExtractionManager (1B) | Pipeline (2A) | `std::optional<std::string>` | Full text or nullopt. Chunking happens in pipeline, NOT extractor. |
| Chunker (2A) | SQLiteStore (0C) | `std::vector<Chunk>` | Each chunk has text (500-2000 chars) + chunkId (SHA-256 of path#index) |
| SQLiteStore (0C) | QueryService (2B) | FTS5 MATCH query | Returns rowid + BM25 rank. QueryService applies Scorer post-processing. |
| Scorer (1D) | QueryService (2B) | `float computeScore()` | Takes SearchResult + QueryContext, returns final score. |
| IPC Layer (1C) | All Services (2B) | JSON messages | 4-byte length prefix + UTF-8 JSON. All messages have id, method, params/result/error. |
| Supervisor (1C) | UI App (3A) | Process lifecycle | Spawns services, monitors heartbeat, restarts on crash with exponential backoff. |

---

## M1 Performance Gates

These numbers come from doc 11. If any gate fails, M1 does not ship.

| Metric | Threshold | How to Measure |
|--------|-----------|----------------|
| Initial metadata scan (500K files) | < 10 minutes | Benchmark script with test corpus |
| Full content extraction (500K files) | < 60 minutes | Benchmark script with test corpus |
| Incremental update (1 file) | < 2 seconds | Timer from FSEvent to FTS5 searchable |
| Metadata search P95 | < 200ms | 1000-query benchmark |
| Full-text search P95 | < 500ms | 1000-query benchmark |
| Indexer memory (active) | < 200MB | Activity Monitor RSS |
| Indexer CPU (user active) | < 50% one core | Activity Monitor, throttled |
| UI memory at rest | < 80MB | Activity Monitor RSS |
| Memory drift (30 min idle) | < 2MB | RSS delta measurement |
| Database size (500K files) | < 2GB | stat on index.db |

---

## What Is NOT In M1 Scope

Do not build these. They are M2 or M3.

- Semantic/vector search (hnswlib, ONNX Runtime, embeddings)
- App-context detection (bundle ID matching)
- Clipboard monitoring
- Project graph inference
- Onboarding wizard
- Auto-update mechanism
- Custom themes or visual polish
- DOCX/XLSX/PPTX content extraction (text files, PDFs, code, and OCR only for M1)
- Encryption at rest (SQLCipher)
- Per-directory .bsignore files (only ~/.bsignore for M1)
- Multi-language stemming (English only for M1)

---

## Sub-Agent Parallelization Summary

```
Wave 0 (Foundation)          Wave 1 (Core Modules)         Wave 2 (Pipeline+Services)    Wave 3 (UI+Tests)
──────────────────          ─────────────────────         ────────────────────────      ─────────────────
┌──────────────┐            ┌──────────────────┐          ┌─────────────────────┐       ┌────────────────┐
│ 0A: CMake    │───────────>│ 1A: FSEvents +   │─────────>│ 2A: Indexing        │──────>│ 3A: Qt UI App  │
│    Scaffold  │            │     PathRules     │          │     Pipeline        │       │                │
└──────────────┘            └──────────────────┘          └─────────────────────┘       └────────────────┘
┌──────────────┐            ┌──────────────────┐          ┌─────────────────────┐       ┌────────────────┐
│ 0B: Shared   │───────────>│ 1B: Extraction   │─────────>│ 2B: Service         │──────>│ 3B: Tests +    │
│    Types     │            │     (Text+PDF)   │          │     Binaries        │       │    Integration │
└──────────────┘            └──────────────────┘          └─────────────────────┘       └────────────────┘
┌──────────────┐            ┌──────────────────┐
│ 0C: SQLite + │───────────>│ 1C: IPC Socket   │──────────────────┘
│    Schema    │            │     Layer        │
└──────────────┘            └──────────────────┘
                            ┌──────────────────┐
                            │ 1D: Ranking &    │──────────────────┘
                            │     Scoring      │
                            └──────────────────┘

Total sub-agents: 10 (3 + 4 + 2 + 2 per wave, minus overlap)
Max parallel at any point: 4 (Wave 1)
Estimated wall-clock with full parallelization: 4-6 days
```

---

## Final Notes for Engineers

1. **Read the docs first.** Every design decision has been documented and debated. If you disagree with something, raise it as feedback rather than silently diverging.

2. **The FTS5 indexing stage is the single most critical piece of code in the entire project.** The Swift scaffold had everything except this one INSERT statement, which made the entire app non-functional. The pipeline doc has a big red CRITICAL STAGE warning for a reason.

3. **Test with real files.** Unit tests with synthetic data are necessary but not sufficient. The integration tests must use actual PDFs, source code files, markdown, and images.

4. **Do not optimize prematurely.** Get the pipeline working end-to-end first, then profile. The performance gates are generous for M1.

5. **When in doubt, the doc wins.** If two pieces of code disagree, check the doc. If two docs disagree, check `../../foundation/storage-schema.md` (canonical for data) or `../../foundation/indexing-pipeline.md` (canonical for pipeline logic).
