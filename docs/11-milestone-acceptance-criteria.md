# BetterSpotlight: Milestone Acceptance Criteria

**Project:** BetterSpotlight (macOS desktop file search app, Qt 6/C++)
**Target Users:** Power users and developers on macOS
**Team Size:** 2-3 people
**Document Version:** 1.0
**Last Updated:** 2026-02-06

---

## Overview

This document defines concrete, testable exit criteria for each milestone. **"Done" is binary**: either all criteria in a milestone pass or the milestone is not complete. There is no partial credit.

Each criterion is verifiable by automated test, manual testing, or built-in diagnostic tool. Acceptance requires documented evidence (test logs, video, screenshot) for any subjective criterion.

---

## MILESTONE 1: BASIC SPOTLIGHT REPLACEMENT

**Goal:** A user can install the app, grant Full Disk Access, and search their files reliably via hotkey. File indexing works end-to-end, search returns results within acceptable latency.

### Functional Acceptance Criteria

#### Search Panel & Input
- [ ] App launches without crash and registers global hotkey (default Cmd+Space, configurable)
- [ ] Hotkey opens floating search panel over current workspace (must be visible and focused)
- [ ] Search panel is rendered with text input field, result list, and status indicator
- [ ] Typing in search panel does not block UI (async indexing or non-blocking search)
- [ ] Search panel closes on Esc key without error
- [ ] Search panel closes on loss of focus (clicking outside panel)
- [ ] Search panel can be reopened immediately after closing

#### Search Results & Performance
- [ ] Typing in search panel returns metadata-based (name/path) results within 200ms (P95 latency)
- [ ] Full-text content search returns results within 500ms (P95 latency) for queries matching indexed content
- [ ] Results are listed in order of relevance (verified by manual inspection of 5 representative queries)
- [ ] Result list updates incrementally as user types (not batch-refreshed)

#### Result Navigation & Actions
- [ ] Results are navigable via keyboard: up/down arrows move selection
- [ ] Enter key on selected result opens the file in its default application
- [ ] Cmd+R (configurable) reveals selected file in Finder
- [ ] Cmd+Shift+C (configurable) copies selected file's absolute path to clipboard
- [ ] Open, Reveal in Finder, and Copy Path actions execute without error
- [ ] Verify opened files appear in Finder/default app

#### Status & Indexing State
- [ ] Status bar icon is visible (macOS menu bar) at all times
- [ ] Status icon shows state: idle (blue), indexing (orange/animated), error (red)
- [ ] Hovering over status icon shows tooltip with brief state description
- [ ] Clicking status icon opens Index Health view (basic version showing root folders and progress)

#### Initial Indexing
- [ ] Initial indexing of user home directory completes without UI crash
- [ ] User can search before initial indexing completes (with partial results)
- [ ] Indexing progress is visible in status bar or onboarding UI
- [ ] No data loss if app is force-quit during initial indexing (resumable on next launch)

#### Incremental Updates (File System Monitoring)
- [ ] FSEvents integration is active and listening to home directory
- [ ] Creating a new file makes it searchable within 5 seconds
- [ ] Modifying file content updates FTS5 index within 5 seconds
- [ ] Deleting a file removes it from search results within 5 seconds
- [ ] Moving a file updates its path in the index within 5 seconds
- [ ] Renaming a file updates its name in the index within 5 seconds

#### Exclusions & Privacy
- [ ] .bsignore file is read from home directory root (~/.bsignore)
- [ ] Exclusion patterns from .bsignore are respected (excluded files do not appear in search results)
- [ ] Default .bsignore ships with 30+ patterns covering common exclusions:
  - Node modules: `node_modules/`, `npm-packages/`
  - Git: `.git/objects/`, `.git/logs/`, `.git/refs/`
  - Build artifacts: `build/`, `dist/`, `*.o`, `*.pyc`
  - Caches: `.cache/`, `cache/`, `__pycache__/`
  - Credential files: `.env`, `.env.local`
  - OS: `.DS_Store`, `Thumbs.db`
- [ ] Sensitive paths are excluded from content extraction by default:
  - `.ssh/` (SSH keys)
  - `.gnupg/` (GPG keys)
  - `.aws/` (AWS credentials)
  - `Library/Preferences/` (app plist files, not indexed)
- [ ] .bsignore changes are picked up on next app launch (no restart required in M1, required in M3)

#### Content Extraction
- [ ] PDF text extraction works for non-encrypted, non-scanned PDFs
- [ ] Plain text files (.txt, .md, .rst, .org) are fully content-indexed
- [ ] Markdown files (.md) are content-indexed (plain text mode, not parsed)
- [ ] Source code files (.py, .js, .ts, .cpp, .c, .java, .go, .rs, .rb, .sh) are content-indexed
- [ ] JSON, YAML, and config files are content-indexed as plain text
- [ ] Files with unsupported formats are gracefully skipped (logged, not indexed)
- [ ] Extractable file types are configurable via settings (M2+)

#### Error Handling & Robustness
- [ ] App handles unreadable files gracefully (logs failure, continues indexing)
- [ ] App handles permission errors (logs, skips file, continues)
- [ ] Extractor process crashes do not crash the main UI (process isolation)
- [ ] Corrupt index entries do not cause search crashes (graceful recovery)
- [ ] Out-of-disk-space condition is detected and logged (user warned in M2+)

#### FTS5 Index Verification
- [ ] FTS5 virtual table is created and populated (verified via SQLite debug tool or test)
- [ ] FTS5 queries return results for indexed content (verified by searching for known content)
- [ ] FTS5 index is updated on file modifications (verified by creating/modifying a file and searching)
- [ ] FTS5 index persists across app restarts (verified by restart and search)

### Performance Acceptance Criteria

#### Indexing Performance (against standard fixture: 500K files, 50GB content)
- [ ] Initial metadata scan: < 10 minutes (file enumeration, no content extraction)
- [ ] Full content extraction (text + PDF): < 60 minutes
- [ ] Incremental update (1 file): < 2 seconds end-to-end

#### Search Latency
- [ ] Metadata search (name/path match) P50: < 50ms
- [ ] Metadata search (name/path match) P95: < 200ms
- [ ] Full-text search P50: < 150ms
- [ ] Full-text search P95: < 500ms
- [ ] Latency measured from keystroke to first result displayed

#### Resource Usage (idle and active indexing)
- [ ] Indexer memory (RSS) during active indexing: < 200MB
- [ ] Indexer CPU during active indexing: throttled to < 50% of one core when user is active
- [ ] Indexer CPU during idle: near zero (< 1%)
- [ ] App (UI) memory at rest: < 80MB (excludes index cache)
- [ ] No memory growth over 30-minute idle session (< 2MB drift)

#### Storage
- [ ] SQLite database size for 500K files (30GB content): < 2GB
- [ ] FTS5 index size: < 1GB (subset of database)

#### Battery & System Impact
- [ ] System "Energy Impact" in Activity Monitor shows "Low" during idle indexing
- [ ] No measurable CPU wake on idle (verified via power trace)
- [ ] Thermal impact minimal (no fan spin during idle indexing)

### Testing Acceptance Criteria

#### Unit Tests
- [ ] PathRules module: exclusion patterns, path classification (code vs. docs vs. media), cloud detection
  - Test coverage: > 80% of code
  - All edge cases: symlinks, relative paths, unicode filenames
- [ ] Scoring module: exact match boost, partial match scoring, type penalties
  - Verify ordering of results for known queries
- [ ] TextChunking module: sentence boundary detection, stable chunk IDs, overlap handling
  - Verify consistency (same file → same chunks across runs)
- [ ] Extractor modules (PDF, text, code): handle corrupt/truncated inputs without crash
  - Malformed PDFs, truncated UTF-8, extremely large files (> 500MB)

#### Integration Tests
- [ ] **Create → Detect → Extract → Index → Search:** File created on disk → FSEvents fires → extractor runs → FTS5 updated → searchable within 5 seconds
- [ ] **Delete → Removed from Index:** File deleted → FSEvents fires → FTS5 entry removed → not in search results within 5 seconds
- [ ] **.bsignore enforcement:** Add exclude rule → file stops appearing in results on next search (no rescan needed for M1)
- [ ] **Extractor crash isolation:** Feed malformed PDF or corrupt file → extractor crashes → UI stays responsive, index remains valid, other files continue indexing
- [ ] **Index persistence:** Create index → restart app → same files are searchable (no re-indexing)

#### Manual Testing
- [ ] Full Disk Access flow: user grants permission, indexing proceeds without sandbox errors
- [ ] Hotkey registration works (verified by opening search panel from various apps)
- [ ] Search works with special characters (emojis, accented chars, CJK)

#### CI Integration
- [ ] All unit and integration tests pass in CI
- [ ] CI builds app and verifies no linking errors
- [ ] CI runs on macOS (Intel and Apple Silicon) with test fixture

### Definition of Done for M1

**All** of the above checkboxes must be checked. If any checkbox fails:
1. Root cause is documented
2. A fix or workaround is implemented
3. The checkbox is re-verified

**Exit criteria are NOT met** if:
- UI crashes during any test
- Search results are non-deterministic or incorrect
- Indexing corrupts the database
- More than 5% of files fail to index without user error

---

## MILESTONE 2: ML INTEGRATION + SEMANTIC SEARCH + 80% RELIABILITY

**Goal:** Semantic search works. Core features are reliable for daily use. 80%+ of search queries return the expected result in the top 3 results.

### Functional Acceptance Criteria (incremental on M1)

#### Semantic Search & ML Integration
- [ ] ONNX Runtime (or libtorch) is integrated and loads on app startup
- [ ] Embedding model (e.g., BGE-small-en-v1.5 or E5-small-v2, 384-dim) is bundled or downloaded on first run
- [ ] Model file is < 50MB (verified by size check on bundle or download)
- [ ] Embedding computation runs in background thread, does not block UI
- [ ] Embedding computation is lower priority than FTS5 indexing (CPU throttling applied)
- [ ] Semantic index (hnswlib, 384-dim int8 quantized, first-chunk-only) is initialized and populated
- [ ] Semantic search works: queries without exact term matches still find conceptually related files
  - Example: query "config" finds files named `settings.ini`, `setup.json` (not containing "config")
- [ ] Semantic and lexical results are merged using weighted scoring (lexical weight ≥ 0.4, semantic ≥ 0.3)
- [ ] If embedding model is unavailable or fails to load, search falls back to FTS5-only gracefully (no crash)
- [ ] If embedding computation fails mid-query, results are returned with lexical-only results (no crash)

#### Frequency & Context Boosting
- [ ] File open events are logged (via FSEvents or direct tracking)
- [ ] Frequency boosting: files the user opens frequently rank higher in results
  - Verification: open a file 10 times, search for it, verify it appears in top 3 even with partial name
- [ ] Recency boosting: recently opened/modified files rank higher
  - Verification: create a new file, open it, search for related term, verify it ranks high
- [ ] App-context boosting is implemented: if IDE (VS Code, Xcode) is frontmost, code files rank higher
  - Verification: switch to IDE, search for file, verify code files ranked above other types

#### Onboarding & Settings
- [ ] First-run flow guides user through:
  1. Full Disk Access permission request (with explanation)
  2. Home directory classification (via Home Map UI)
  3. Initial indexing progress display
- [ ] Home Map UI shows top-level home folders with recommended indexing modes (Index, Index+Embed, Skip)
- [ ] User can customize Home Map before first indexing starts
- [ ] Settings UI is accessible via status bar menu or app menu
- [ ] Settings UI includes tabs: General, Indexing, Exclusions, Privacy, Indexing Health
- [ ] General tab: hotkey, launch at login, theme toggle, language
- [ ] Indexing tab: roots to index, extractor types (PDF, OCR), embedding model selection
- [ ] Exclusions tab: editable .bsignore with syntax help and validation
- [ ] Privacy tab: sensitive path list, masking options, telemetry toggle
- [ ] Indexing Health tab: root list, exclusion summary, queue size, failure list, last scan time (basic version)
- [ ] Settings changes take effect immediately (no restart required)
- [ ] Settings are persisted to disk (verified by app restart)

#### Cloud & Sync Integration
- [ ] Cloud sync folders (iCloud Drive, Dropbox, Google Drive, OneDrive) are detected and indexed by default
- [ ] Sync artifacts (.sync, .dropbox.attr, .conflict, etc.) are excluded by default
- [ ] Symlinks in cloud folders are followed (optional, configurable)

#### Privacy & Data Masking
- [ ] Sensitive-classified files show path and name in results, but content snippets are masked
- [ ] Masking is applied consistently across search results and previews

### Reliability Acceptance Criteria

#### Search Relevance (80% threshold)
- [ ] Test Corpus: 50 representative queries tested against standardized home directory fixture
  - **Corpus composition:**
    - 10 exact filename queries (e.g., "budget.xlsx")
    - 10 partial filename queries (e.g., "bud" → finds "budget.xlsx")
    - 15 content-based queries (e.g., "export" → finds files containing the word "export")
    - 10 conceptual/semantic queries (e.g., "settings" → finds "config.ini", "preferences.json")
    - 5 edge case queries (emoji, accents, CJK, special characters)
  - **Scoring:** For each query, if the expected file appears in top 3 results, query passes
  - **Pass criterion:** ≥ 40 / 50 queries pass (80%)
- [ ] Test Corpus is version-controlled and reproducible (fixture available in repo)
- [ ] Test corpus is documented with expected file for each query

#### Zero Unhandled Crashes
- [ ] 8-hour continuous use stress test: alternating rapid typing, result navigation, file actions
  - No UI crashes
  - No indexer crashes
  - No silent failures
- [ ] Test script measures and logs all crashes (if any) with stack trace

#### Indexer Resilience
- [ ] Indexer crash recovery: if indexer subprocess crashes, it restarts within 10 seconds
  - Verification: kill indexer process, measure time to resume indexing, verify status bar reflects recovery
- [ ] Extractor crash does not produce corrupt index entries
  - Verification: force extractor to crash on a file, verify FTS5/vector index is valid afterward
- [ ] No data loss on dirty shutdown: kill -9 the app during indexing, verify on restart that:
  - Index is consistent (no corruption detected by SQLite integrity check)
  - Indexing resumes from last checkpoint (no re-indexing of previously completed files)

#### Database Integrity
- [ ] SQLite WAL recovery works: app shutdown during transaction → on restart, transaction is rolled back safely
  - Verification: abort app mid-transaction, restart, run PRAGMA integrity_check
- [ ] No duplicate entries in FTS5 index (verified by unique constraint checks in tests)

### Performance Acceptance Criteria (tightened from M1)

#### Search Latency (with semantic queries)
- [ ] Combined lexical + semantic search P50: < 80ms
- [ ] Combined lexical + semantic search P95: < 300ms
- [ ] Latency degradation vs. M1: < 50% increase due to embedding computation

#### Embedding Performance
- [ ] Embedding computation per chunk: < 50ms on Apple Silicon (M1/M2/M3)
- [ ] Batch embedding (100 chunks): < 3 seconds
- [ ] Embedding memory overhead: < 100MB per concurrent query

#### Vector Index
- [ ] VectorIndex memory (500K files): < 500MB
- [ ] Vector index is memory-mapped or lazy-loaded (not fully in-memory)
- [ ] Vector search latency: < 100ms for nearest-neighbor queries

#### Index Size Growth
- [ ] Semantic metadata adds < 20% to total database size
- [ ] Database size for 500K files: < 2.5GB (up from 2GB in M1, due to vector index)

### Testing Acceptance Criteria (incremental on M1)

#### Unit Tests (M1 + new)
- [ ] Scoring merger: lexical + semantic scores combine correctly, weights respected
- [ ] Embedding inference: input text → embedding vector, deterministic (same input → same output)
- [ ] Boost calculation: frequency, recency, app-context boosts apply correctly
- [ ] Fallback: embedding fails → search returns FTS5-only results without crash

#### Integration Tests (M1 + new)
- [ ] **Semantic Search:** query "configuration" → finds files named "settings.ini", "preferences.json" (no exact match)
- [ ] **Relevance:** run Test Corpus, measure pass rate, verify ≥ 80%
- [ ] **Boost verification:** open a file 5 times → search for it → appears in top 3 even with low relevance
- [ ] **Context boost:** switch to IDE → search for code file → ranks higher than non-code files
- [ ] **Embedding recovery:** disable model → search works (FTS5-only) → re-enable model → semantic works again

#### Stress Test
- [ ] 8-hour continuous use test (script): random queries, result navigation, file opens, with logging
- [ ] Output: crash log, resource usage graph, search latency histogram

### Definition of Done for M2

**All** of the above checkboxes must be checked, plus:

- Search relevance test: ≥ 40 / 50 queries pass (80%)
- Zero unhandled crashes in 8-hour stress test
- All unit and integration tests pass in CI

**Exit criteria are NOT met** if:
- Semantic search does not improve over FTS5 (verify with A/B comparison on Test Corpus)
- Relevance score < 75%
- App crashes more than once in stress test
- Embedding model fails to load on clean macOS system

---

## MILESTONE 3: POLISHED APP + 95% RELIABILITY

**Goal:** The app is daily-driver ready for power users. Edge cases are handled. Error states are informative. Reliability is 95%+.

### Functional Acceptance Criteria (incremental on M2)

#### Index Health Dashboard (expanded)
- [ ] Index Health view accessible from status bar or settings
- [ ] Dashboard shows:
  - [ ] List of indexed roots (with checkboxes to enable/disable indexing per root)
  - [ ] Summary of exclusions (count, and expandable list)
  - [ ] Indexing queue size and current activity
  - [ ] List of files that failed to index (with error reason, clickable to show more details)
  - [ ] Last indexing scan time and duration
  - [ ] Estimated database size and file count
  - [ ] Memory usage of app and indexer processes
- [ ] "Reindex folder" button: user selects a folder, it is re-indexed immediately
  - Verification: reindex a folder, verify modified files are updated
- [ ] "Rebuild all" button: clears all indices and starts fresh, with user confirmation dialog
  - Verification: user confirms, indices are cleared, re-indexing begins, no data loss

#### Error States & User Messaging
- [ ] All error messages are user-facing, not raw error strings
  - Examples:
    - Bad: "errno 13: Permission denied"
    - Good: "Cannot access /path/to/file (permission denied). Grant Full Disk Access to include this folder."
- [ ] Error states have actionable recovery paths:
  - FDA revoked: message suggests "Go to System Settings > Privacy & Security > Full Disk Access"
  - Disk full: message suggests "Free up space or exclude large folders"
  - Index corruption: message suggests "Rebuild all indices"
- [ ] Warnings are shown for non-fatal issues (e.g., some files skipped due to permission)
  - Visible in Index Health dashboard, not in primary UI

#### Result Display & Actions
- [ ] Preview masking: sensitive-classified files show path only, no content snippet in preview
- [ ] Results show:
  - [ ] File name (bold)
  - [ ] Parent folder path (dimmed)
  - [ ] File type icon (doc, code, image, etc.)
  - [ ] Last modified date (relative: "2 hours ago", "yesterday", "3 weeks ago")
  - [ ] File size (for large files: "12.4 MB")
  - [ ] Match context snippet (for full-text hits)
- [ ] Result actions (right-click or keyboard shortcuts):
  - [ ] Open (default action, Enter key)
  - [ ] Reveal in Finder (Cmd+R)
  - [ ] Copy absolute path (Cmd+Shift+C)
  - [ ] Copy relative path (Cmd+Shift+Alt+C)
  - [ ] Open with specific app (submenu of recent apps)
  - [ ] Add to Favorites (star icon, optional)
- [ ] All actions execute without error and produce expected result

#### Result Grouping & Organization
- [ ] Results are visually grouped by type:
  - [ ] Files (grouped by recency or relevance)
  - [ ] Folders (grouped by relevance)
  - [ ] Recently opened items (separate section, if applicable)
- [ ] Groups are collapsible/expandable (optional, but UI shows separation)
- [ ] Grouping is consistent across queries

#### Hotkey & Conflict Detection
- [ ] Hotkey conflict detection: app checks if default hotkey (Cmd+Space) is bound to another app
  - If conflict detected, user is prompted to choose an alternative
  - Verification: set up conflicting hotkey, launch app, verify detection and prompt
- [ ] Hotkey can be reconfigured in Settings without error
- [ ] Custom hotkey persists across app restart

#### .bsignore Updates (No Restart)
- [ ] Changes to ~/.bsignore file are picked up on next search (no app restart required)
  - Verification: edit .bsignore, search, verify exclusion is applied
- [ ] Changes via Settings UI (Exclusions tab) are applied immediately
  - Verification: add/remove rule in UI, search, verify change

#### Update Checker
- [ ] App checks for updates on startup (optional, can be disabled in settings)
- [ ] Update check uses Sparkle or similar framework
- [ ] Notification: user is prompted to update, not forced
- [ ] Update process does not interrupt indexing (runs in background)

#### Onboarding (Polished)
- [ ] First-run experience is smooth and < 5 minutes:
  1. Welcome screen (1 screen)
  2. Full Disk Access permission (OS prompt)
  3. Home Map customization (1-2 screens)
  4. Initial indexing progress (background, with visual feedback)
  5. Search ready (user can search before indexing is 100% complete)
- [ ] Tooltips and inline help are present throughout onboarding
- [ ] User can skip optional steps or proceed without Full Disk Access (with warning)

#### Clipboard & Context Boosting (Speculative, validate before committing)
- [ ] App monitors clipboard (with privacy consideration: only count, don't read content)
- [ ] Files matching clipboard content are boosted in relevance
  - Example: copy a file path → search for related term → that file ranks high
- [ ] Clipboard monitoring is optional and can be disabled in Privacy settings
- [ ] Clipboard data is not stored or logged (temporary, per-session only)

### Reliability Acceptance Criteria

#### Search Relevance (95% threshold)
- [ ] Test Corpus: expanded to 100 representative queries
  - Composition:
    - 20 exact filename queries
    - 20 partial filename queries
    - 25 content-based queries
    - 20 conceptual/semantic queries
    - 15 edge case queries (emoji, accents, CJK, special characters)
  - **Pass criterion:** ≥ 95 / 100 queries pass
- [ ] Test corpus results are manually reviewed by team (not 100% algorithmic)
  - Each failing query is analyzed and either:
    - Marked as "expected failure" (query is ambiguous or fixture lacks context), or
    - Indicates a bug to fix
- [ ] Relevance score is reproducible and tracked over time (in CI dashboard)

#### Zero Crashes in Extended Stress Test
- [ ] 48-hour continuous use stress test:
  - Rapid typing alternating with idle periods
  - File system mutations (create, modify, delete, move)
  - Hotkey spamming (open/close search panel 100x)
  - Result navigation and file open actions
  - Settings changes during indexing
  - Manual rebuild during search
- [ ] Expected outcome: zero crashes, no silent failures, no index corruption
- [ ] Test results logged and available in CI

#### Graceful Degradation
- [ ] Disk pressure (< 1GB free):
  - Indexing pauses (not crashes)
  - User is notified
  - Search still works with existing index
  - Recovery: user frees space or excludes folders → indexing resumes
- [ ] Low memory (< 256MB available):
  - Indexing throttles or pauses
  - Search latency may increase, but no crash
  - Recovers when memory available
- [ ] Network outage (if cloud folders are indexed):
  - Indexing of cloud folders pauses gracefully
  - Local indexing continues
  - No index corruption or data loss

#### Permission Changes
- [ ] Full Disk Access is revoked:
  - App detects revocation on next indexing attempt
  - User is shown actionable error message
  - Indexing stops, but app does not crash
  - Recovery path: user re-grants FDA in System Settings
  - Verification: revoke FDA, launch app, verify behavior
- [ ] Folder permissions change (read-only, no access):
  - Affected files are skipped with logged reason
  - Indexing continues
  - User sees warning in Index Health (file count may decrease)

#### Volume Changes
- [ ] Volume mount: new volume appears, user can add to indexed roots via settings
  - Indexing of new volume starts without crash
- [ ] Volume unmount: missing volume is detected, removed from roots, no crash
  - Remaining roots continue indexing
  - User is notified in Index Health
- [ ] Symlink targets move: symlinks are followed until invalid, then skipped gracefully

### Performance Acceptance Criteria

#### Startup & Responsiveness
- [ ] Cold start (after initial indexing is complete): search panel opens in < 3 seconds
- [ ] Hotkey response time (press hotkey to panel visible): < 500ms
- [ ] Search latency P99 (all query types): < 500ms

#### Memory Stability
- [ ] Memory leak test: run app idle for 24 hours
  - RSS growth: < 10MB (within noise threshold)
  - No gradual increase in peak memory
- [ ] Memory leak test during continuous indexing: 24 hours
  - RSS growth: < 50MB
  - No unbounded growth

#### App Distribution
- [ ] DMG size: < 100MB (includes app, model, runtime, notarization)
- [ ] App bundle size: < 50MB (excluding model, which is separate or downloaded)
- [ ] Model file: < 50MB (if bundled) or download on first run

### Testing Acceptance Criteria (incremental on M2)

#### Unit Tests (M1 + M2 + new)
- [ ] Error message formatting: raw errors → user-friendly messages (test 20+ error cases)
- [ ] Settings validation: invalid hotkey, invalid paths, invalid exclusions (handled gracefully)
- [ ] Preview masking: sensitive files → masked output

#### Integration Tests (M1 + M2 + new)
- [ ] **Relevance (expanded):** run 100-query Test Corpus, measure pass rate, verify ≥ 95%
- [ ] **Disk pressure:** fill disk to < 1GB free, verify indexing pauses and recovers
- [ ] **Permission change:** revoke FDA, verify graceful handling and recovery
- [ ] **Volume mount/unmount:** add/remove volume, verify no crash and correct behavior
- [ ] **Settings persistence:** change all settings, restart app, verify persistence
- [ ] **Hotkey conflict:** set hotkey to existing app hotkey, verify conflict detection

#### Manual Testing
- [ ] Full flow: install → grant FDA → customize Home Map → index → search → open file → change settings → search again
  - Verify smooth experience, no crashes, expected behavior at each step
- [ ] Error handling: trigger common errors (disk full, permission denied, invalid path) and verify user-friendly messages
- [ ] 24-hour soak test: background indexing, periodic searches, manual file operations
  - Verify no crashes, memory stable, performance consistent

#### Stress Test (Extended)
- [ ] 48-hour continuous use test (script): see Reliability criteria above
- [ ] Output: crash log, memory graph, search latency histogram, index integrity log

### Definition of Done for M3

**All** of the above checkboxes must be checked, plus:

- Search relevance test: ≥ 95 / 100 queries pass (95%)
- Zero crashes in 48-hour stress test
- Memory leak test: < 10MB drift over 24 hours idle
- All unit and integration tests pass in CI
- Manual testing of full user flow: no blockers
- App is notarized and ready for distribution

**Exit criteria are NOT met** if:
- Relevance score < 90%
- Any crash occurs in stress test
- Error messages are not user-friendly or actionable
- Settings are not persistent across restart
- DMG size > 120MB

---

## How to Verify These Criteria

This section describes the tools, scripts, and processes used to verify each category of acceptance criteria.

### Performance Verification

#### Built-in Benchmarks
- **Location:** `/tests/benchmarks/` (to be created)
- **Scripts:**
  - `benchmark_indexing.sh`: indexes a standard fixture (500K files, 50GB), measures time and memory
    - Output: CSV with metrics (total time, peak RSS, avg CPU %)
    - Fixture location: `/tests/fixtures/large_home/` (version-controlled, ~500K files)
    - Run command: `./benchmark_indexing.sh /path/to/fixture 2>&1 | tee results.log`
  - `benchmark_search.sh`: runs 100 representative queries, measures latency (P50, P95, P99)
    - Output: CSV with query name, latency, result count
    - Run command: `./benchmark_search.sh --queries /tests/fixtures/queries.csv --iterations 10`
  - `benchmark_memory.sh`: monitors memory (RSS, VSZ) over 24-hour idle and 24-hour active indexing
    - Output: CSV with timestamp, RSS, VSZ, CPU %
    - Run command: `./benchmark_memory.sh --duration 24h --mode idle|active`

#### CI Integration
- Benchmarks run on:
  - macOS Intel (i7) and Apple Silicon (M1/M2) in CI environment
  - Results are logged and compared against baseline
  - CI fails if latency exceeds thresholds by > 20% or memory exceeds limits by > 10%
- Baseline metrics are stored in `/tests/benchmarks/baselines.json` and updated per milestone

#### Manual Measurement (if CI unavailable)
- Latency: use `time` command or built-in profiling (e.g., Instruments on macOS)
  - Example: `time ./search_queries.sh 100` → reports total time
  - Divide by query count to get average latency
- Memory: use Activity Monitor (macOS) or `ps` command
  - Example: `ps -o rss= -p <pid>` → reports RSS in KB
  - Sample every 10 seconds during indexing
- CPU: use Activity Monitor or `top` command
  - Example: `top -pid <pid> -l 1` → reports CPU %

### Reliability Verification

#### Crash Logging
- **Location:** `~/.betterspotlight/crashes/` (auto-created)
- **Format:** Each crash dumps:
  - Stack trace
  - Timestamp
  - Process (UI or indexer)
  - Reason (SEGV, unhandled exception, assertion, etc.)
- **Script:** `analyze_crashes.sh` parses crash logs and reports
  - Output: crash summary (count, types, trends)

#### Stress Test Scripts
- **Location:** `/tests/stress_tests/`
- **Scripts:**
  - `stress_8h.sh`: runs 8-hour M1/M2 stress test
    - Simulates: typing, result navigation, file operations, settings changes
    - Launches app in background, drives via CLI or scripted UI automation
    - Logs: every action, timestamp, any errors
    - Output: `stress_8h_results.log` (searchable for crashes)
    - Run command: `./stress_8h.sh --duration 8h --log-file results.log`
  - `stress_48h.sh`: runs 48-hour M3 stress test (similar to 8h, longer duration)
    - Run command: `./stress_48h.sh --duration 48h --log-file results.log`
  - `stress_disk_pressure.sh`: fills disk to < 1GB, measures app behavior
    - Run command: `./stress_disk_pressure.sh --target-free 500M`
  - `stress_permission_changes.sh`: revokes FDA, changes folder permissions, measures recovery
    - Run command: `./stress_permission_changes.sh`

#### Index Integrity
- **Script:** `check_index_integrity.sh`
  - Runs SQLite `PRAGMA integrity_check` on database
  - Verifies FTS5 index consistency (no duplicate entries, all files reachable)
  - Verifies vector index consistency (no orphaned embeddings)
  - Output: pass/fail summary and detailed report
  - Run command: `./check_index_integrity.sh ~/.betterspotlight/index.db`

### Relevance & Search Quality Verification

#### Test Corpus Definition
- **Location:** `/tests/fixtures/test_corpus.json`
- **Format:**
  ```json
  {
    "version": "1.0",
    "fixture_id": "standard_home_v1",
    "queries": [
      {
        "id": "q001",
        "query": "budget",
        "category": "exact_filename",
        "expected_file": "/Users/test/Documents/budget.xlsx",
        "expected_rank": 1,
        "rationale": "Exact match on filename"
      },
      {
        "id": "q002",
        "query": "export",
        "category": "content_search",
        "expected_file": "/Users/test/Projects/report_export.py",
        "expected_rank": 1,
        "rationale": "File contains 'export' function"
      },
      {
        "id": "q003",
        "query": "settings",
        "category": "semantic",
        "expected_file": "/Users/test/config.ini",
        "expected_rank": 3,
        "rationale": "Semantic match: settings ~ config"
      }
    ]
  }
  ```
- **Fixture:** Standardized home directory with 500 representative files
  - Location: `/tests/fixtures/standard_home_v1/`
  - Includes: code projects, documents, images, PDFs, configs, media
  - Version-controlled in Git (fixtures stored as TAR or ZIP for efficiency)

#### Scoring Methodology
- **Scoring script:** `score_relevance.sh`
  - Runs search for each query in Test Corpus
  - Extracts top 3 results
  - Checks if `expected_file` is in top 3
  - Counts pass/fail
  - Output: CSV with query, expected file, actual top 3, pass/fail, reason if fail
  - Run command: `./score_relevance.sh --corpus /tests/fixtures/test_corpus.json --output results.csv`
- **Pass criterion:**
  - M1/M2: "Pass" if expected file is in top 3 results
  - M3: Same as M2
- **Ambiguity resolution:**
  - If a query is inherently ambiguous (e.g., "a" → many matches), mark as "expected failure" and exclude from pass rate calculation
  - Document rationale in test_corpus.json under `"notes"` field

#### Manual Relevance Review (M3)
- **Process:**
  - Run relevance script on all 100 queries
  - Extract failures (expected file not in top 3)
  - Team reviews each failure:
    1. Is the expected file really the best match? (Is the query unambiguous?)
    2. If yes, why did the search fail? (Missing content, weak scoring, etc.)
    3. If no, mark as "expected failure"
  - Document findings and track as "technical debt" or "improvement opportunity"
- **Output:** Manual review spreadsheet with findings and resolutions

### Integration Test Verification

#### Test Harness
- **Location:** `/tests/integration/`
- **Framework:** Catch2 (C++ test framework, integrated with Qt)
- **Test structure:**
  ```cpp
  TEST_CASE("File created → indexed → searchable", "[integration]") {
    // Setup: create temp directory with test file
    // Action: create new file
    // Verify: FSEvents fires, file is extracted, searchable within 5 seconds
    // Cleanup: remove temp directory
  }
  ```
- **Run command:** `ctest --output-on-failure`
- **CI:** Tests run on every commit, CI blocks if any test fails

#### Manual Integration Testing Checklist
- **Location:** `/docs/manual_testing_checklist.md`
- **Sections:**
  - Installation & FDA flow
  - Search functionality (exact, partial, content, semantic)
  - File operations (open, reveal, copy path)
  - Settings & customization
  - Error handling
- **Process:** Tester checks off each item, notes any issues, attaches screenshots/video evidence
- **Evidence:** Screen recordings or screenshots stored in issue tracker (Linear/GitHub)

### CI/CD Pipeline

#### GitHub Actions Workflow
- **Trigger:** Every push to `main` or PR to `main`
- **Steps:**
  1. Checkout code
  2. Install Qt 6, dependencies (ONNX Runtime, etc.)
  3. Build app (CMake)
  4. Run unit tests (ctest)
  5. Run integration tests (ctest)
  6. Run benchmark suite (see Performance Verification)
  7. Check code coverage (> 70% required for M2+)
  8. Upload results to CI dashboard
  9. Fail build if any test fails or thresholds exceeded
- **Results:** Accessible at `https://github.com/orgs/betterspotlight/actions` (or equivalent)

#### Pre-Release Checklist
- **Location:** `/docs/pre_release_checklist.md`
- **Items:**
  - [ ] All acceptance criteria pass (or failures documented)
  - [ ] Manual testing completed (checklist signed off)
  - [ ] Stress tests run for required duration (8h or 48h)
  - [ ] No open critical bugs (marked in issue tracker)
  - [ ] Release notes drafted
  - [ ] DMG built and notarized
- **Approval:** Lead engineer signs off before release

---

## Appendix: Fixture & Test Data

### Standard Test Fixture (500K files, 50GB)
- **Location:** `/tests/fixtures/large_home/`
- **Composition:**
  - Code projects (20): Python, JavaScript, C++, Rust, Go, etc.
  - Documents (50): Word, PDF, Markdown, text files
  - Media (100): images (JPEG, PNG), videos (mov, mp4)
  - Config files (50): JSON, YAML, .env, .plist, INI
  - Build artifacts (100K): .o, .pyc, node_modules (excluded by default)
  - Caches (50K): .cache, __pycache__, etc. (excluded by default)
  - Symlinks (1K): some valid, some broken (for testing)
- **Maintenance:** Fixture is versioned in Git (compressed as `.tar.gz` due to size)
  - Re-generated annually or when patterns change significantly

### Test Corpus (100 queries)
- **Location:** `/tests/fixtures/test_corpus.json`
- **Maintained by:** Team lead, updated per milestone
- **Changes reviewed in:** pull request before merge

---

## Revision History

| Version | Date       | Author | Changes |
|---------|------------|--------|---------|
| 1.0     | 2026-02-06 | Team   | Initial document |

---

## Sign-Off

**Milestone 1 Acceptance:** [ ] Lead Engineer  [ ] Product Manager  [ ] Date: ____

**Milestone 2 Acceptance:** [ ] Lead Engineer  [ ] Product Manager  [ ] Date: ____

**Milestone 3 Acceptance:** [ ] Lead Engineer  [ ] Product Manager  [ ] Date: ____

