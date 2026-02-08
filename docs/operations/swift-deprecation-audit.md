# Swift Codebase Deprecation Audit

**Date:** 2026-02-07
**Auditor:** Principal Engineer (automated cross-reference audit)
**Scope:** All 57 Swift-era components vs. 35 C++ production files + 12 C++ test files

---

## How to read this document

Every Swift file gets one of three classifications:

- **SAFE TO DELETE** -- C++ fully supersedes it. No logic, edge cases, or test coverage is lost.
- **DELETE AFTER PORTING** -- C++ covers the core functionality, but specific logic items need to be ported first. Each item is listed explicitly.
- **DO NOT DELETE YET** -- Significant functionality exists only in Swift. Must remain as reference until C++ catches up.

> **Important architectural note:** Some items the audit flagged as "missing from C++"
> are actually *relocated*, not missing. For example, Swift extractors chunk text internally,
> but C++ has a dedicated `Chunker` class in `src/core/indexing/chunker.cpp`. These are
> called out where relevant.

---

## 1. ~~SAFE TO DELETE (21 files)~~ COMPLETED 2026-02-07

These have been removed. See correction below about `packaging/macos/Info.plist`.

### Xcode Project Infrastructure

| File | Reason |
|------|--------|
| `BetterSpotlight.xcodeproj/` (entire directory) | Xcode project, replaced by CMakeLists.txt |
| `Package.swift` | SPM manifest, replaced by CMake |
| `Package.resolved` | SPM lockfile, not applicable |

### Info.plist / Entitlements

| File | Reason |
|------|--------|
| `App/Info.plist` | Xcode build artifact, Qt uses its own packaging |
| `App/BetterSpotlight.entitlements` | Xcode entitlements, handled differently in Qt |
| `Services/IndexerXPC/Info.plist` | XPC service plist, replaced by Unix socket services |
| `Services/QueryXPC/Info.plist` | Same |
| `Services/ExtractorXPC/Info.plist` | Same |

### XPC Service Entry Points

| File | C++ Replacement | Reason |
|------|-----------------|--------|
| `Services/IndexerXPC/main.swift` | `src/services/indexer/main.cpp` | Equivalent entry point |
| `Services/QueryXPC/main.swift` | `src/services/query/main.cpp` | Equivalent entry point |
| `Services/ExtractorXPC/main.swift` | `src/services/extractor/main.cpp` | Equivalent entry point |

### App Entry / UI Framework

| File | C++ Replacement | Reason |
|------|-----------------|--------|
| `App/BetterSpotlightApp.swift` | `src/app/main.cpp` | SwiftUI App replaced by QApplication |
| `App/UI/StatusBarController.swift` | `src/app/main.cpp` (tray setup) | NSStatusBar replaced by QSystemTrayIcon |
| `App/SearchPanel/SearchPanelController.swift` | `src/app/search_controller.cpp` | NSPanel replaced by QML SearchWindow |
| `App/SearchPanel/SearchPanelView.swift` | `src/app/qml/SearchPanel.qml` | SwiftUI view replaced by QML (*) |

(*) QML SearchPanel is less feature-rich (missing context menu, no-results view, keyboard hints). These are UI polish items, not logic -- safe to delete Swift and implement in QML from scratch.

### IPC Protocol Definitions

| File | C++ Replacement | Reason |
|------|-----------------|--------|
| `Shared/IPC/ServiceProtocols.swift` | `src/core/ipc/message.cpp` + `service_base.cpp` | @objc protocols replaced by JSON-RPC dispatch |
| `Shared/IPC/ServiceClient.swift` | `src/core/ipc/socket_client.cpp` | XPC client replaced by Unix socket client |

### Built Artifacts

| File | Reason |
|------|--------|
| `BetterSpotlight.app/` (entire directory) | Old build output, not source |
| `packaging/macos/Info.plist` | **CORRECTION:** Was used by Qt CMake build (`MACOSX_BUNDLE_INFO_PLIST`). Relocated to `src/app/Info.plist` and CMake reference updated. |
| `packaging/macos/entitlements.plist` | Old packaging config (not referenced by CMake) |

---

## 2. DELETE AFTER PORTING (20 files)

C++ covers the primary functionality, but specific items need to be ported first.
Each "Port first" item is an actionable work item.

### Core/Index/SQLiteStore.swift

**C++ counterpart:** `src/core/index/sqlite_store.cpp` + `schema.h`

Port first:
- [ ] **Tags CRUD methods** -- Schema exists in C++ (`schema.h` lines 63-73) but no `insertTag()`, `deleteTag()`, `getTagsForItem()` methods in `sqlite_store.cpp`
- [ ] **Explicit checkpoint() API** -- Swift exposes `PRAGMA wal_checkpoint(TRUNCATE)`. C++ only has auto-checkpoint pragma. Add an explicit method.
- [ ] **Feedback context field** -- Swift stores `context_json` in feedback table. C++ feedback table (schema.h line 99) has no context column. Add it or decide it's not needed.
- [ ] **Statistics helpers** -- Swift has `getTotalContentChunkCount()` and `getDatabaseSize()`. C++ `getHealth()` is close but missing these specific queries.

### Core/FS/PathRules.swift

**C++ counterpart:** `src/core/fs/path_rules.cpp`

Port first:
- [ ] **Per-path override system** -- Swift has `overrides: [String: FolderClassification]` allowing user-configurable per-path rules. C++ has no override mechanism -- validation is purely rule-based. This is the user's way to say "always include/exclude this specific folder."
- [ ] **`suggestedClassification()` utility** -- Swift has a static function that categorizes common home directories (Documents, Library, .config, .ssh). Used by OnboardingView. Port when onboarding is built.

Note: Swift's regex-based pattern matching is replaced by C++'s custom glob implementation. Not a loss, just different.

### Core/FS/FSEvents.swift

**C++ counterpart:** `src/core/fs/file_monitor_macos.cpp`

Port first:
- [ ] **Error callback** -- Swift's `FSEventsWatcherDelegate` has `didFailWithError`. C++ has no error reporting path from FSEvents to caller. Add an error callback/signal.
- [ ] **Event ID preservation** -- Swift preserves `FSEventStreamEventId` for resume-from-last-event. C++ discards it. Needed for crash recovery.

Note: C++ adds debouncing and event classification that Swift lacks. These are improvements, not losses.

### Core/FS/FileScanner.swift

**C++ counterpart:** `src/core/fs/file_scanner.cpp`

Port first:
- [ ] **Depth limiting** -- Swift has `maxDepth` parameter. C++ will recurse indefinitely. Add a depth cap to prevent runaway scans.
- [ ] **Symlink cycle prevention** -- Swift uses `.skipsPackageDescendants`. C++ has no explicit protection against symlink loops in recursive scan.
- [ ] **Owner field** -- Swift captures file owner from FileManager attributes. C++ `FileMetadata` doesn't populate owner.

Note: Swift's `AsyncThrowingStream` (lazy evaluation) vs C++'s `std::vector` (all in memory) is an architectural choice, not a bug. For typical user home directories, vector is fine.

### Core/Ranking/Scoring.swift

**C++ counterpart:** `src/core/ranking/scorer.cpp`

Port first:
- [ ] **Important dotfile list** -- Swift has `isImportantDotfile()` recognizing 9 files (.gitignore, .zshrc, .bashrc, .vimrc, .env, .editorconfig, .prettierrc, .eslintrc). C++ has no equivalent -- dotfiles get zero special treatment.
- [ ] **Reconcile junk patterns** -- Swift includes `/__pycache__/`, `/.next/`, `/coverage/`. C++ includes `/.build/`, `/deriveddata/`, `/.trash/`, `/vendor/bundle/`. Neither has the other's patterns. Merge both lists.

Note: Recency boost differs fundamentally (Swift: stepped tiers, C++: exponential decay). **This is intentional.** The C++ exponential decay is the documented spec (doc 06). Swift's stepped approach was a simpler approximation. C++ is correct per spec.

### Core/Ranking/ContextSignals.swift

**C++ counterpart:** `src/core/ranking/context_signals.cpp`

Port first:
- [ ] **InteractionTracker** -- Swift tracks which paths were selected for which queries, file type preferences, and path preferences. This entire subsystem (query pattern learning) is absent from C++. ~60 lines of logic.
- [ ] **Recent paths tracking** -- Swift maintains a deduped list of recently accessed paths (capped at 50) for context boosting. C++ has no runtime path tracking.
- [ ] **App context type classification** -- Swift returns `isDevContext`, `isTerminalContext`, `isDocumentContext`, `isDesignContext` booleans. C++ only maps bundle ID to extensions. The boolean classification drives context-aware scoring.

### Core/Index/LexicalIndex/LexicalIndex.swift

**C++ counterpart:** `src/core/index/sqlite_store.cpp` (FTS5 search methods)

Port first:
- [ ] **FTS5 query sanitization** -- Swift strips dangerous characters (`"'*():^~`), collapses spaces, adds prefix matching for single words, generates phrase-OR-word queries for multi-word input. **C++ passes user input directly to FTS5.** This is a crash/injection risk. High priority.
- [ ] **Highlight range parsing** -- Swift parses `<mark>` tags from FTS5 snippets into `HighlightRange` structs (start/end positions). C++ has a `Highlight` struct (search_result.h:24) but doesn't populate it from snippets. The QML UI needs these for search result highlighting.

Note: Match type classification in Swift (exactName, prefixName, substringName, etc.) is handled differently in C++ via `MatchClassifier`. Not missing, relocated.

### Core/Extraction/TextExtractor.swift

**C++ counterpart:** `src/core/extraction/text_extractor.cpp`

Port first:
- [ ] **Metadata extraction** -- Swift extracts title, author, word count, character count, language, encoding, MIME type. C++ returns raw content only. Metadata populates the `items` table columns.
- [ ] **macOS Roman encoding fallback** -- Swift tries UTF-8, ISO Latin-1, macOS Roman. C++ tries UTF-8, Latin-1 only. macOS Roman handles legacy Mac files.

Note: Chunking is NOT missing from C++. Swift chunks inside the extractor. C++ separates this into `src/core/indexing/chunker.cpp` (142 lines). This is a deliberate architectural improvement.

### Core/Extraction/PdfExtractor.swift

**C++ counterpart:** `src/core/extraction/pdf_extractor.cpp`

Port first:
- [ ] **Text cleaning** -- Swift collapses multiple spaces, collapses triple+ newlines, removes control characters, trims whitespace. C++ inserts raw Poppler output. Add a `cleanExtractedText()` utility.

Note: Chunking and metadata are handled by Chunker and FileScanner respectively, not missing.

### Core/Extraction/OcrExtractor.swift

**C++ counterpart:** `src/core/extraction/ocr_extractor.cpp`

Port first:
- [ ] **Multi-language support** -- Swift accepts configurable `recognitionLanguages`. C++ hardcodes `"eng"` in Tesseract init. Add language configuration.
- [ ] **Layout preservation** -- Swift sorts Vision observations by visual position (top-to-bottom, left-to-right) with line break detection. C++ gets flat text from Tesseract. Quality difference for multi-column documents.
- [ ] **HEIC/HEIF/GIF support** -- Swift supports 10 image formats including HEIC/HEIF/GIF. C++ supports 7 (missing HEIC, HEIF, GIF).

### Core/Extraction/ExtractionManager.swift

**C++ counterpart:** `src/core/extraction/extraction_manager.cpp`

Port first:
- [ ] **Settings-driven OCR toggle** -- Swift conditionally loads OcrExtractor based on `settings.enableOCR`. C++ always instantiates all extractors. Wasteful if OCR is disabled.
- [ ] **Batch extraction** -- Swift has `extractBatch()` with configurable concurrency and progress callbacks. C++ processes files individually through the pipeline. Not blocking, but batch mode would be useful for initial index build.

### Services/IndexerXPC/IndexerService.swift

**C++ counterpart:** `src/services/indexer/indexer_service.cpp`

Port first:
- [ ] **Indexing statistics** -- Swift computes `itemsIndexed`, `itemsFailed`, `bytesProcessed`, `averageExtractionTimeMs`, `extractionTimesByType`. C++ has no statistics endpoint. Needed for Index Health UI.

Note: Pause/resume, work queue prioritization, and FSEvents setup are handled by Pipeline class in C++. Not missing, relocated.

### Services/QueryXPC/QueryService.swift

**C++ counterpart:** `src/services/query/query_service.cpp`

Port first:
- [ ] **Per-root health status** -- Swift builds `IndexRootStatus` for each configured root with path-specific item counts and error counts. C++ returns flat health metrics. Needed for Index Health UI.
- [ ] **Health status classification** -- Swift determines healthy/degraded/unhealthy based on failure thresholds. C++ returns raw numbers only.

### Services/ExtractorXPC/ExtractorService.swift

**C++ counterpart:** `src/services/extractor/extractor_service.cpp`

Port first:
- [ ] **Word/character count in extraction response** -- Swift returns these. C++ doesn't compute them. Populates index metadata.

### App/Hotkey/HotkeyManager.swift

**C++ counterpart:** `src/app/hotkey_manager.cpp`

Port first:
- [ ] **`isHotkeyAvailable()` check** -- Swift defines this (currently returns true). C++ lacks it. Needed to validate user-configured hotkeys before registration.

Note: C++ hotkey manager is actually MORE comprehensive (better keymap table, Qt property/signal model). Swift can be deleted once the availability check is added.

### App/UI/AppState.swift

**C++ counterpart:** State spread across `search_controller.cpp` + `service_manager.cpp`

Port first:
- [ ] **Full Disk Access check** -- Swift checks FDA via test file access and can open System Preferences. C++ has no permission verification. Critical for macOS -- without FDA, file scanning silently fails on protected directories.
- [ ] **Settings persistence** -- Swift has `loadSettings()` / `saveSettings()` to JSON file. C++ settings loading is not visible. Verify it exists or add it.
- [ ] **Default index roots** -- Swift builds defaults from home directory (Documents, Desktop, Downloads, Developer, Projects). C++ doesn't show default settings generation.

### Shared/Models/IndexItem.swift

**C++ counterpart:** `src/core/shared/types.h`

Port first:
- [ ] **Computed path helpers** -- Swift has `filename`, `parentPath`, `fileExtension` computed properties. Verify C++ equivalents exist where needed in UI/scoring code.

Note: `ItemKind` enums diverge intentionally. Swift has `application`, `bundle`, `symlink`. C++ has `Text`, `Code`, `Markdown`, `Pdf`, `Image`, `Archive`, `Binary`, `Unknown`, `Directory`. C++ classification is extension-based (better for extraction routing). Not a loss.

### Shared/Models/SearchResult.swift

**C++ counterpart:** `src/core/shared/search_result.h`

Port first:
- [ ] **SearchQuery / SearchOptions structs** -- Swift wraps queries with context and options (includeSemantic, maxResults, etc.). C++ passes raw query string. Add a query wrapper struct.

Note: C++ `SearchResult` actually has MORE fields than Swift (includes `ScoreBreakdown`, `isPinned`, `openCount`, `lastOpenDate`). The C++ version is more complete for its purposes.

### Shared/Models/Settings.swift

**C++ counterpart:** `src/core/shared/settings.h`

Port first:
- [ ] **HotkeyConfig struct** -- Swift has keyCode + modifiers. C++ needs this for configurable hotkeys.
- [ ] **SearchSettings** -- Swift has maxResults, enableFuzzy, fuzzyThreshold, boostRecentFiles, boostFrequentFiles. C++ needs these for Settings UI.
- [ ] **PrivacySettings** -- Swift has enableFeedbackLogging, anonymizeFilePaths, retentionDays. C++ needs these.
- [ ] **ExclusionPattern struct** -- Swift has 32 built-in patterns with descriptions. C++ has patterns but no descriptions for UI display.
- [ ] **SensitiveFolderConfig** -- Swift has 6 default sensitive paths. C++ has these hardcoded in PathRules but not as a configurable model.

### Shared/Models/IndexHealth.swift

**C++ counterpart:** `src/core/shared/index_health.h`

Port first:
- [ ] **IndexHealthStatus enum** -- Swift has healthy/degraded/unhealthy/rebuilding. C++ has only `isHealthy` bool.
- [ ] **IndexRootStatus struct** -- Per-root diagnostics. Needed for Index Health UI.
- [ ] **QueueLengthSample** -- Historical queue length tracking. C++ has no queue history.
- [ ] **IndexManagementAction enum** -- reindex, rebuild, clearCache, etc. C++ has no management action model.

---

## 3. DO NOT DELETE YET (5 files)

These contain significant functionality with NO C++ counterpart at all.

### Core/Index/VectorIndex/VectorIndex.swift

**C++ counterpart:** None (M2 feature)

Contains: Complete in-memory vector index with cosine similarity search, normalization, persistence to disk via JSON. C++ has only a schema table placeholder (`vector_map` in schema.h). Keep as M2 reference.

### Shared/Models/Feedback.swift

**C++ counterpart:** None

Contains: `FeedbackAction` enum (5 types), `FeedbackEntry` struct (14 fields, UUID-keyed), `FeedbackContext` struct, `ItemFrequency` struct (itemId, path, openCount, lastOpened, isPinned). C++ scorer uses frequency data but has no persistence model for feedback collection. Keep as reference for building C++ feedback system.

### App/Settings/IndexHealthView.swift

**C++ counterpart:** QML `SettingsPanel.qml` has a tab header but no implementation

Contains: 5 functional cards (StatusOverview, Statistics, IndexRoots, RecentErrors, Actions) with refresh capability and loading states. This is the entire Index Health monitoring UI. Keep as reference until QML implementation is built.

### App/UI/OnboardingView.swift

**C++ counterpart:** None

Contains: 3-step onboarding flow (Welcome, Permissions/FDA check, HomeMap directory classification). Includes home directory enumeration, cloud folder detection, sensitive folder flagging, and suggested classification logic. Keep as reference until QML onboarding is built.

### Tests/Unit/TextExtractorTests.swift

**C++ counterpart:** None

Contains: 12 tests covering supported extensions, text extraction, chunking, offsets, error handling, OCR configuration. C++ has no text extractor unit tests. Keep as reference until C++ tests are written.

---

## 4. TEST FILES -- SPECIAL HANDLING

Swift tests that have C++ counterparts with BETTER coverage can be deleted.
Swift tests that cover cases C++ tests miss should be kept as reference.

| Swift Test | C++ Test | Verdict |
|------------|----------|---------|
| `Tests/Unit/PathRulesTests.swift` | `tests/Unit/test_path_rules.cpp` | ~~**DELETE**~~ DELETED 2026-02-07 -- C++ has 24 tests vs Swift's 16. |
| `Tests/Unit/ScoringTests.swift` | `tests/Unit/test_scoring.cpp` | ~~**DELETE**~~ DELETED 2026-02-07 -- C++ has 20+ tests vs Swift's 9. |
| `Tests/Unit/TextExtractorTests.swift` | None | **KEEP** -- No C++ equivalent. 12 tests need porting. |
| `Tests/Integration/IndexingIntegrationTests.swift` | `tests/Integration/test_full_pipeline.cpp` | **KEEP as reference** -- Tests different aspects. Swift covers CRUD, frequency tracking, failure recording. C++ covers end-to-end pipeline and FTS5. Both valuable. |
| `Tests/Fixtures/sample_documents/sample.swift` | N/A | **KEEP** -- It's a test fixture (sample file for extraction testing), not old code. |

---

## Summary

| Classification | Count | Action |
|---------------|-------|--------|
| ~~Safe to delete now~~ | ~~21~~ | ~~Remove immediately~~ DONE 2026-02-07 |
| Delete after porting | 20 | Port listed items first, then remove |
| Do not delete yet | 5 | Keep as reference |
| ~~Tests: delete~~ | ~~2~~ | ~~Remove~~ DONE 2026-02-07 |
| Tests: keep | 3 | Keep as reference / port |
| **Remaining Swift files** | **28** | (down from 51) |

### Porting work items (from "Delete After Porting"): 39 items

**High priority (affects correctness/safety):**
1. FTS5 query sanitization (LexicalIndex)
2. Full Disk Access check (AppState)
3. Depth limiting in FileScanner
4. Symlink cycle prevention in FileScanner
5. Error callback in FSEvents monitor
6. Per-path override system in PathRules

**Medium priority (affects feature completeness):**
7. Tags CRUD in SQLiteStore
8. InteractionTracker in ContextSignals
9. Recent paths tracking
10. Multi-language OCR support
11. Settings model completion (hotkey, search, privacy)
12. IndexHealth model (status enum, per-root, queue history)
13. Feedback persistence model
14. Statistics endpoint in IndexerService
15. Per-root health in QueryService
16. Highlight range parsing from FTS5 snippets

**Low priority (quality/polish):**
17. Text cleaning for PDF output
18. macOS Roman encoding fallback
19. Important dotfile list
20. Junk pattern reconciliation
21. OCR layout preservation
22. HEIC/HEIF/GIF image support
23. Settings-driven OCR toggle
24. Batch extraction mode
25. Remaining items (computed path helpers, checkpoint API, etc.)

---

## Recommended execution order

**Phase 1: Safety-critical ports (before any deletion)**
Items 1-6 above. Estimated: 2-3 days.

**Phase 2: Delete safe-to-delete files (21 files)** -- COMPLETED 2026-02-07
Deleted 21 files (2,980 lines) + 2 superseded test files. Relocated `packaging/macos/Info.plist` to `src/app/Info.plist` (Qt build dependency, not a pure Swift artifact). Updated CMake reference. C++ build and all 12 tests pass.

**Phase 3: Feature-critical ports (items 7-16)**
Estimated: 5-7 days. After completion, delete the corresponding Swift files.

**Phase 4: Polish ports (items 17-25)**
Estimated: 3-4 days. After completion, delete remaining Swift files.

**Phase 5: Delete "Do Not Delete Yet" files**
Only after M2 (VectorIndex), onboarding QML, Index Health QML, feedback system, and text extractor tests are all complete.
