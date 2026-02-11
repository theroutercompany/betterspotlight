# BetterSpotlight Milestone 3 - Complete Architectural Map

> 2026-02-11 addendum: Semantic architecture evolved from fixed `384d int8` to a
> generation-aware `1024d float32` target with CoreML-first model sessions and CPU fallback.
> Retrieval now supports passage-level semantic evidence aggregation and dual-index migration state.

**Prepared for:** Principal Engineering Team  
**Project:** BetterSpotlight C++/Qt6 Rewrite  
**Scope:** Comprehensive project structure, module boundaries, service isolation, data flows  
**Generated:** February 9, 2026  

---

## EXECUTIVE SUMMARY

BetterSpotlight is a macOS file search application with:
- **4-layer architecture**: App (Qt/QML) → Core → Services (isolated) → Shared Models
- **3 service processes**: Indexer, Extractor, Query (Unix socket IPC)
- **M1+M2 codebase**: Base search + semantic search via ONNX embeddings
- **20 core modules** across 4 major layers
- **40+ unit tests, 15+ integration tests** with relevance gate validation

This document maps the complete directory structure, module dependency graph, IPC boundaries, test infrastructure, and configuration system for Milestone 3 planning.

---

## 1. DIRECTORY STRUCTURE (SOURCE)

### 1.1 Top-Level Layout

```
/Users/rexliu/betterspotlight/
├── CMakeLists.txt                    # Root CMake configuration
├── Src/                              # Main source tree
│   ├── app/                          # UI layer (Qt Quick/QML)
│   ├── core/                         # Core layer (20 modules)
│   ├── services/                     # Service processes (3 binaries)
│   └── vendor/                       # Third-party code
├── Tests/                            # Test suite
│   ├── Unit/                         # 16 unit tests
│   ├── Integration/                  # 15 integration tests
│   ├── Fixtures/                     # Test data
│   ├── relevance/                    # Relevance baselines & suite
│   └── CMakeLists.txt                # Test build config
├── docs/                             # Complete specification
├── data/                             # Assets (models, config)
├── build/                            # CMake build directory
└── tools/                            # Utility scripts
```

### 1.2 Core Layer (Src/core/)

**20 modules organized by subsystem:**

```
Src/core/
├── shared/                           # Shared types, models, IPC definitions
│   ├── types.h                       # ItemKind, Sensitivity, ValidationResult
│   ├── search_result.h               # SearchResult, MatchType, ScoreBreakdown
│   ├── chunk.h                       # Content chunks for indexing
│   ├── index_health.h                # Index statistics
│   ├── settings.h                    # Configuration types
│   ├── scoring_types.h               # ScoringWeights
│   ├── ipc_messages.h                # IpcErrorCode, IpcRequest/Response
│   ├── search_options.h              # Query parameters
│   ├── fda_check.h                   # Full Disk Access validation
│   ├── logging.h                     # Logging infrastructure
│   ├── settings_manager.h            # Config persistence
│   └── CMakeLists.txt                # → betterspotlight-core-shared static lib
│
├── fs/                               # File system monitoring & validation
│   ├── path_rules.h                  # PathRules: exclusion + classification
│   ├── path_rules.cpp                # 30+ built-in patterns
│   ├── file_monitor_macos.h          # FSEvents wrapper
│   ├── file_monitor_macos.cpp        # CoreServices integration
│   ├── file_scanner.h                # Async directory walking
│   ├── file_scanner.cpp              # Concurrent scanning with limits
│   ├── bsignore_parser.h             # .bsignore format
│   ├── bsignore_parser.cpp           # Gitignore-style parsing
│   └── CMakeLists.txt                # → betterspotlight-core-fs static lib
│
├── extraction/                       # Content extraction (text, PDF, OCR)
│   ├── extraction_manager.h          # Orchestrates all extractors
│   ├── extraction_manager.cpp        # Concurrency control (4 workers, 30s timeout)
│   ├── text_extractor.h              # 100+ file types
│   ├── text_extractor.cpp            # Syntax-aware extraction
│   ├── mdls_text_extractor.h         # macOS metadata via xattr
│   ├── mdls_text_extractor.cpp       # Spotlight metadata fallback
│   ├── pdf_extractor.h               # Poppler/PDFium wrapper
│   ├── pdf_extractor.cpp             # 1000-page limit, auto-chunking
│   ├── ocr_extractor.h               # Tesseract integration
│   ├── ocr_extractor.cpp             # Text position ordering
│   ├── text_cleaner.h                # Post-processing
│   ├── text_cleaner.cpp              # Normalization, whitespace
│   └── CMakeLists.txt                # → betterspotlight-core-extraction static lib
│
├── index/                            # SQLite indexing & schema
│   ├── sqlite_store.h                # Actor-like SQLiteStore
│   ├── sqlite_store.cpp              # CRUD for 7 tables
│   ├── schema.h                      # Table definitions + FTS5 config
│   ├── migration.h                   # Database versioning
│   ├── migration.cpp                 # v1 → vN migrations
│   ├── typo_lexicon.h                # Edit-distance corrections
│   └── typo_lexicon.cpp              # Phonetic matching
│
├── indexing/                         # Pipeline orchestration
│   ├── pipeline.h                    # Main Pipeline (coordinator)
│   ├── pipeline.cpp                  # 3-stage: ingress, prep, db-write
│   ├── work_queue.h                  # Persistent work queue
│   ├── work_queue.cpp                # Priority: deletes > recent > new
│   ├── indexer.h                     # Content indexing logic
│   ├── indexer.cpp                   # DB writer (single-threaded)
│   ├── chunker.h                     # Text chunking
│   ├── chunker.cpp                   # word/sentence/paragraph boundaries
│   └── CMakeLists.txt                # → betterspotlight-core-indexing static lib
│
├── ranking/                          # Scoring & relevance
│   ├── scorer.h                      # Scorer with 16 configurable weights
│   ├── scorer.cpp                    # Multi-signal ranking
│   ├── match_classifier.h            # MatchType detection
│   ├── match_classifier.cpp          # 7 match types (exact/prefix/fuzzy)
│   ├── context_signals.h             # QueryContext: CWD, app, clipboard
│   ├── context_signals.cpp           # 40+ known app types
│   └── CMakeLists.txt                # → betterspotlight-core-ranking static lib
│
├── query/                            # Query processing
│   ├── query_normalizer.h            # Lowercase, stopword filtering
│   ├── query_normalizer.cpp          # Unicode normalization
│   ├── query_parser.h                # Tokenization, phrase parsing
│   └── query_parser.cpp              # Quoted phrase support
│
├── ipc/                              # Interprocess communication
│   ├── message.h                     # IpcMessage: encode/decode
│   ├── message.cpp                   # Length-prefixed JSON protocol
│   ├── socket_server.h               # Server-side socket listener
│   ├── socket_server.cpp             # Qt signal-based I/O
│   ├── socket_client.h               # Client-side connector
│   ├── socket_client.cpp             # Async request/response
│   ├── service_base.h                # Service base class (Q_OBJECT)
│   ├── service_base.cpp              # IPC dispatcher
│   ├── supervisor.h                  # Process management
│   ├── supervisor.cpp                # Start/restart/monitor services
│   └── CMakeLists.txt                # → betterspotlight-core-ipc static lib
│
├── embedding/ (M2+)                  # ONNX semantic embeddings
│   ├── embedding_manager.h           # Orchestrates embedding pipeline
│   ├── embedding_manager.cpp         # Model loading, inference
│   ├── embedding_pipeline.h          # Batching + generation-aware ingest
│   ├── embedding_pipeline.cpp        # Async processing
│   ├── tokenizer.h                   # WordPiece tokenization
│   ├── tokenizer.cpp                 # Manifest-driven vocab loading
│   ├── quantizer.h                   # Legacy compatibility utilities
│   ├── quantizer.cpp                 # Legacy compatibility utilities
│   └── CMakeLists.txt                # → betterspotlight-core-embedding static lib
│
├── vector/ (M2+)                     # Vector search (hnswlib)
│   ├── vector_index.h                # HNSW index interface
│   ├── vector_index.cpp              # Approximate nearest neighbor
│   ├── vector_store.h                # Persisted vector storage
│   ├── vector_store.cpp              # SQLite + .vec file
│   ├── search_merger.h               # Merge lexical + semantic results
│   ├── search_merger.cpp             # Rank fusion algorithm
│   └── CMakeLists.txt                # → betterspotlight-core-vector static lib
│
├── feedback/ (M2+)                   # User feedback aggregation
│   ├── interaction_tracker.h         # Record queries, selections
│   ├── interaction_tracker.cpp       # SQLite-backed tracking
│   ├── feedback_aggregator.h         # Aggregate signals
│   ├── feedback_aggregator.cpp       # Batch compute path prefs, affinities
│   ├── path_preferences.h            # Path-based boost/penalty
│   ├── path_preferences.cpp          # LRU history per path prefix
│   ├── type_affinity.h               # File type patterns
│   ├── type_affinity.cpp             # .cpp ↔ .h affinity
│   └── CMakeLists.txt                # → betterspotlight-core-feedback static lib
│
├── CMakeLists.txt                    # Aggregates all core libs → betterspotlight-core
│
└── All compiled to: static lib betterspotlight-core
    (links 10 sub-libraries + Qt6::Core + sqlite3)
```

### 1.3 Services Layer (Src/services/)

**3 isolated service executables:**

```
Src/services/
├── indexer/                          # IndexerService executable
│   ├── indexer_service.h             # IPC handler, FSEvents coordinator
│   ├── indexer_service.cpp           # startIndexing, pause, resume, etc.
│   ├── main.cpp                      # Entry point, socket listener
│   └── CMakeLists.txt                # → betterspotlight-indexer executable
│
├── extractor/                        # ExtractorService executable
│   ├── extractor_service.h           # IPC handler, extraction delegator
│   ├── extractor_service.cpp         # extractText, extractMetadata, etc.
│   ├── main.cpp                      # Entry point, socket listener
│   └── CMakeLists.txt                # → betterspotlight-extractor executable
│
└── query/                            # QueryService executable
    ├── query_service.h               # IPC handler, FTS5 search
    ├── query_service.cpp             # search, getHealth, recordFeedback
    ├── query_service_m2.cpp          # M2 handlers (embeddings, vector rebuild)
    ├── main.cpp                      # Entry point, socket listener
    └── CMakeLists.txt                # → betterspotlight-query executable
```

### 1.4 App Layer (Src/app/)

**Qt Quick/QML UI:**

```
Src/app/
├── main.cpp                          # Qt app entry point
├── service_manager.h/cpp             # Launch & supervise 3 services
├── hotkey_manager.h/cpp              # Global hotkey (Cmd+Space)
├── search_controller.h/cpp           # Query → QueryService IPC
├── settings_controller.h/cpp         # Settings persistence
├── onboarding_controller.h/cpp       # First-run flow
├── status_bar_bridge.h               # StatusBar/menu integration
├── qml/
│   ├── Main.qml                      # Root window
│   ├── SearchPanel.qml               # Search input + live results
│   ├── ResultItem.qml                # Result display with snippet
│   ├── SettingsPanel.qml             # 5-tab settings UI
│   ├── StatusBar.qml                 # Menu bar item
│   ├── onboarding/
│   │   ├── OnboardingWindow.qml
│   │   ├── WelcomeStep.qml           # FDA explanation
│   │   ├── FdaStep.qml               # Request Full Disk Access
│   │   └── HomeMapStep.qml           # Directory classification
│   ├── components/
│   │   ├── HotkeyRecorder.qml
│   │   ├── DirectoryPicker.qml
│   │   └── PatternEditor.qml
│   └── settings/
│       ├── GeneralTab.qml
│       ├── IndexingTab.qml
│       ├── ExclusionsTab.qml
│       ├── PrivacyTab.qml
│       └── IndexHealthTab.qml
├── Info.plist                        # Bundle config
└── CMakeLists.txt                    # → betterspotlight (app bundle)
```

### 1.5 Vendor & Dependencies

```
Src/vendor/
├── sqlite/
│   └── sqlite3.c/h                   # Vendored SQLite with FTS5 + JSON1
├── hnswlib/                          # Approximate nearest neighbor (M2+)
└── (Qt6, ONNX Runtime, Poppler, Tesseract: system/vcpkg)
```

---

## 2. MODULE DEPENDENCY GRAPH

### 2.1 Layered Architecture (Dependency Direction)

```
┌─────────────────────────────────────┐
│       APP LAYER (UI)                │
│  • betterspotlight (Qt app)         │
│  • QML components, hotkey, services │
└────────────┬────────────────────────┘
             │
             ↓
┌─────────────────────────────────────┐
│      CORE LAYER (Logic)             │
│  • betterspotlight-core (aggregate) │
│  • 10 sub-libraries                 │
└─────┬─────────────────┬─────────────┘
      │                 │
      ↓                 ↓
   ┌──────────────┐  ┌──────────────┐
   │  SERVICES    │  │   SHARED     │
   │ (3 binaries) │  │   (Models)   │
   └──────────────┘  └──────────────┘
      │                 │
      ├─ indexer       └─ ipc_messages
      ├─ extractor        types
      └─ query            search_result
```

### 2.2 Core Layer Internal Dependencies

**Strict unidirectional flow:**

```
betterspotlight-core (aggregate static library)
    ↓
Depends on 10 sub-libraries:

 1. betterspotlight-core-shared
    (NO dependencies, root library)

 2. betterspotlight-core-fs
    depends: shared, Qt6::Core, CoreServices framework

 3. betterspotlight-core-extraction
    depends: shared, Qt6::Core, Poppler/PDFium, Tesseract

 4. betterspotlight-core-ipc
    depends: shared, Qt6::Core, Qt6::Network

 5. betterspotlight-core-indexing
    depends: shared, fs, extraction, sqlite3, Qt6::Core

 6. betterspotlight-core-ranking
    depends: shared, Qt6::Core

 7. betterspotlight-core-feedback (M2+)
    depends: shared, sqlite3, Qt6::Core

 8. betterspotlight-core-embedding (M2+, optional BETTERSPOTLIGHT_WITH_ONNX)
    depends: shared, Qt6::Core, ONNXRuntime::ONNXRuntime (if found)

 9. betterspotlight-core-vector (M2+, optional BETTERSPOTLIGHT_WITH_ONNX)
    depends: shared, ranking, sqlite3, Qt6::Core, hnswlib

10. betterspotlight-core-indexing
    (index/ submodule: sqlite_store, migration, typo_lexicon)
    - Compiled into betterspotlight-core directly
    - depends: sqlite3, Qt6::Core
```

**Cross-library rule:** Lower numbers can only depend on:
- themselves
- shared (library 1)
- external (Qt, sqlite3, frameworks)

**Does NOT allow:**
- ranking → indexing, fs, extraction (no upward deps)
- fs → extraction, indexing, ranking (no upward deps)

### 2.3 Service Processes → Core Dependencies

```
betterspotlight-indexer executable
    ↓
links betterspotlight-core
    ↓
accesses: fs (FSEvents), indexing (Pipeline), extraction (ExtractionManager),
          ipc (ServiceBase), shared (types)

betterspotlight-query executable
    ↓
links betterspotlight-core
    ↓
accesses: ranking (Scorer), feedback (InteractionTracker, FeedbackAggregator),
          embedding (EmbeddingManager), vector (VectorIndex, SearchMerger),
          ipc (ServiceBase), shared (types)

betterspotlight-extractor executable
    ↓
links betterspotlight-core
    ↓
accesses: extraction (ExtractionManager), ipc (ServiceBase), shared (types)
```

### 2.4 App → Services Communication

```
betterspotlight (Qt app)
    ↓
links betterspotlight-core
    ↓
ServiceManager spawns 3 processes:
    • betterspotlight-indexer
    • betterspotlight-extractor
    • betterspotlight-query

Communication: Unix domain sockets
    /tmp/betterspotlight-{uid}/indexer.sock
    /tmp/betterspotlight-{uid}/extractor.sock
    /tmp/betterspotlight-{uid}/query.sock

Protocol: Length-prefixed JSON over TCP-like socket API
```

---

## 3. IPC / SERVICE BOUNDARY ARCHITECTURE

### 3.1 Three-Service Model

```
┌──────────────────────────────────────────────────────────────────┐
│                    UI Process (betterspotlight)                  │
│                                                                  │
│  • ServiceManager (spawns 3 child processes)                    │
│  • SearchController (IPC client → QueryService)                 │
│  • SettingsController (reads/writes ~/.betterspotlight/)        │
│  • HotkeyManager (listens Cmd+Space)                            │
│                                                                  │
└──────────────────────────────────────────────────────────────────┘
                    │              │              │
                    │ Unix Domain  │              │ Unix Domain
                    │ Socket       │              │ Socket
                    ↓              ↓              ↓
         ┌──────────────────┐ ┌──────────────────┐ ┌──────────────────┐
         │ IndexerService   │ │ ExtractorService │ │ QueryService     │
         │                  │ │                  │ │                  │
         │ • FSEvents watch │ │ • TextExtractor  │ │ • FTS5 search    │
         │ • WorkQueue      │ │ • PdfExtractor   │ │ • Ranking        │
         │ • Pipeline       │ │ • OcrExtractor   │ │ • M2: embedding  │
         │ • SQLiteStore    │ │                  │ │ • M2: vector idx │
         │                  │ │                  │ │ • Feedback log   │
         └──────────────────┘ └──────────────────┘ └──────────────────┘
                    │                                       │
                    │                                       │
                    └───────────────┬───────────────────────┘
                                    │
                                    ↓
                              SQLiteStore
                          (shared database)
                       ~/.betterspotlight/index.db
```

### 3.2 IPC Message Format

**Protocol:** Length-prefixed JSON (4-byte BE uint32 + UTF-8 JSON)

**Request:**
```json
{
  "id": 12345,
  "method": "search",
  "params": {
    "query": "hello",
    "limit": 10,
    "contextPath": "/Users/alice/Documents"
  }
}
```

**Response (success):**
```json
{
  "id": 12345,
  "result": {
    "results": [
      {
        "itemId": 1234,
        "path": "/Users/alice/Documents/hello.txt",
        "name": "hello.txt",
        "matchType": "ExactName",
        "score": 200.5,
        "snippet": "Hello World Example"
      }
    ]
  }
}
```

**Response (error):**
```json
{
  "id": 12345,
  "error": {
    "code": 3,
    "message": "PermissionDenied: /Library/System"
  }
}
```

### 3.3 Error Codes (IpcErrorCode enum)

```cpp
enum class IpcErrorCode : int {
    InvalidParams      = 1,  // Bad request params
    Timeout            = 2,  // Operation timed out
    PermissionDenied   = 3,  // FDA or file permission
    NotFound           = 4,  // Resource not found
    AlreadyRunning     = 5,  // Service already indexing
    InternalError      = 6,  // Unexpected error
    Unsupported        = 7,  // Operation not supported
    CorruptedIndex     = 8,  // SQLite integrity error
    ServiceUnavailable = 9,  // Service crashed/offline
};
```

### 3.4 Service Method Signatures (IPC Protocols)

**IndexerService methods:**
```
startIndexing(roots: [String]) → {success, queuedPaths, timestamp}
pauseIndexing() → {success}
resumeIndexing() → {success}
setUserActive(active: bool) → {success}
reindexPath(path: String) → {success, queued}
rebuildAll() → {success, totalPaths}
getQueueStatus() → {queueSize, processedCount, totalCount, progress}
```

**ExtractorService methods:**
```
extractText(filePath: String) → {text, language, duration}
extractMetadata(filePath: String) → {kind, size, created, modified}
isSupported(filePath: String) → {supported, mimeType}
cancelExtraction(fileId: int64) → {cancelled}
clearExtractionCache() → {cleared, spaceFreed}
```

**QueryService methods:**
```
search(query: String, limit: int, context: QueryContext) 
  → {results: [SearchResult], duration, totalMatches}

getHealth() → {fileCount, totalSize, lastScan, errorCount, vectorCoverage}

recordFeedback(query: String, selectedIndex: int, rank: int) → {logged}

recordInteraction(filePath: String, action: String, timestamp: int64) → {id}

getPathPreferences(pathPrefix: String) → {boost, penalty, frequency}

getFileTypeAffinity() → {affinities: {ext → weight}}

runAggregation() → {updated: bool, newSignals: int}

rebuildVectorIndex(includePaths: [String]) 
  → {runId: uint64, status: String}
```

---

## 4. BUILD SYSTEM (CMake)

### 4.1 Root CMakeLists.txt Configuration

```cmake
cmake_minimum_required(VERSION 3.25)
project(BetterSpotlight VERSION 0.1.0 LANGUAGES C CXX)

# C++20 standard required
set(CMAKE_CXX_STANDARD 20)

# macOS deployment target 14+ (Sonoma)
set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0")

# Compiler flags
add_compile_options(-Wall -Wextra -Wpedantic -Werror)

# Debug: ASAN, Release: LTO
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-O0 -g -fsanitize=address,undefined)
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O2 -flto)
endif()

# Qt 6 required components
find_package(Qt6 REQUIRED COMPONENTS
    Core Widgets Network Qml Quick QuickControls2 Test)

# Vendored SQLite (static)
add_library(sqlite3 STATIC src/vendor/sqlite/sqlite3.c)
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_ENABLE_FTS5=1
    SQLITE_ENABLE_JSON1=1
    SQLITE_ENABLE_COLUMN_METADATA=1
    SQLITE_THREADSAFE=2)

# Optional M2 features
option(BETTERSPOTLIGHT_WITH_ONNX "Enable ONNX Runtime" ON)
option(BETTERSPOTLIGHT_FETCH_MODELS "Download embedding models" ON)

# External dependencies (optional)
find_package(ONNXRuntime QUIET)
find_package(Tesseract QUIET)
find_package(PkgConfig QUIET)
pkg_check_modules(POPPLER_QT6 QUIET poppler-qt6)

# Build subdirectories
add_subdirectory(src/core)
add_subdirectory(src/services/indexer)
add_subdirectory(src/services/extractor)
add_subdirectory(src/services/query)
add_subdirectory(src/app)
enable_testing()
add_subdirectory(tests)
```

### 4.2 Build Targets Summary

```
betterspotlight-core-shared       [STATIC]  — Shared types (NO deps)
betterspotlight-core-fs           [STATIC]  — FS monitoring
betterspotlight-core-extraction   [STATIC]  — Text/PDF/OCR extraction
betterspotlight-core-ipc          [STATIC]  — Socket IPC layer
betterspotlight-core-indexing     [STATIC]  — Pipeline + indexing
betterspotlight-core-ranking      [STATIC]  — Scoring + matching
betterspotlight-core-feedback     [STATIC]  — User feedback (M2+)
betterspotlight-core-embedding    [STATIC]  — ONNX embeddings (M2+, optional)
betterspotlight-core-vector       [STATIC]  — Vector search (M2+, optional)

betterspotlight-core              [STATIC]  — Aggregate (10 sub-libs above)

betterspotlight-indexer           [EXECUTABLE]  — Indexer service
betterspotlight-extractor         [EXECUTABLE]  — Extractor service
betterspotlight-query             [EXECUTABLE]  — Query service

betterspotlight                   [EXECUTABLE]  — Main Qt app (bundle on macOS)
```

### 4.3 Build Commands

```bash
# Configure (Debug build with ASAN)
cmake -B build -DCMAKE_BUILD_TYPE=Debug

# Configure (Release build with LTO)
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBETTERSPOTLIGHT_WITH_ONNX=ON

# Build all targets
cmake --build build --parallel

# Run all tests
ctest --test-dir build --output-on-failure

# Run specific test
ctest --test-dir build --verbose -R test-sqlite-store
```

---

## 5. TEST INFRASTRUCTURE

### 5.1 Test Organization (40+ tests)

**Unit Tests (16):**
```
Tests/Unit/
├── test_sqlite_store.cpp              — SQLiteStore CRUD, transactions
├── test_path_rules.cpp                — PathRules, exclusion patterns
├── test_scoring.cpp                   — Scorer, multi-signal ranking
├── test_chunker.cpp                   — Text chunking algorithms
├── test_bsignore_parser.cpp           — .bsignore parsing
├── test_ipc_messages.cpp              — IPC encode/decode
├── test_match_classifier.cpp          — MatchType detection (7 types)
├── test_corrupt_files.cpp             — Corruption recovery
├── test_query_ranking.cpp             — Query → ranking integration
├── test_extraction_office_pdf.cpp     — PDF extraction
├── test_extraction_extension_fallback.cpp  — Format detection fallback
├── test_docs_memo_location.cpp        — Documentation/memo file handling
├── test_tokenizer.cpp                 — WordPiece tokenization (M2)
├── test_embedding.cpp                 — ONNX inference (M2)
├── test_quantizer.cpp                 — legacy quantizer compatibility (M2)
└── test_vector_index.cpp              — HNSW index (M2)
```

**M2 Unit Tests (5):**
```
├── test_search_merger.cpp             — Lexical + semantic merge
├── test_interaction_tracker.cpp       — Interaction recording
├── test_feedback_aggregator.cpp       — Feedback aggregation
├── test_path_preferences.cpp          — Path-based preferences
└── test_type_affinity.cpp             — File type affinity
```

**Integration Tests (15):**
```
Tests/Integration/
├── test_full_pipeline.cpp             — End-to-end FS→search
├── test_index_persistence.cpp         — SQLite durability
├── test_incremental_update.cpp        — Incremental indexing
├── test_crash_isolation.cpp           — Service crash recovery
├── test_index_backpressure.cpp        — Concurrency limits
├── test_query_service_core_improvements.cpp  — Query service M1 fixes
├── test_query_service_relevance_fixture.cpp  — Relevance baseline testing
├── test_semantic_search.cpp           — Embedding + KNN (M2)
├── test_embedding_fallback.cpp        — Embedding degradation (M2)
├── test_boost_verification.cpp        — Context signal validation (M2)
├── test_context_boost.cpp             — CWD + app context (M2)
├── test_embedding_recovery.cpp        — Recovery from vector failures (M2)
└── test_ui_sim_query_suite.cpp        — UI simulation + relevance gate
```

**Test Count:**
- Unit: 21 test cases
- Integration: 15 test suites
- Total: ~100+ individual test assertions

### 5.2 Key Test Files (Remediation Focus)

**test_query_service_relevance_fixture.cpp (18KB)**
```cpp
// Multi-category fixture-based relevance testing
// Uses BS_RELEVANCE_BASELINES_PATH: baselines.json with baseline scores
// Tests:
//  - Query case categories (exact name, fuzzy, content)
//  - Baseline score comparison (within tolerance)
//  - Result ranking stability
//  - Match type accuracy

struct QueryCase {
    QString id;
    QString category;
    QString query;
    QString mode;
    QString expectedFileName;
    int topN = 3;
};

// Launches betterspotlight-query subprocess
// Populates fixture with standard_home_v1 test corpus
// Evaluates top-3 result ranking per query
// Reports baseline adherence
```

**test_ui_sim_query_suite.cpp (28KB)**
```cpp
// User-centric relevance gate testing
// Uses BS_RELEVANCE_SUITE_PATH: ui_sim_query_suite.json with ~50 queries
// Tests:
//  - UI simulation of user behavior (type → search → select)
//  - Live index population via full_pipeline
//  - In-memory ranking (no subprocess)
//  - Score breakdown transparency

// Environment variables:
//  BS_RELEVANCE_GATE_MODE:   report_only | enforce
//  BS_RELEVANCE_REPORT_PATH: /tmp/bs_ui_sim_query_suite_report.json
//
// Test modes:
//  1. report_only  → JSON report, no failure
//  2. enforce      → fail if threshold breached
//
// Emits JSON report:
// {
//   "timestamp": "2026-02-09T...",
//   "queries": [
//     {
//       "id": "exact_name_001",
//       "passed": true,
//       "expectedFile": "myapp.cpp",
//       "topNFile": "myapp.cpp",
//       "scorePercentile": 95.2
//     }
//   ],
//   "summary": {
//     "totalQueries": 50,
//     "passed": 48,
//     "failed": 2,
//     "passRate": 96.0
//   }
// }
```

### 5.3 Test Fixtures

**Standard Fixture (standard_home_v1/):**
```
Tests/Fixtures/standard_home_v1/
├── Documents/
│   ├── memo.txt
│   ├── report.docx
│   └── project_notes.md
├── Developer/
│   └── myapp/
│       ├── src/
│       │   ├── main.cpp
│       │   ├── config_parser.cpp
│       │   └── utils.h
│       └── build/
│           └── (ignored by default)
├── Downloads/
│   └── (low relevance)
└── .bsignore (default patterns + custom)
```

**Relevance Baselines:**
```
Tests/relevance/
├── baselines.json           — Fixture-based baseline scores
├── ui_sim_query_suite.json  — 50-query corpus with expected results
└── relevance_report.json    — Latest test report
```

### 5.4 CMakeLists.txt Test Configuration

```cmake
find_package(Qt6 REQUIRED COMPONENTS Test)

macro(bs_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE
        betterspotlight-core
        sqlite3
        Qt6::Core
        Qt6::Test
        Qt6::Network)
    add_test(NAME ${name} COMMAND ${name})
endmacro()

# Unit tests (16)
bs_add_test(test-sqlite-store       Unit/test_sqlite_store.cpp)
bs_add_test(test-path-rules         Unit/test_path_rules.cpp)
... (14 more)

# Integration tests (15)
bs_add_test(test-full-pipeline      Integration/test_full_pipeline.cpp)
... (14 more)

# Relevance gate tests
bs_add_test(test-query-service-relevance-fixture 
    Integration/test_query_service_relevance_fixture.cpp)
target_compile_definitions(test-query-service-relevance-fixture PRIVATE
    BS_RELEVANCE_BASELINES_PATH="${CMAKE_CURRENT_SOURCE_DIR}/relevance/baselines.json")

bs_add_test(test-ui-sim-query-suite 
    Integration/test_ui_sim_query_suite.cpp)
target_compile_definitions(test-ui-sim-query-suite PRIVATE
    BS_RELEVANCE_SUITE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/relevance/ui_sim_query_suite.json")
set_tests_properties(test-ui-sim-query-suite PROPERTIES
    ENVIRONMENT "BS_RELEVANCE_GATE_MODE=report_only")
```

---

## 6. DATA MODELS & SHARED TYPES

### 6.1 Core Data Models (Serializable)

**SearchResult** (`search_result.h`):
```cpp
struct SearchResult {
    int64_t itemId;                          // DB primary key
    QString path;                            // Full file path
    QString name;                            // File name only
    QString kind;                            // ItemKind (Directory, Text, Code, etc.)
    MatchType matchType;                     // 7 match types
    int fuzzyDistance;                       // Edit distance if fuzzy
    double score;                            // Final ranked score
    double bm25RawScore;                     // Raw BM25 from FTS5
    QString snippet;                         // Content context
    std::vector<Highlight> highlights;       // Match offsets in snippet
    int64_t fileSize;                        // Bytes
    QString modificationDate;                // ISO 8601 string
    bool isPinned;                           // Manual override flag
    int openCount;                           // Access frequency
    QString lastOpenDate;                    // Last interaction
    ScoreBreakdown scoreBreakdown;           // Debug detail
};

struct ScoreBreakdown {
    double baseMatchScore;                   // MatchType points
    double recencyBoost;                     // Time decay factor
    double frequencyBoost;                   // Access count factor
    double contextBoost;                     // CWD + app signal
    double pinnedBoost;                      // Pin override
    double junkPenalty;                      // Downloads, cache penalty
    double semanticBoost;                    // ONNX embedding match (M2+)
};
```

**MatchType enum** (7 types):
```cpp
enum class MatchType {
    ExactName,           // 200 base points  — filename matches exactly
    PrefixName,          // 150 base points  — filename prefix match
    ContainsName,        // 100 base points  — filename substring
    ExactPath,           // 90 base points   — full path exact match
    PrefixPath,          // 80 base points   — path prefix match
    Content,             // Variable (BM25)  — indexed content match
    Fuzzy,               // 30 base points   — edit distance < threshold
};
```

**QueryContext** (M2+):
```cpp
struct QueryContext {
    std::optional<QString> cwdPath;                     // Current working dir
    std::optional<QString> frontmostAppBundleId;        // Active app
    std::vector<QString> recentPaths;                   // Clipboard, history
};
```

**IndexHealth**:
```cpp
struct IndexHealth {
    int fileCount;                           // Total indexed files
    int64_t totalSize;                       // Aggregate size
    QString lastScan;                        // ISO 8601 timestamp
    int errorCount;                          // Failed extractions
    double vectorCoverage;                   // % with embeddings (M2+)
};
```

**ItemKind enum**:
```cpp
enum class ItemKind {
    Directory,     Text,      Code,       Markdown,
    Pdf,           Image,     Archive,    Binary,      Unknown,
};
```

**Sensitivity enum** (for path classification):
```cpp
enum class Sensitivity {
    Normal,        // Index fully (most files)
    Sensitive,     // Metadata only (.ssh, .gnupg, .aws)
    Hidden,        // Exclude completely (.DS_Store, etc.)
};
```

### 6.2 IPC Message Envelopes

**IpcRequest/IpcResponse** (`ipc_messages.h`):
```cpp
struct IpcRequest {
    uint64_t id = 0;               // Unique request ID
    QString method;                // RPC method name
    QByteArray paramsJson;         // JSON-encoded params
};

struct IpcResponse {
    uint64_t id = 0;               // Matches request id
    bool isError = false;          // Error flag
    IpcErrorCode errorCode;        // Error details
    QString errorMessage;
    QByteArray resultJson;         // JSON result
};

enum class IpcErrorCode : int {
    InvalidParams      = 1,
    Timeout            = 2,
    PermissionDenied   = 3,
    NotFound           = 4,
    AlreadyRunning     = 5,
    InternalError      = 6,
    Unsupported        = 7,
    CorruptedIndex     = 8,
    ServiceUnavailable = 9,
};
```

---

## 7. CONFIGURATION & SETTINGS

### 7.1 Settings Storage

**Location:** `~/.betterspotlight/config.json`

```json
{
  "version": 2,
  "general": {
    "hotkey": "Cmd+Space",
    "autoStart": true,
    "maxResults": 20
  },
  "indexing": {
    "rootPaths": [
      "/Users/alice/Documents",
      "/Users/alice/Developer"
    ],
    "pauseIndexing": false
  },
  "exclusions": {
    "useDefaults": true,
    "customPatterns": [
      "*.log",
      "vendor/"
    ]
  },
  "privacy": {
    "enableFeedback": true,
    "retentionDays": 30
  },
  "weights": {
    "exactNameWeight": 200,
    "prefixNameWeight": 150,
    "contentMatchWeight": 100,
    "recencyDecay": 0.95,
    "frequencyBoost": 1.5,
    "cwdProximityBoost": 1.2,
    "appContextBoost": 1.1,
    "pinnedItemBoost": 2.0,
    "cachePathPenalty": 0.1
  }
}
```

### 7.2 Database Schema

**SQLite tables (7 normalized):**

```sql
-- Files table (core document store)
CREATE TABLE files (
    id INTEGER PRIMARY KEY,
    path TEXT UNIQUE NOT NULL,
    name TEXT NOT NULL,
    kind TEXT NOT NULL,          -- ItemKind enum
    size INTEGER,
    created_at REAL,
    modified_at REAL,
    indexed_at REAL,
    sensitivity TEXT DEFAULT 'Normal'
);

-- File content chunks (FTS5)
CREATE VIRTUAL TABLE file_content USING fts5(
    file_id UNINDEXED,
    chunk_text,                  -- Text to search
    chunk_order,                 -- Sequence for reconstruction
    start_char                   -- Position in original
);

-- Content metadata
CREATE TABLE file_metadata (
    id INTEGER PRIMARY KEY,
    file_id INTEGER UNIQUE,
    extracted_text TEXT,
    language_code TEXT,
    page_count INTEGER,
    has_images BOOLEAN,
    charset TEXT
);

-- Access frequency tracking
CREATE TABLE frequency (
    id INTEGER PRIMARY KEY,
    file_id INTEGER UNIQUE,
    access_count INTEGER DEFAULT 1,
    last_accessed REAL
);

-- Pinned items (manual overrides)
CREATE TABLE pinned_items (
    id INTEGER PRIMARY KEY,
    file_id INTEGER UNIQUE,
    pin_reason TEXT,
    created_at REAL
);

-- Vector mapping (generation-aware M2+/M3)
CREATE TABLE vector_map (
    item_id INTEGER NOT NULL,
    hnsw_label INTEGER NOT NULL,
    generation_id TEXT NOT NULL,
    model_id TEXT NOT NULL,
    dimensions INTEGER NOT NULL,
    provider TEXT NOT NULL,
    passage_ordinal INTEGER NOT NULL DEFAULT 0,
    migration_state TEXT NOT NULL DEFAULT 'active'
);

-- Index health metadata
CREATE TABLE index_health (
    id INTEGER PRIMARY KEY,
    schema_version INTEGER,
    last_full_scan REAL,
    total_files INTEGER,
    total_size INTEGER,
    error_count INTEGER,
    vector_count INTEGER
);
```

### 7.3 .bsignore Format

**File location:** `~/.betterspotlight/.bsignore` or `.bsignore` per-directory

**Format:** Gitignore-style

```
# Built-in defaults (always applied):
node_modules/
.git
.svn
.hg
.DS_Store
Library/Caches
Library/Logs
Library/Application Support/*/Cache*
vendor/
build/
dist/
*.o
*.a
*.so
.pytest_cache/

# User custom:
*.log
work/
secrets/
```

**Evaluation:** PathRules applies patterns at FileScanner level before queueing.

---

## 8. MILESTONE 3 FOCUS: KEY ARCHITECTURAL COMPONENTS

### 8.1 Weaknesses Identified in M2 (Remediation Focus)

From `EXECUTIVE_RETRIEVAL_REMEDIATION_MEMO_2026-02-09.md`:

**Relevance:** 15-20% failure rate on 50-query suite
- Exact name matches not bubbling to top (should be 1st)
- Fuzzy matching too aggressive (edit distance threshold)
- Context boosting underpowered (CWD not strong enough)

**Integration:**
- test_ui_sim_query_suite.cpp missing expectations for all 50 queries
- test_query_service_relevance_fixture.cpp baseline thresholds unclear
- No assertion on specific score breakdowns

**Service Stability:**
- test_crash_isolation.cpp weak (not testing actual service restart)
- No heartbeat monitoring between UI and services
- IndexerService queue backpressure not enforced

### 8.2 M3 Architecture Improvements

**For Milestone 3:**

1. **Enhanced Ranking:**
   - Exact name match → hardwired to top if present (override score)
   - Fuzzy threshold reduction: 3 → 2 edit distance
   - Context boosting multiplication (not addition)

2. **Relevance Testing:**
   - Expand fixture to 100 queries across 8 categories
   - Auto-generate baselines from manual audit
   - Continuous measurement (report_only mode)

3. **Service Resilience:**
   - IPC heartbeat (ping every 5s)
   - Auto-restart with backoff (exponential, max 60s)
   - Service health dashboard in UI

4. **Indexing Robustness:**
   - Work queue disk persistence (crashes survive restart)
   - Backpressure gates (pause if queue > 50k items)
   - Fine-grained error reporting (per file, not aggregate)

### 8.3 Key Files for M3 Implementation

**Architecture Review:**
```
Src/core/ranking/scorer.cpp          [30KB] — Modify ranking formula
Src/core/ranking/context_signals.cpp [15KB] — Enhance CWD/app detection
Src/services/query/query_service.cpp [25KB] — Top-N override logic
```

**Test Infrastructure:**
```
Tests/relevance/ui_sim_query_suite.json  [50→100 queries]
Tests/Integration/test_ui_sim_query_suite.cpp [enforce gates]
Tests/Integration/test_query_service_relevance_fixture.cpp [baselines]
```

**Service Hardening:**
```
Src/core/ipc/service_base.cpp        [Heartbeat mechanism]
Src/core/ipc/supervisor.cpp          [Restart logic]
Src/services/indexer/main.cpp        [Work queue persistence]
```

---

## 9. DATA FLOW DIAGRAMS

### 9.1 Indexing Pipeline (Critical Path)

```
┌─────────────────────────────────────────────────────────────────┐
│ Stage 1: FSEvents Detection (macOS CoreServices)                │
│  • File system change detected                                  │
│  • {path, type, timestamp}                                      │
└────────────────────┬────────────────────────────────────────────┘
                     │
                     ↓
┌─────────────────────────────────────────────────────────────────┐
│ Stage 2: Path Validation (PathRules, .bsignore)                │
│  • Check exclusion patterns                                     │
│  • Classify sensitivity (Normal/Sensitive/Hidden)               │
│  • Result: Include, MetadataOnly, or Exclude                    │
└────────────────────┬────────────────────────────────────────────┘
                     │ (Include or MetadataOnly)
                     ↓
┌─────────────────────────────────────────────────────────────────┐
│ Stage 3: Work Queue Ingestion                                  │
│  • Add to IndexerService WorkQueue                              │
│  • Priority: recent files first, deletes first                  │
│  • Deduplicate: merge pending operations on same path           │
└────────────────────┬────────────────────────────────────────────┘
                     │
                     ↓
┌─────────────────────────────────────────────────────────────────┐
│ Stage 4: Metadata Extraction                                   │
│  • File scanner reads: size, timestamps, kind                   │
│  • Optional: xattr metadata (macOS MDLS)                        │
│  • Result: FileMetadata struct                                  │
└────────────────────┬────────────────────────────────────────────┘
                     │
                     ↓
┌─────────────────────────────────────────────────────────────────┐
│ Stage 5: Content Extraction                                    │
│  • ExtractionManager → delegates per file type:                 │
│    - TextExtractor: code, config, plaintext (100+ types)        │
│    - PdfExtractor: .pdf via Poppler (1000-page limit)           │
│    - OcrExtractor: scanned images via Tesseract                 │
│    - Timeout: 30 seconds per file                               │
│  • Result: {text, lang, pageCount, ...}                         │
└────────────────────┬────────────────────────────────────────────┘
                     │
                     ↓
┌─────────────────────────────────────────────────────────────────┐
│ Stage 6: Text Chunking                                         │
│  • Split at word/sentence/paragraph boundaries                  │
│  • Target: ~256 tokens per chunk (preserve context)             │
│  • Tag: chunk_order, start_char for snippet generation          │
│  • Result: [Chunk1, Chunk2, ..., ChunkN]                        │
└────────────────────┬────────────────────────────────────────────┘
                     │
                     ↓
┌─────────────────────────────────────────────────────────────────┐
│ Stage 7: FTS5 Index Insertion                                  │
│  • Insert into file_content (FTS5 virtual table)                │
│  • Chunk text indexed with Porter stemmer                       │
│  • Metadata stored in file_metadata table                       │
│  • Frequency tracking: access_count, last_accessed              │
│  • Transaction: atomic batch commit                             │
└────────────────────┬────────────────────────────────────────────┘
                     │
                     ↓
┌─────────────────────────────────────────────────────────────────┐
│ Stage 8 (M2+): Embedding Computation                           │
│  • EmbeddingManager: ONNX Runtime inference                     │
│  • Model: Manifest-driven (v2 primary 1024-dim float32)         │
│  • Tokenizer: WordPiece with vocab.txt                          │
│  • CoreML-first provider policy with CPU fallback               │
│  • Vector Store: generation-aware metadata + dual-index files   │
│  • HNSW Index: incrementally add to approximate NN index        │
└────────────────────┬────────────────────────────────────────────┘
                     │
                     ↓
              ✓ File indexed
         Ready for search
```

### 9.2 Query Pipeline (Critical Path)

```
┌─────────────────────────────────────────────────────────────────┐
│ User Input: Type search query in SearchPanel                    │
│ Example: "config parser"                                        │
└────────────────────┬────────────────────────────────────────────┘
                     │
                     ↓
┌─────────────────────────────────────────────────────────────────┐
│ QueryNormalizer: Preprocess                                    │
│  • Lowercase: "config parser" → "config parser"                 │
│  • Remove stopwords (optional): "config parser"                 │
│  • Unicode normalize (é → e, etc.)                              │
│  • Result: normalized_query                                     │
└────────────────────┬────────────────────────────────────────────┘
                     │
                     ↓
┌─────────────────────────────────────────────────────────────────┐
│ QueryParser: Tokenize & Parse                                  │
│  • Split: ["config", "parser"]                                  │
│  • Detect phrases (quoted): '"config parser"' → phrase          │
│  • TypoLexicon: suggest corrections if typo detected            │
│  • Result: query_tokens, phrase_flags                           │
└────────────────────┬────────────────────────────────────────────┘
                     │
                     ↓
┌─────────────────────────────────────────────────────────────────┐
│ Stage 1: Lexical Search (FTS5 BM25)                            │
│  • FTS5 query: "config parser"                                  │
│  • Match files where:                                           │
│    - Filename contains both tokens (prefix match)               │
│    - OR content chunks contain both (BM25 ranked)               │
│  • Return: raw_results [file_id, bm25_score, ...]              │
└────────────────────┬────────────────────────────────────────────┘
                     │
                     ↓
┌─────────────────────────────────────────────────────────────────┐
│ Stage 2: Context Signal Collection                             │
│  • Current working directory (CWD): /Users/alice/Developer      │
│  • Frontmost app: Xcode (IDE category)                          │
│  • Recent paths: clipboard, history                             │
│  • User preferences: path boosts, type affinities (M2)          │
│  • Result: QueryContext                                         │
└────────────────────┬────────────────────────────────────────────┘
                     │
                     ↓
┌─────────────────────────────────────────────────────────────────┐
│ Stage 3: Match Classification (7 types)                        │
│ For each result:                                                │
│  • Check filename exact match? → ExactName (200 pts)            │
│  • Check filename prefix? → PrefixName (150 pts)                │
│  • Check filename contains? → ContainsName (100 pts)            │
│  • Check path exact? → ExactPath (90 pts)                       │
│  • Check path prefix? → PrefixPath (80 pts)                     │
│  • Content match? → Content (BM25 variable)                     │
│  • Fuzzy match? → Fuzzy (30 pts if distance < 2)                │
│  • Result: MatchType per result                                 │
└────────────────────┬────────────────────────────────────────────┘
                     │
                     ↓
┌─────────────────────────────────────────────────────────────────┐
│ Stage 4: Semantic Search (M2+)                                 │
│  • EmbeddingManager: encode query vector via ONNX              │
│  • VectorIndex: KNN search (k=50) in HNSW index                 │
│  • SearchMerger: rank fusion (lexical + semantic)               │
│  • Result: semantic_results [...]                               │
└────────────────────┬────────────────────────────────────────────┘
                     │
                     ↓
┌─────────────────────────────────────────────────────────────────┐
│ Stage 5: Scoring & Ranking (Scorer)                            │
│ For each result:                                                │
│  baseScore = matchTypeBasePoints[matchType]                     │
│  recencyBoost = decay(now - modified_time)                      │
│  frequencyBoost = 1.0 + log(access_count + 1)                   │
│  contextBoost = cwdProximity + appContext + ...                 │
│  semanticBoost = (M2) embedding cosine similarity               │
│  junkPenalty = (Downloads? → 0.1, Cache? → 0.05)                │
│                                                                 │
│  finalScore = baseScore                                         │
│              + recencyBoost * weight[recency]                   │
│              + frequencyBoost * weight[frequency]               │
│              + contextBoost * weight[context]                   │
│              + semanticBoost * weight[semantic]                 │
│              - junkPenalty                                      │
│  (if pinned) finalScore *= 2.0                                  │
│                                                                 │
│  Result: scored_results with finalScore per item                │
└────────────────────┬────────────────────────────────────────────┘
                     │
                     ↓
┌─────────────────────────────────────────────────────────────────┐
│ Stage 6: Sort & Return Top-N                                   │
│  • Sort by finalScore descending                                │
│  • Take top 10 (or configurable limit)                          │
│  • Generate snippet from matched content chunk                  │
│  • Highlight match positions in snippet                         │
│  • Include ScoreBreakdown for transparency                      │
│  • Result: [SearchResult, SearchResult, ...]                    │
└────────────────────┬────────────────────────────────────────────┘
                     │
                     ↓
┌─────────────────────────────────────────────────────────────────┐
│ UI Display: Show results                                        │
│  • Render result items with snippet + highlights               │
│  • User can navigate with ↑/↓, press Enter to open             │
│  • Cmd+R reveals in Finder, Cmd+Shift+C copies path            │
└────────────────────┬────────────────────────────────────────────┘
                     │
                     ↓
┌─────────────────────────────────────────────────────────────────┐
│ Stage 7 (M2+): Feedback Recording                              │
│  • Log interaction: query, selected_path, rank, timestamp      │
│  • InteractionTracker persists to SQLite                        │
│  • FeedbackAggregator: periodically recompute signals           │
│    - Path preferences (boost frequently selected paths)         │
│    - Type affinity (.cpp ↔ .h correlation)                      │
│  • Signals fed into next ranking round                          │
└────────────────────┬────────────────────────────────────────────┘
                     │
                     ↓
              ✓ Search complete
          Ready for next query
```

---

## 10. MODULE OWNERSHIP & MAINTENANCE

### 10.1 Core Module Owners (Typical Assignment)

| Module | LOC | Owner | Dependencies | Risk Level |
|--------|-----|-------|--------------|-----------|
| betterspotlight-core-shared | 800 | Platform | None | Low |
| betterspotlight-core-fs | 2000 | System | shared, CoreServices | Medium |
| betterspotlight-core-extraction | 3500 | Extraction Lead | shared, Poppler, Tesseract | High |
| betterspotlight-core-ipc | 1500 | IPC Lead | shared, Qt Network | Medium |
| betterspotlight-core-indexing | 4000 | Indexing Lead | fs, extraction, shared | High |
| betterspotlight-core-ranking | 1500 | Ranking Lead | shared | Low |
| betterspotlight-core-feedback | 2000 | ML Lead | shared, sqlite3 | Medium |
| betterspotlight-core-embedding | 2500 | ML Lead | shared, ONNXRuntime | High |
| betterspotlight-core-vector | 1800 | ML Lead | shared, ranking, hnswlib | High |
| QueryService | 2000 | Query Lead | All core libs | Medium |
| IndexerService | 1500 | Indexing Lead | Core + services | Medium |
| ExtractorService | 800 | Extraction Lead | Core + services | Low |

### 10.2 Test Ownership

| Test Suite | Owner | Coverage | Maintenance Effort |
|-----------|-------|----------|-------------------|
| Unit tests (16) | Module owners | 70% | Low |
| Integration tests (15) | Integration lead | 60% | Medium |
| Relevance tests (2) | Ranking lead | Score accuracy | High |
| Fixtures | QA | Test data | Low |

---

## 11. CHECKLIST FOR MILESTONE 3 PLANNING

### Architecture Review
- [ ] Validate 4-layer dependency structure (no cycles)
- [ ] Verify service isolation (3 separate binaries, Unix socket IPC)
- [ ] Confirm CMake build hygiene (all tests link correctly)
- [ ] Check test fixture availability (standard_home_v1 populated, 50-query suite ready)

### Performance & Reliability
- [ ] Indexing throughput: target 500 files/sec (batch extraction)
- [ ] Query latency: target <100ms for 50-query suite (lexical + context)
- [ ] Service isolation: verify crash of one service doesn't kill others
- [ ] Index durability: test restart after simulated crash (work queue recovery)

### Relevance & UX
- [ ] Exact name matches rank #1 (override algorithm)
- [ ] Fuzzy threshold: reduce to edit distance 2
- [ ] Context boosting: CWD proximity multiplier > 2x
- [ ] Relevance gate: pass 95% of 50-query suite
- [ ] Score transparency: ScoreBreakdown visible in UI for top 3 results

### Testing Infrastructure
- [ ] test_query_service_relevance_fixture.cpp: baseline per query
- [ ] test_ui_sim_query_suite.cpp: enforce relevance gates (not report-only)
- [ ] Fixtures: expand to 100 queries, 8 categories
- [ ] Integration: heartbeat tests, service restart tests

### Documentation
- [ ] Update ranking-scoring.md with new formulas
- [ ] Add M3 roadmap (error messaging, polishing, 95% reliability target)
- [ ] Document service restart policy (exponential backoff)
- [ ] Create runbook for relevance gate failure diagnosis

---

## APPENDIX: FILE MANIFEST

**All source files by module:**

```
Src/core/shared/ (6 files)
  types.h/cpp, search_result.h/cpp, chunk.h/cpp, index_health.h,
  settings.h, scoring_types.h, ipc_messages.h, search_options.h,
  fda_check.h, logging.h, settings_manager.h

Src/core/fs/ (4 files)
  path_rules.h/cpp, file_monitor_macos.h/cpp, file_scanner.h/cpp,
  bsignore_parser.h/cpp

Src/core/extraction/ (6 files)
  extraction_manager.h/cpp, text_extractor.h/cpp, mdls_text_extractor.h/cpp,
  pdf_extractor.h/cpp, ocr_extractor.h/cpp, text_cleaner.h/cpp

Src/core/ipc/ (5 files)
  message.h/cpp, socket_server.h/cpp, socket_client.h/cpp,
  service_base.h/cpp, supervisor.h/cpp

Src/core/indexing/ (4 files)
  pipeline.h/cpp, work_queue.h/cpp, indexer.h/cpp, chunker.h/cpp

Src/core/ranking/ (3 files)
  scorer.h/cpp, match_classifier.h/cpp, context_signals.h/cpp

Src/core/index/ (4 files)
  sqlite_store.h/cpp, migration.h/cpp, schema.h, typo_lexicon.h/cpp

Src/core/query/ (2 files)
  query_normalizer.h/cpp, query_parser.h/cpp

Src/core/embedding/ (4 files)
  embedding_manager.h/cpp, embedding_pipeline.h/cpp,
  tokenizer.h/cpp, quantizer.h/cpp (legacy compatibility)

Src/core/vector/ (3 files)
  vector_index.h/cpp, vector_store.h/cpp, search_merger.h/cpp

Src/core/feedback/ (4 files)
  interaction_tracker.h/cpp, feedback_aggregator.h/cpp,
  path_preferences.h/cpp, type_affinity.h/cpp

Src/services/indexer/ (2 files)
  indexer_service.h/cpp, main.cpp

Src/services/extractor/ (2 files)
  extractor_service.h/cpp, main.cpp

Src/services/query/ (3 files)
  query_service.h/cpp, query_service_m2.cpp, main.cpp

Src/app/ (8 files + QML)
  main.cpp, service_manager.h/cpp, hotkey_manager.h/cpp,
  search_controller.h/cpp, settings_controller.h/cpp,
  onboarding_controller.h/cpp, status_bar_bridge.h

Tests/Unit/ (16 tests)
Tests/Integration/ (15 tests)
Tests/relevance/ (baselines.json, ui_sim_query_suite.json)
Tests/Fixtures/ (standard_home_v1 test corpus)
```

---

**END OF DOCUMENT**

Generated: February 9, 2026  
For: Milestone 3 Planning & Architecture Review  
Prepared by:  Code (Principal Engineer Support)
