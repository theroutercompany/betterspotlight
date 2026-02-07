# Migration Mapping: Swift/SwiftUI → Qt 6/C++

**Document Purpose:** Maps the existing Swift scaffold (~75-80% implemented) to the planned Qt 6/C++ rewrite. Documents what transfers, what redesigns, what gets dropped, and known bugs to fix.

**Team Context:** 2-3 person team, lead fluent in C/C++, minimal Swift familiarity. Goal: carry forward DESIGNS AND DECISIONS, not code.

**Date:** February 2026
**Status:** Pre-Rewrite Architectural Document

---

## Executive Summary

The Swift scaffold is architecturally sound and ~75% complete, but has **three critical bugs** preventing functionality:

1. **IndexerService never writes to FTS5 index** (line 386 TODO)
2. **QueryService searches empty index** → all searches return nothing
3. **VectorIndex is placeholder-only** (in-memory, no persistence, no model)

The Qt 6/C++ rewrite should:
- **Adopt the architecture lock-in** (layered services, XPC-like IPC, modular extraction)
- **Fix the three critical bugs** (write to index, vector embedding pipeline)
- **Drop Apple-specific code** (Cocoa, NSPanel, FSEventStream, Carbon events)
- **Replace with Qt equivalents** (QWidget windows, QFileSystemWatcher, native hotkey libs)
- **Keep algorithm/design/data model logic** (FTS5 schema, BM25 scoring, context signals, chunking strategy)

**Estimated Total Effort:** Medium (≈6-8 weeks for experienced C++ team)

---

## Module-by-Module Migration Plan

### APP LAYER

#### BetterSpotlightApp.swift
**Current:** SwiftUI app entry point, lifecycle management, window orchestration
**Status:** ~70% complete

**Transfers to Qt:**
- App initialization sequence and lifecycle
- Multi-window model (main search panel + settings window + status icon)
- Onboarding flow logic (3-step sequence with permission checks)
- Global app state initialization (settings load, index health on startup)

**Redesign for Qt:**
- SwiftUI → Qt Widgets or QML (recommend QML for modern look)
- SwiftUI lifecycle hooks → Qt application signals (aboutToQuit, etc.)
- Use `QApplication` + `QMainWindow` / `QWidget` hierarchy
- System tray icon management via `QSystemTrayIcon`

**Dropped:**
- Swift concurrency primitives (use `std::thread`, `QThread`, or Qt thread pools)
- SwiftUI's state binding (use Qt model-view architecture instead)

**Bugs to Fix:**
- None specific to app layer; fixes to lower layers will solve app-level issues


#### OnboardingView.swift
**Current:** 3-step onboarding (Welcome → Permissions → Home Map)
**Status:** ~80% complete

**Transfers:**
- Logic flow (welcome → check permissions → show dashboard)
- Permission checks (`requestFoldersPermission()`, `requestMicrophonePermission()`)
- Completion marker (set in AppState)

**Redesign for Qt:**
- SwiftUI multi-step UI → Qt QML or custom QWidget wizard
- Use `QWizard` framework or custom state machine
- macOS system permission checks → `QSysInfo` + native macOS APIs via C++
- Dashboard display (index health visualization)

**Dropped:**
- SwiftUI's automatic state binding
- Native iOS-style transitions (not applicable for macOS desktop)

**Bugs to Fix:**
- None; onboarding is feature-complete


### UI STATE & SETTINGS

#### AppState.swift
**Current:** Central state manager with JSON file persistence
**Status:** ~80% complete

**Transfers Directly:**
- State model structure (all properties, enums, codecs)
- JSON serialization/deserialization logic
- Settings defaults (30+ exclusion patterns, cloud service detection)
- Path for persistent state (~/.config/betterspotlight/ or ~/Library/Application Support/)
- Singleton pattern (accessed from everywhere)

**Redesign for Qt:**
- Swift `Codable` → Qt's `QJsonDocument` + serialization helpers
- Or use modern C++ `nlohmann/json` library (smaller dependencies)
- Singleton → `QObject`-based singleton with signals/slots for observers
- File I/O → `QFile` + `QStandardPaths::ApplicationDataLocation`
- Thread-safe access → `QMutex` (AppState accessed from multiple threads)

**Dropped:**
- Swift's `@Published` reactive properties (use Qt signals instead)

**Known Bugs to Fix:**
- ✓ Health snapshot incorrectly shows zeros for root item counts (see Settings/IndexHealthView)
- Make state immutable or fully thread-safe with locking


#### SettingsView.swift + IndexHealthView.swift
**Current:** Settings UI with 5 tabs (General, Indexing, Exclusions, Privacy, Health) + health dashboard
**Status:** ~75% complete UI, health backend broken

**Transfers:**
- Tab structure and information architecture
- Settings validation rules
- Health metrics display concept (queue length, index size, extraction errors)
- Exclusion pattern editor UI flow

**Redesign for Qt:**
- SwiftUI tabs → `QTabWidget` or QML `TabBar` + `StackLayout`
- Table views for exclusions → `QTableView` + `QAbstractTableModel`
- Toggle switches → `QCheckBox` or `QSwitch` (Qt 6.2+)
- Statistics display → `QLabel` + custom layouts
- Pie charts/graphs → Qt Charts module or custom painting

**Dropped:**
- Native SwiftUI styling (replace with Qt stylesheets)
- Automatic form validation binding (implement manually with slot/signal)

**Bugs to Fix:**
- **CRITICAL:** Health snapshot shows zeros for root item count and queue length
  - Root cause: `IndexHealthView` queries database but gets invalid snapshots
  - Fix: Ensure health calculation queries correct SQLite views in C++ backend
- **CRITICAL:** Index rebuild button is stubbed (does nothing)
  - Fix: Implement full rebuild: clear FTS5 tables → rescan filesystem → re-extract → re-index
- **CRITICAL:** Cache clear button is stubbed
  - Fix: Implement vector embedding cache clear (when VectorIndex is properly implemented)


#### StatusBarController.swift
**Current:** NSStatusBar icon with health info popover
**Status:** ~70% complete

**Transfers:**
- Menu bar icon concept
- Health info display (queue length, last scan time)
- Quick access to search panel from status bar

**Redesign for Qt:**
- NSStatusBar → `QSystemTrayIcon`
- Context menu → `QMenu` + actions
- Popover → small `QWidget` tooltip or custom menu
- Status updates → signals from backend services

**Dropped:**
- NSStatusBar macOS-specific APIs
- Native macOS menu styling

**Bugs to Fix:**
- Health info accuracy depends on fixing AppState health snapshot


---

## CORE: FILESYSTEM & INDEXING

### Filesystem Operations

#### FSEvents.swift
**Current:** FSEventStream wrapper with C callbacks
**Status:** ~80% complete

**Transfers Directly:**
- Event filtering logic (file type patterns, exclusion rules)
- Debouncing strategy (buffer events, process in batches)
- Recursive directory monitoring concept

**Redesign for Qt:**
- FSEventStream (macOS-only) → `QFileSystemWatcher` (cross-platform)
  - `QFileSystemWatcher` is simpler but less efficient for large directory trees
  - Alternative: Use native macOS FSEvents via C++ wrapper + Qt signals
- C callbacks → Qt signal/slot mechanism
- Event queuing → `QQueue<FileChangeEvent>` + worker thread

**Dropped:**
- macOS-specific FSEventStream API details
- C callback machinery (use C++/Qt)

**Bugs to Fix:**
- None; FSEvents is solid design


#### PathRules.swift
**Current:** Pattern matching, sensitivity classification, cloud detection, 30+ defaults
**Status:** ~90% complete (well-tested)

**Transfers Directly:**
- All matching logic and algorithms
- 30+ default exclusion patterns (copy exactly)
- Cloud service detection patterns (Dropbox, iCloud, OneDrive, Google Drive)
- Sensitivity classifications (private, system, cache, temp)
- Test suite (13 tests in PathRulesTests.swift)

**Redesign for Qt:**
- Swift pattern matching → C++ `std::regex` or Qt `QRegularExpression`
- Keep logic identical; only replace syntax
- Tests: C++ Google Test framework

**Dropped:**
- Nothing; this is pure algorithm

**Bugs to Fix:**
- None; PathRules is mature


#### FileScanner.swift
**Current:** Async directory walker with exclusion support
**Status:** ~85% complete

**Transfers:**
- Walking algorithm and recursion logic
- Exclusion application at each level
- Symlink handling
- Batch emission of file lists
- Permission error handling (skip unreadable dirs)

**Redesign for Qt:**
- Async actor (`actor`) → `QThread` + work queue or `QThreadPool`
- Recursive `FileManager.contentsOfDirectory()` → `QDirIterator` or custom `QDir` recursion
- Structured concurrency patterns → explicit thread management

**Dropped:**
- Swift actor isolation (use explicit locks instead)

**Bugs to Fix:**
- None; FileScanner design is solid


### Extraction Pipeline

#### ExtractionManager.swift
**Current:** Actor-based extraction with concurrency control
**Status:** ~75% complete

**Transfers:**
- Extraction orchestration logic
- Concurrency control (max N extractors in flight)
- Error tracking and reporting
- Delegation to specialized extractors (PDF, OCR, Text)
- Chunking strategy (split large extracted text)
- XPC service delegation

**Redesign for Qt:**
- Swift actor → `QThreadPool` + work queue
- Semaphore concurrency control → `QSemaphore`
- Async/await patterns → explicit callbacks/signals or C++20 coroutines

**Dropped:**
- Swift structured concurrency

**Bugs to Fix:**
- None; ExtractionManager architecture is sound


#### TextExtractor.swift
**Current:** 100+ file type support (txt, markdown, code, etc.)
**Status:** ~80% complete

**Transfers Directly:**
- File type detection logic (UTType mapping)
- Extraction implementations for each type
- Fallback handling (if specialized extractor fails, try text read)
- Encoding detection (UTF-8, Latin-1, etc.)

**Redesign for Qt:**
- macOS UTType → Qt `QMimeType` + MIME database
- File reading → `QFile` + `QTextStream`
- Encoding detection → `QTextCodec` or `std::filesystem` hints

**Dropped:**
- macOS UTType system (replace with Qt MIME types)

**Bugs to Fix:**
- None; TextExtractor is straightforward


#### PdfExtractor.swift
**Current:** PDFKit text extraction, 1000 page limit, chunking
**Status:** ~85% complete

**Transfers Directly:**
- PDF text extraction algorithm
- Page-by-page processing loop
- 1000-page limit (memory safety)
- Chunking strategy (split text at paragraph boundaries)
- Error handling (corrupt PDFs, encryption)

**Redesign for Qt:**
- PDFKit (macOS) → Qt PDF module or external lib (`pdfium`, `MuPDF`, `poppler-cpp`)
  - Recommend: `pdfium` (Google, well-maintained, C++ API)
- Page iteration → library's iterator
- Text extraction → library's text API

**Dropped:**
- PDFKit macOS implementation

**Bugs to Fix:**
- None; PdfExtractor logic is solid


#### OcrExtractor.swift
**Current:** Vision framework OCR with text position ordering
**Status:** ~80% complete

**Transfers:**
- OCR text extraction strategy
- Text position ordering (reconstruct reading order from bounding boxes)
- Confidence filtering
- Image-to-text pipeline

**Redesign for Qt:**
- Vision framework → external OCR library
  - Recommend: **Tesseract** (open-source, C++ API, cross-platform)
    - Or: Apple's Vision framework via native C++ API (macOS-only)
    - Or: cloud service (Google Cloud Vision, Azure Computer Vision)
  - For MVP: Tesseract recommended (no external service dependency)
- Image loading → Qt `QImage` + `QPixmap`
- Text position ordering algorithm → keep identical (language-agnostic)

**Dropped:**
- Vision framework macOS implementation

**Bugs to Fix:**
- None; OCR extraction logic is sound


### Indexing

#### SQLiteStore.swift
**Current:** 7 normalized tables, WAL mode, full CRUD, failure tracking
**Status:** ~90% complete, schema/operations solid

**Transfers Directly:**
- Complete database schema (all 7 tables, all columns)
- WAL mode for concurrent reads
- All CRUD operations (insert, update, delete, select)
- Transaction handling
- Failure tracking (retry counts, error logs)
- Prepared statement patterns

**Redesign for Qt:**
- Apple SQLite → system SQLite (same engine)
- Swift database library → Qt `QSqlDatabase` + `QSqlQuery`
  - Or keep raw SQLite C API with Qt thread safety wrappers
  - Or use C++ wrapper library (`sqlitecpp`, `sqlpp11`)
  - Recommendation: Qt SQL module for cross-platform safety
- Connection pooling → `QSqlDatabase::addDatabase()` per thread

**Dropped:**
- Nothing; SQLite is universal

**Bugs to Fix:**
- ✓ **CRITICAL:** IndexerService never writes to FTS5 tables (line 386 TODO)
  - This is the root cause of empty search results
  - Fix: Ensure extraction pipeline → indexing pipeline → FTS5 write
  - See IndexerService details below


#### LexicalIndex.swift (FTS5)
**Current:** FTS5 full-text search with BM25, snippets, prefix/phrase queries
**Status:** ~85% complete schema, broken write path

**Transfers Directly:**
- FTS5 schema design (perfect for search)
- BM25 ranking function (FTS5 native)
- Query building logic (prefix, phrase, AND/OR)
- Snippet generation algorithm
- Prefix matching for autocomplete

**Redesign for Qt:**
- FTS5 is SQLite extension; use via C API or Qt SQL module
- Keep all query logic identical

**Dropped:**
- Nothing; FTS5 is universal

**Bugs to Fix:**
- ✓ **CRITICAL:** Table never receives writes
  - Root cause: IndexerService TODO at line 386
  - Fix: Wire extraction results → LexicalIndex.insert() calls
  - Verify inserts happen in XPC service context


#### VectorIndex.swift
**Current:** IN-MEMORY PLACEHOLDER ONLY
**Status:** ~10% complete, non-functional

**Transfers:**
- Ranking concept (embeddings + cosine similarity for semantic search)
- Metadata storage design (file path, kind, snippet)
- Integration points in scoring pipeline

**Redesign for Qt (MAJOR WORK):**
- **Choose embedding model:**
  - Option A: On-device (`sentence-transformers` via ONNX Runtime, ~150MB)
  - Option B: Cloud service (OpenAI, Cohere, etc.)
  - Recommendation: **ONNX Runtime** + BGE-small-en-v1.5 or E5-small-v2 class model (~50MB, 384-dim embeddings, int8 quantized, first-chunk-only)

- **Architecture:**
  - `hnswlib` (header-only C++, Apache 2.0) for HNSW approximate nearest neighbor vector index
    - Vectors stored in `vectors.hnsw` file; SQLite `vector_map` table maps items to hnsw labels
  - Embedding inference server (separate process) or in-process library
  - Batch embedding (collect texts, embed N at a time)
  - Persistence: SQLite BLOB column + KNN index metadata

- **C++ Implementation:**
  - ONNX Runtime C++ API for model loading/inference
  - `hnswlib` C++ header-only library for approximate nearest neighbors
  - SQLite wrapper for vector storage
  - Threading: embedding inference in thread pool, avoid blocking search UI

**Dropped:**
- In-memory only approach (must persist to disk)

**Bugs to Fix:**
- ✓ **CRITICAL:** Entire subsystem is placeholder
  - Scope: ~2-3 weeks for experienced team
  - High value: semantic search (e.g., "fast file searcher" finds "quick document lookup")
  - Start AFTER LexicalIndex working (phase 2)


---

## CORE: EXTRACTION SERVICES (XPC)

#### ExtractorService.swift
**Current:** XPC service wrapper around ExtractionManager
**Status:** ~90% complete

**Transfers:**
- Service protocol definition (interface)
- Delegation to ExtractionManager
- Error handling and reporting
- Concurrency patterns

**Redesign for Qt:**
- XPC (macOS-only) → **gRPC** or **Qt IPC** (message passing over sockets)
  - Recommendation: **gRPC** for language-agnostic RPC, or Qt's `QLocalSocket`/`QLocalServer`
  - For simplicity: keep as in-process library (no IPC needed if single process)
  - For security/isolation: gRPC + separate process

- Service entry point → executable with QLocalServer listener
- Protocol → `.proto` file (gRPC) or custom message format

**Dropped:**
- macOS XPC framework (use universal alternative)

**Bugs to Fix:**
- None specific to ExtractorService; see ExtractionManager


#### IndexerService.swift
**Current:** XPC service wrapper around indexing pipeline
**Status:** ~70% complete, has CRITICAL gap

**Transfers:**
- Service protocol definition
- Orchestration logic (receive extracted items → index them)
- Health tracking

**Redesign for Qt:**
- XPC → gRPC or Qt IPC
- Same architecture as ExtractorService

**Bugs to Fix:**
- ✓ **CRITICAL (line 386 TODO):** Never writes to FTS5 index
  - Root cause: extraction results reach IndexerService but `LexicalIndex.insert()` is never called
  - Impact: FTS5 remains empty, all searches return nothing
  - Fix steps:
    1. Ensure ExtractionManager emits results with full metadata
    2. IndexerService receives results
    3. Call `LexicalIndex.insert(item)` for each result
    4. Verify FTS5 tables have rows after indexing run
    5. Add logging to trace flow
  - Testing: run indexing on small test directory, query FTS5 directly to verify writes
  - Estimated effort: small (1-2 hours) once root cause identified


#### QueryService.swift
**Current:** XPC service wrapper around search
**Status:** ~65% complete, returns empty results

**Transfers:**
- Service protocol definition (search request → results response)
- Query execution (call LexicalIndex.search())
- Result ranking and sorting

**Redesign for Qt:**
- XPC → gRPC or Qt IPC

**Bugs to Fix:**
- ✓ **CRITICAL:** Searches return empty results
  - Root cause: depends on IndexerService bug (empty FTS5)
  - Fix: once IndexerService writes to FTS5, QueryService will automatically work
  - Secondary: add logging to verify FTS5 queries are correct


---

## CORE: RANKING & SCORING

#### ContextSignals.swift
**Current:** Frontmost app tracking, recent paths, 40+ bundle ID patterns
**Status:** ~80% complete

**Transfers Directly:**
- All signal gathering logic
- Bundle ID pattern matching (40+ known apps)
- Recent paths tracking (from filesystem events)
- App focus detection algorithm

**Redesign for Qt:**
- macOS-specific APIs (`NSWorkspace`, app bundles) → Qt + native C++
  - macOS: CoreFoundation APIs for frontmost app
  - Cross-platform: app name from Qt or fallback
- File access time tracking → filesystem metadata

**Dropped:**
- macOS NSWorkspace implementation (replace with native calls)

**Bugs to Fix:**
- None; ContextSignals logic is sound


#### Scoring.swift
**Current:** 16 weights, 7 match types, recency/frequency/context/junk scoring
**Status:** ~90% complete (well-tested, 8 tests)

**Transfers Directly:**
- All 16 weight constants
- Scoring formula (weighted sum of factors)
- Match type detection (exact, prefix, phrase, fuzzy, etc.)
- Recency decay function
- Frequency boost logic
- Junk score detection
- Test suite (8 tests)

**Redesign for Qt:**
- Pure C++ algorithm; no platform-specific code
- Copy tests to C++ Google Test framework

**Dropped:**
- Nothing; pure algorithm

**Bugs to Fix:**
- None; Scoring is mature


---

## SHARED: IPC & MODELS

#### ServiceClient.swift + ServiceProtocols.swift
**Current:** XPC connection management and protocol definitions
**Status:** ~75% complete

**Transfers:**
- Protocol interface design (request/response models)
- Connection lifecycle
- Error handling patterns

**Redesign for Qt:**
- XPC → gRPC or Qt local sockets
- Protocol definitions → `.proto` (gRPC) or custom serialization
- Connection pooling → client-side stub management

**Dropped:**
- macOS XPC framework

**Bugs to Fix:**
- ✓ **Missing:** XPC error handling lacks retry logic
  - Add exponential backoff for transient failures
  - Add circuit breaker for repeated failures
  - Add logging for debugging


#### Models (Feedback, IndexHealth, IndexItem, Settings, SearchResult)
**Current:** Data models with serialization
**Status:** ~85% complete

**Transfers Directly:**
- All field definitions
- Enum types (ItemKind, Classification, etc.)
- Serialization logic (adapt to C++ equivalents)
- Defaults and constants

**Redesign for Qt:**
- Swift `Codable` → Qt JSON serialization or C++ struct serialization
- Use modern C++ (classes/structs) with operator== for comparisons
- Add toString() methods for debugging

**Dropped:**
- Nothing; data models are universal

**Bugs to Fix:**
- None in model definitions


---

## HOTKEY MANAGEMENT

#### HotkeyManager.swift
**Current:** Carbon event handlers for Cmd+Space hotkey
**Status:** ~70% complete

**Transfers:**
- Hotkey registration logic (OS-level event hook)
- Hotkey conflict detection concept
- Activation/deactivation toggle

**Redesign for Qt:**
- Carbon (deprecated macOS API) → native macOS APIs or cross-platform library
  - Recommend: **QHotkey** (Qt-based, cross-platform)
    - Or: native macOS `EventTap` via CoreGraphics + CoreFoundation
    - Or: native macOS `HotKeyUIO` (non-public API, fragile)
  - For Linux/Windows future: QHotkey is most portable

- Event registration → library API or native OS calls

**Dropped:**
- Carbon event API (unsupported in recent macOS)

**Bugs to Fix:**
- ✓ **CRITICAL:** `isHotkeyAvailable()` always returns true
  - Should check OS-level hotkey registry (System Preferences, other apps)
  - Impact: if Cmd+Space is registered elsewhere (e.g., macOS Spotlight), causes conflict
  - Fix: query macOS event taps to detect existing hotkey registrations
  - Estimated effort: small (1-2 hours) with native macOS APIs


---

## TESTING

#### PathRulesTests.swift + ScoringTests.swift
**Current:** 13 + 8 unit tests
**Status:** ~100% (solid coverage)

**Transfers Directly:**
- All test cases and assertions
- Test data and edge cases

**Redesign for Qt:**
- Swift XCTest → C++ Google Test framework
- Adapt assertion syntax

**Dropped:**
- Nothing; tests are portable

#### IndexingIntegrationTests.swift
**Current:** FS scanning, SQLite CRUD, chunking
**Status:** ~85% complete

**Transfers:**
- Test scenarios (scan directory, store results, verify database)
- Test fixtures (temp directories, sample files)
- Assertion patterns

**Redesign for Qt:**
- XCTest → Google Test
- Temp directory handling → `QTemporaryDir`
- SQLite verification → Qt SQL queries

**Bugs to Fix:**
- Add integration tests for indexing pipeline end-to-end
- Verify FTS5 write path (catch regression of current bug)


---

## Infrastructure & Cross-Cutting

### Logging & Debugging
**Status:** ~0% in Swift scaffold
**Must Build in Qt:**
- Structured logging (timestamps, levels, categories)
- File rotation
- Performance instrumentation (trace bottlenecks)
- XPC/IPC request tracing
- Thread identification

**Recommendation:**
- Use `spdlog` C++ library (high-performance, easy integration)
- Logs to `~/.config/betterspotlight/logs/` or macOS standard location
- Development: console + file; production: file only

### Error Handling & Recovery
**Status:** ~60% (some gaps)
**Must Improve in Qt:**
- XPC/IPC retry logic (currently missing)
- Circuit breaker for failing services
- Graceful degradation (search works even if vector index fails)
- User-facing error messages (not just logs)

### Thread Safety
**Status:** ~70% (actors help in Swift, but gaps exist)
**Must Implement in Qt:**
- Explicit mutex/RWlock for shared state (AppState, SQLite connection pool)
- Thread-safe event queues for FSEvents
- Executor thread pool for extraction

### Persistence & State Recovery
**Status:** ~75%
**Must Implement in Qt:**
- Index recovery after crash (WAL mode handles this)
- Settings backup/restore
- Index versioning (handle schema changes)

---

## Porting Priority & Effort Matrix

**Dependencies Flow:**
```
Models ↓
├→ PathRules → FileScanner → FSEvents
├→ IndexItem → SQLiteStore → LexicalIndex → QueryService
├→ SearchResult ← Scoring ← ContextSignals
└→ Settings → SettingsView
```

**Recommended Porting Order:**

| Phase | Module | Priority | Effort | Notes |
|-------|--------|----------|--------|-------|
| **1: Foundation** | Models | P0 | Small | No dependencies |
| 1 | AppState + Settings | P0 | Small | Used everywhere |
| 1 | PathRules | P0 | Small | Tested, pure algorithm |
| **2: Storage** | SQLiteStore | P0 | Medium | Foundational, many dependents |
| 2 | LexicalIndex | P0 | Medium | Core search capability |
| **3: Filesystem** | FSEvents wrapper | P1 | Medium | Platform-specific, needed for indexing |
| 3 | FileScanner | P1 | Small | Depends on FSEvents |
| **4: Extraction** | TextExtractor | P1 | Small | Simplest extractor |
| 4 | PdfExtractor | P1 | Medium | Common file type |
| 4 | OcrExtractor | P2 | Large | Complex, external dependency (Tesseract) |
| 4 | ExtractionManager | P1 | Medium | Orchestrates extractors |
| **5: Indexing Pipeline** | IndexerService | P0 | Medium | **CRITICAL BUG FIX** |
| 5 | Hotkey + Search Panel | P1 | Medium | User-facing, depends on QueryService |
| **6: Ranking** | ContextSignals | P2 | Medium | Nice-to-have initially |
| 6 | Scoring | P2 | Small | Pure algorithm, tested |
| **7: UI** | Main window + Search UI | P1 | Large | Depends on QueryService working |
| 7 | Settings UI | P2 | Medium | Not blocking search |
| 7 | Onboarding | P3 | Small | Last (users can skip) |
| **8: Advanced** | VectorIndex | P3 | Large | Phase 2, after lexical search solid |
| 8 | ExtractorService IPC | P2 | Medium | Can be in-process initially |
| **9: Polish** | Logging infrastructure | P1 | Medium | Essential for debugging crashes |
| 9 | Error recovery | P2 | Medium | Improves reliability |
| 9 | Tests | P2 | Medium | Ongoing, critical for quality |

**Total Effort Estimate:**
- Foundation + Storage + Core Extraction: **4-5 weeks** (2 people)
- Full indexing pipeline + UI: **2-3 weeks** (2 people)
- VectorIndex + Polish: **2-3 weeks** (1 person parallel)
- **Total: 6-8 weeks** for working MVP with no vector search
- **8-10 weeks** with full vector search enabled

---

## Critical Path for MVP (Working Search)

1. **Models & AppState** (3-4 days)
   - Migrate data structures
   - Implement Qt serialization

2. **SQLiteStore + LexicalIndex** (1 week)
   - Verify schema works with Qt SQL
   - Test FTS5 queries

3. **PathRules + FileScanner + FSEvents** (1 week)
   - Filesystem watching working
   - Can enumerate files with exclusions

4. **TextExtractor + ExtractionManager** (4-5 days)
   - Extract text from common files
   - Chunking working

5. **IndexerService (FIX CRITICAL BUG)** (2-3 days)
   - Wire extracted items → LexicalIndex.insert()
   - Verify FTS5 tables populate

6. **QueryService + Search Panel** (1 week)
   - Search UI callable
   - Returns results from FTS5
   - Results ranked by score

7. **Hotkey + Status Bar** (3-4 days)
   - Cmd+Space triggers search
   - Status bar shows health

**Critical Path Duration: 5-6 weeks** → searchable index working

---

## Known Bugs Summary & Fix Priority

| Bug | Severity | Impact | Fix Effort | Fix Timing |
|-----|----------|--------|-----------|-----------|
| IndexerService doesn't write to FTS5 | 🔴 CRITICAL | Search returns nothing | 2-3 hrs | Phase 5 (during indexing service port) |
| QueryService searches empty index | 🔴 CRITICAL | Blocks all search | Depends on above | Phase 5 (auto-fixed when above fixed) |
| VectorIndex is placeholder | 🔴 CRITICAL | No semantic search | 2-3 weeks | Phase 8 (after lexical search solid) |
| HotkeyManager.isHotkeyAvailable() always true | 🟠 HIGH | Can't detect conflicts | 1-2 hrs | Phase 7 (during hotkey impl) |
| Index rebuild button stubbed | 🟠 HIGH | Can't manually refresh | 4-6 hrs | Phase 6 or later |
| Cache clear button stubbed | 🟡 MEDIUM | Can't clear vector cache | 2-3 hrs | Phase 8 (with VectorIndex) |
| Health snapshot shows zeros | 🟡 MEDIUM | UI misinformation | 2-3 hrs | Phase 2 (during AppState impl) |
| Highlights empty in search results | 🟡 MEDIUM | UX: can't see why match | 4-6 hrs | Phase 6 (during search UI) |
| XPC error handling lacks retry | 🟡 MEDIUM | Transient failures crash | 4-6 hrs | Phase 9 (polish) |
| No logging infrastructure | 🟡 MEDIUM | Hard to debug issues | 2-3 days | Phase 1 (early, critical for debugging) |

---

## Architectural Decisions to Carry Forward

✓ **Keep these from Swift scaffold:**
1. Layered architecture (UI → Services → Core → Storage)
2. Extraction pipeline with concurrency control
3. XPC/IPC service separation (for isolation/reliability)
4. FTS5 schema for full-text search
5. BM25 ranking with context signals
6. Chunked text extraction (prevents large text blobs)
7. Async/concurrent operations throughout
8. Modular extraction (different extractors for different types)
9. Exclusion-based filesystem filtering
10. SQLite WAL mode for concurrent reads

✗ **Don't try to port these directly:**
1. SwiftUI (use Qt Widgets/QML)
2. Swift actors (use explicit threading)
3. Cocoa frameworks (use Qt equivalents)
4. macOS-specific APIs without replacement
5. Carbon hotkey API (use modern alternative)

---

## Q&A for Rewrite Team

**Q: Should we keep in-process services or separate processes?**
A: **Start in-process** (simpler), split to separate processes later if needed for isolation. IPC adds complexity; ensure search works first.

**Q: How do we handle cross-platform in future?**
A: **Design for it now:** Qt abstracts windows/dialogs, gRPC abstracts RPC, modern C++ STL is portable. Avoid macOS-isms (hardcoded paths, APIs).

**Q: When should we implement vector search?**
A: **Phase 2, after lexical search solid.** It's valuable but not blocking; wait for user feedback on FTS5 performance.

**Q: What's the test strategy?**
A: **Unit tests for algorithms** (PathRules, Scoring, extraction logic), **integration tests for pipeline** (filesystem → extraction → indexing → search), **manual tests for UI** (hotkey, search panel).

**Q: How do we debug the XPC/IPC layer?**
A: **Comprehensive logging** (see Infrastructure section). Log all requests/responses with timestamps, payloads, errors.

---

## File Manifest for Reference

**Swift Scaffold Structure:**
```
App/
  BetterSpotlightApp.swift
  OnboardingView.swift
  Hotkey/HotkeyManager.swift
  SearchPanel/SearchPanelController.swift
  SearchPanel/SearchPanelView.swift
  Settings/IndexHealthView.swift
  Settings/SettingsView.swift
  UI/AppState.swift
  UI/StatusBarController.swift

Core/
  Extraction/ExtractionManager.swift
  Extraction/OcrVision/OcrExtractor.swift
  Extraction/PdfExtractor/PdfExtractor.swift
  Extraction/TextExtractors/TextExtractor.swift
  FS/FSEvents.swift
  FS/FileScanner.swift
  FS/PathRules.swift
  Index/LexicalIndex/LexicalIndex.swift
  Index/SQLiteStore.swift
  Index/VectorIndex/VectorIndex.swift
  Ranking/ContextSignals.swift
  Ranking/Scoring.swift

Services/
  ExtractorXPC/ExtractorService.swift
  IndexerXPC/IndexerService.swift
  QueryXPC/QueryService.swift

Shared/
  IPC/ServiceClient.swift
  IPC/ServiceProtocols.swift
  Models/Feedback.swift
  Models/IndexHealth.swift
  Models/IndexItem.swift
  Models/Settings.swift
  Models/SearchResult.swift

Tests/
  Unit/PathRulesTests.swift
  Unit/ScoringTests.swift
  Integration/IndexingIntegrationTests.swift
```

**Key Qt/C++ Libraries to Use:**
- Qt 6 (Widgets/QML, SQL, Concurrent)
- SQLite (system)
- `nlohmann/json` or Qt JSON
- `spdlog` (logging)
- `QHotkey` (hotkey management)
- Tesseract (OCR) or Vision framework (macOS)
- `pdfium` (PDF extraction)
- `hnswlib` (vector KNN) [Phase 2]
- ONNX Runtime (embeddings inference) [Phase 2]

---

## Conclusion

The Swift scaffold is **architecturally sound** and demonstrates correct design thinking. The Qt 6/C++ rewrite should:

1. **Adopt the architecture** (layering, extraction pipeline, XPC concept)
2. **Fix three critical bugs** (FTS5 writes, hotkey checking, vector index implementation)
3. **Replace platform-specific code** with Qt/C++ equivalents
4. **Keep all algorithms and business logic** (score calculation, pattern matching, extraction strategies)
5. **Improve infrastructure** (logging, error recovery, testing)

With disciplined porting and parallel work, a 2-3 person team can deliver a working MVP in **6-8 weeks**, with full feature parity (including vector search) in **8-10 weeks**.

**Next Step:** Review this document with the team, prioritize any differences, and begin Phase 1 (Models/AppState) immediately.
