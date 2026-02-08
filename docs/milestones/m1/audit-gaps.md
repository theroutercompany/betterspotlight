# M1 Audit Gap Report — 2026-02-06

Generated from comprehensive doc-by-doc audit of C++ codebase against all specification documents.

## Status: ALL 19 GAPS CLOSED — M1 implementation complete ✓

**Last updated:** 2026-02-07 (post-implementation verification)
**Build:** ✓ passes (12/12 tests)

---

## CRITICAL (blocks M1 from working)

### Gap 1: Pipeline scan-before-process — ✅ ALREADY FIXED (pre-audit)
- **Status:** Pipeline already runs scan and processing concurrently via separate threads.
- **Verified:** `pipeline.cpp` starts processing thread before scan.

## HIGH (spec violations affecting correctness)

### Gap 2: FSEvents debounce timer missing — ✅ ALREADY FIXED (pre-audit)
- **Status:** 500ms debounce implemented via dispatch_after in `file_monitor_macos.cpp:270`.
- **Verified:** `kDebounceMs = 500`, events buffered in `m_pendingEvents`, delivered via `flushPendingEvents()`.

### Gap 3: Extension format mismatch — ✅ ALREADY FIXED (pre-audit)
- **Status:** Extensions already include the dot prefix.
- **Verified:** Both `file_scanner.cpp` and `indexer.cpp` prepend ".".

### Gap 4: CWD proximity boost formula wrong — ✅ ALREADY FIXED (pre-audit)
- **Status:** Uses flat boost for depth <= maxDepth, not linear decay.
- **Verified:** `context_signals.cpp` returns full `cwdBoostWeight` when depth <= threshold.

### Gap 5: IPC response field names diverge from spec — ✅ ALREADY FIXED (pre-audit)
- **Status:** All three service files use spec-compliant field names.
- **Verified:** Response objects match doc 05 field names.

### Gap 6: IPC outbound notifications missing — ✅ ALREADY FIXED (pre-audit)
- **Status:** `SocketServer::broadcast()` exists. Pipeline signals wired to IndexerService IPC broadcasts.
- **Verified:** indexingProgress, indexingComplete, indexingError notifications implemented.

### Gap 7: Feedback table never populated — ✅ ALREADY FIXED (pre-audit)
- **Status:** `recordFeedback(itemId, action, query, position)` exists in SQLiteStore. QueryService calls it.
- **Verified:** Feedback INSERT into feedback table confirmed.

### Gap 8: Scoring weights not in settings table — ✅ ALREADY FIXED (pre-audit)
- **Status:** 16 INSERT OR IGNORE statements present in `schema.h` kDefaultSettings.
- **Verified:** All scoring weights have default rows.

### Gap 9: IPC responses include non-spec "type" field — ✅ CLOSED (documented)
- **Decision:** KEEP. The "type" field aids client-side dispatch. Spec doesn't prohibit extra fields.
- **Documented in:** `docs/decisions/ipc-deviations.md`

## MEDIUM (missing features from spec)

### Gap 10: Missing getFrequency method in QueryService — ✅ ALREADY FIXED (pre-audit)
- **Status:** `handleGetFrequency` exists, queries frequencies table, returns open_count, last_opened_at, total_interactions.

### Gap 11: Missing service "ready" signal on startup — ✅ ALREADY FIXED (pre-audit)
- **Status:** ServiceBase emits ready notification after socket bind/listen.

### Gap 12: Missing PID files — ✅ ALREADY FIXED (pre-audit)
- **Status:** Supervisor writes PID to `/tmp/betterspotlight-{uid}/{service}.pid`, cleans up on stop.

### Gap 13: cancelExtraction returns error stub — ✅ FIXED (this session)
- **Fix applied:** Atomic `m_cancelRequested` flag in ExtractionManager. `requestCancel()`/`clearCancel()`/`isCancelRequested()` API. Pre-semaphore check returns `Cancelled` status. ExtractorService handler calls through to ExtractionManager.
- **Files:** `extraction_manager.h`, `extraction_manager.cpp`, `extractor.h` (added `Cancelled` enum), `extractor_service.cpp`
- **Verified:** Build ✓, LSP clean

### Gap 14: Missing PRAGMA user_version = 1 — ✅ ALREADY FIXED (pre-audit)
- **Status:** `PRAGMA user_version = 1` present in kPragmas.

### Gap 15: Missing database file chmod(0600) — ✅ ALREADY FIXED (pre-audit)
- **Status:** `QFile::setPermissions(dbPath, ReadOwner | WriteOwner)` called after DB creation.

### Gap 16: Missing daily feedback aggregation job — ✅ ALREADY FIXED (pre-audit)
- **Status:** Maintenance method aggregates feedback → frequencies on startup.

### Gap 17: Missing 90-day feedback retention cleanup — ✅ ALREADY FIXED (pre-audit)
- **Status:** Cleanup query deletes feedback older than 90 days during maintenance.

### Gap 18: Missing PRAGMA integrity_check utility — ✅ ALREADY FIXED (pre-audit)
- **Status:** `integrityCheck()` method runs PRAGMA integrity_check and returns result.

### Gap 19: ADR-004 deviation (QLocalSocket vs raw Unix sockets) — ✅ CLOSED (documented)
- **Decision:** KEEP QLocalSocket. Uses Unix domain sockets internally on macOS. Signal/slot integration with Qt event loop is a net positive.
- **Documented in:** `docs/decisions/ipc-deviations.md`

---

## Implementation Summary (this session — 2026-02-07)

### Gaps fixed with new code (12 items):

| # | Gap | Key Files Modified |
|---|-----|--------------------|
| 2 (roadmap) | FTS5 query sanitization | `sqlite_store.cpp` (sanitizeFtsQuery), `sqlite_store.h` |
| 4 (roadmap) | Highlight range parsing | `query_service.cpp` (parseHighlights + snippet tag stripping) |
| 6 (roadmap) | FileScanner depth limit (kMaxDepth=64) | `file_scanner.h`, `file_scanner.cpp` |
| 10 (roadmap) | Important dotfile boost | `scorer.h`, `scorer.cpp` (isImportantDotfile, 15 dotfiles) |
| 9 (roadmap) | Text cleaning utility | NEW `text_cleaner.h/.cpp`, `extraction_manager.cpp`, `CMakeLists.txt` |
| 13 (audit) | cancelExtraction | `extraction_manager.h/.cpp`, `extractor.h`, `extractor_service.cpp` |
| 5+7 (roadmap) | FSEvents event ID persistence + error callback | `file_monitor.h`, `file_monitor_macos.h/.cpp` |
| 3 (roadmap) | FDA verification probe | NEW `fda_check.h/.cpp`, `CMakeLists.txt` |
| 8 (roadmap) | Settings persistence | NEW `settings_manager.h/.cpp`, `CMakeLists.txt` |
| 11 (roadmap) | GitHub Actions CI | NEW `.github/workflows/ci.yml` |
| 12 (roadmap) | Benchmark scripts (4 scripts) | NEW `tests/benchmarks/{benchmark_search,benchmark_indexing,benchmark_memory,stress_8h}.sh` |
| 9+19 (audit) | IPC deviations documentation | NEW `docs/decisions/ipc-deviations.md` |

### Gaps verified as already fixed (10 items):

Gaps 1-8, 10-12, 14-18 from this audit were confirmed already implemented in the codebase during the initial verification phase (pre-implementation).

### Build verification:
- `cmake --build build -j$(sysctl -n hw.ncpu)` — ✓ passes
- `ctest --test-dir build --output-on-failure` — 12/12 tests pass
