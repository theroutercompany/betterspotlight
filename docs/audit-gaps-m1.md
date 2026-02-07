# M1 Audit Gap Report — 2026-02-06

Generated from comprehensive doc-by-doc audit of C++ codebase against all specification documents.

## Status: All M1 features are implemented (Waves 0-3 complete), but 19 gaps found.

---

## CRITICAL (blocks M1 from working)

### Gap 1: Pipeline scan-before-process
- **File:** `src/core/indexing/pipeline.cpp:163-189` and `pipeline.h`
- **Problem:** `workerEntry()` runs full directory scan BEFORE `processingLoop()` starts. Nothing gets indexed for minutes.
- **Spec:** Doc 11 M1: "User can search before initial indexing completes (with partial results)"
- **Fix:** Run scan and processing on two separate threads. Scan thread enqueues items; processing thread dequeues and processes concurrently.
- **Changes:** pipeline.h (add m_processThread), pipeline.cpp (start processing thread in workerEntry before scan, or start both in start())

## HIGH (spec violations affecting correctness)

### Gap 2: FSEvents debounce timer missing
- **File:** `src/core/fs/file_monitor_macos.cpp`
- **Problem:** Doc 03 Stage 1 requires 500ms debounce timer. Events fire immediately to callback.
- **Fix:** Add a QTimer-based debounce that batches events before delivering to callback. Accumulate events in a buffer, deliver on timer expiry.
- **Changes:** file_monitor_macos.h (add buffer + timer members), file_monitor_macos.cpp (buffer events, deliver on timer)

### Gap 3: Extension format mismatch
- **File:** `src/core/fs/file_scanner.cpp:93`, `src/core/indexing/indexer.cpp:241`
- **Problem:** Qt `suffix()` returns "txt" not ".txt". Spec says extensions include the dot.
- **Fix:** Prepend "." to extension in both locations (guard against empty suffix).
- **Changes:** file_scanner.cpp (~1 line), indexer.cpp (~1 line)

### Gap 4: CWD proximity boost formula wrong
- **File:** `src/core/ranking/context_signals.cpp:178-181`
- **Problem:** Uses linear decay `25 * (1.0 - depth/3.0)`. Spec says flat boost of 25 for depth <= 2.
- **Fix:** Replace linear decay with flat boost: if depth <= maxDepth, return full cwdBoostWeight.
- **Changes:** context_signals.cpp (~5 lines)

### Gap 5: IPC response field names diverge from spec
- **Files:** `src/services/indexer/indexer_service.cpp`, `src/services/extractor/extractor_service.cpp`, `src/services/query/query_service.cpp`
- **Problem:** Every service method returns different field names than doc 05 specifies.
- **Fix:** Update all response objects to match spec field names exactly.
- **Specifics:**
  - startIndexing: return `{success, queuedPaths, timestamp}` not `{status, roots}`
  - pauseIndexing: return `{paused, queuedPaths}` not `{status}`
  - resumeIndexing: return `{resumed, queuedPaths}` not `{status}`
  - reindexPath: return `{queued, deletedEntries}` not `{status, path}`
  - rebuildAll: return `{cleared, deletedEntries, reindexingStarted}` not `{status}`
  - getQueueStatus: add `lastProgressReport` field
  - extractText: return `{text, chunks, metadata, duration}` not `{content, status, durationMs}`
  - extractMetadata: add full metadata fields
  - isSupported: add `kind` field
  - search: add `highlights`, `frequency` fields
  - getHealth: add `serviceHealth` section
  - recordFeedback: return `{recorded: true}` not `{status: "recorded"}`

### Gap 6: IPC outbound notifications missing
- **File:** `src/core/ipc/service_base.cpp:105-113`, `src/core/ipc/socket_server.h`
- **Problem:** No broadcast mechanism. indexingProgress, indexingComplete, indexingError notifications not sent.
- **Fix:** Add broadcast capability to SocketServer (iterate connected clients, send notification). Wire Pipeline signals to IndexerService which broadcasts via IPC.
- **Changes:** socket_server.h/.cpp (add broadcast method), service_base.h/.cpp (add sendNotification), indexer_service.cpp (connect Pipeline signals to IPC broadcasts)

### Gap 7: Feedback table never populated
- **File:** `src/services/query/query_service.cpp:208-210`, `src/core/index/sqlite_store.cpp`
- **Problem:** recordFeedback in QueryService calls incrementFrequency() but discards action, query, position.
- **Fix:** Add `recordFeedback(itemId, action, query, position)` to SQLiteStore that INSERTs into feedback table. Update QueryService to call it.
- **Changes:** sqlite_store.h/.cpp (add recordFeedback method), query_service.cpp (call it)

### Gap 8: Scoring weights not in settings table
- **File:** `src/core/index/schema.h:135-142`
- **Problem:** 16 scoring weights have code defaults but no INSERT statements in schema.
- **Fix:** Add 16 INSERT OR IGNORE statements to kDefaultSettings for all weights.
- **Changes:** schema.h (~20 lines added)

### Gap 9: IPC responses include non-spec "type" field
- **File:** `src/core/ipc/message.cpp:88-109`
- **Problem:** makeResponse() adds `"type": "response"`, makeError() adds `"type": "error"`.
- **Decision:** KEEP this. The "type" field is a useful implementation detail for client-side dispatch. The spec doesn't prohibit extra fields. Document as intentional enhancement.
- **No code change needed — document deviation.**

## MEDIUM (missing features from spec)

### Gap 10: Missing getFrequency method in QueryService
- **File:** `src/services/query/query_service.cpp`
- **Fix:** Add handleGetFrequency that queries frequencies table and returns open_count, last_opened_at, total_interactions.

### Gap 11: Missing service "ready" signal on startup
- **File:** `src/core/ipc/service_base.cpp`
- **Fix:** After successful socket bind and listen, write a "ready" notification to stdout or send to supervisor.

### Gap 12: Missing PID files
- **File:** `src/core/ipc/supervisor.cpp`
- **Fix:** After starting each service process, write its PID to `/tmp/betterspotlight-{uid}/{service}.pid`. Clean up on stop.

### Gap 13: cancelExtraction returns error stub
- **File:** `src/services/extractor/extractor_service.cpp:174`
- **Fix:** Implement cancellation via ExtractionManager (set a cancel flag, check in extraction loop).

### Gap 14: Missing PRAGMA user_version = 1
- **File:** `src/core/index/schema.h`
- **Fix:** Add `"PRAGMA user_version = 1;\n"` to kPragmas string.

### Gap 15: Missing database file chmod(0600)
- **File:** `src/core/index/sqlite_store.cpp`
- **Fix:** After database creation, call `QFile::setPermissions(dbPath, QFile::ReadOwner | QFile::WriteOwner)`.

### Gap 16: Missing daily feedback aggregation job
- **Fix:** Add a maintenance method to SQLiteStore that aggregates feedback → frequencies. Call periodically or on app startup.

### Gap 17: Missing 90-day feedback retention cleanup
- **Fix:** Add cleanup query: `DELETE FROM feedback WHERE timestamp < datetime('now', '-90 days')`. Run during maintenance.

### Gap 18: Missing PRAGMA integrity_check utility
- **Fix:** Add `integrityCheck()` method to SQLiteStore that runs `PRAGMA integrity_check` and returns result.

### Gap 19: ADR-004 deviation (QLocalSocket vs raw Unix sockets)
- **Decision:** KEEP QLocalSocket. On macOS, QLocalSocket uses Unix domain sockets internally. The abstraction is acceptable and provides signal/slot integration with Qt event loop. Document as intentional deviation.
- **No code change needed — document deviation.**

---

## Implementation Priority Order

### Batch A (Pipeline + Core Fixes) — Parallel
1. Gap 1: Pipeline concurrent scan+process
2. Gap 2: FSEvents debounce timer
3. Gap 3: Extension format fix
4. Gap 4: CWD proximity formula fix

### Batch B (IPC + Schema Fixes) — Parallel
5. Gap 5: IPC response field alignment (all 3 services)
6. Gap 6: IPC outbound notifications
7. Gap 7: Feedback table population
8. Gap 8: Scoring weights in settings table
9. Gap 10: getFrequency method
10. Gap 14: PRAGMA user_version

### Batch C (Remaining Medium Items) — Parallel
11. Gap 11: Service ready signal
12. Gap 12: PID files
13. Gap 15: Database chmod(0600)
14. Gap 16+17: Feedback aggregation + cleanup
15. Gap 18: Integrity check utility

### Deferred (Document Only)
- Gap 9: Keep "type" field in responses (document)
- Gap 13: cancelExtraction (complex, low priority for M1)
- Gap 19: Keep QLocalSocket (document)
