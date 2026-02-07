# IPC / Service Boundary Specification

**Document Status:** Implementation-Ready
**Last Updated:** February 2026
**Applies to:** BetterSpotlight M1+

## Overview

BetterSpotlight decomposes into three isolated service processes (Indexer, Extractor, Query) communicating via Unix domain sockets. This architecture provides:
- **Crash isolation:** Crash in Extractor doesn't kill entire app
- **Resource control:** CPU throttling per service
- **Testability:** Services can be tested independently
- **Robustness:** Automatic restart and recovery mechanisms

The main Qt UI app spawns and manages all service processes on launch.

---

## IPC Protocol Foundation

### Socket Configuration

```
Socket Path:        /tmp/betterspotlight-{uid}/{service}.sock
Service Names:      indexer, extractor, query
Example:            /tmp/betterspotlight-501/indexer.sock
```

### Message Format

All messages use a **length-prefixed JSON protocol**:

```
[4-byte BE uint32: payload length] + [JSON payload]
```

**Request:**
```json
{
  "id": 12345,
  "method": "startIndexing",
  "params": {
    "roots": ["/Users/alice/Documents", "/Users/alice/Desktop"]
  }
}
```

**Response (success):**
```json
{
  "id": 12345,
  "result": {
    "success": true,
    "queuedPaths": 1247
  }
}
```

**Response (error):**
```json
{
  "id": 12345,
  "error": {
    "code": 3,
    "message": "Permission denied for /Users/alice/Library"
  }
}
```

**Notification (one-way, no id):**
```json
{
  "method": "indexingProgress",
  "params": {
    "scanned": 5000,
    "total": 12000,
    "currentPath": "/Users/alice/Documents/Archive"
  }
}
```

### Protocol Rules

- **Request ID:** Unsigned 64-bit integer, must be unique per request (increment per connection)
- **No duplicate requests:** UI awaits response before issuing next request of same type
- **Notifications:** No response expected, fire-and-forget
- **Timeout:** UI expects response within 30 seconds; longer operations use progress notifications
- **Encoding:** UTF-8 for all strings

---

## IndexerService

### Responsibilities

- Monitors filesystem events via FSEvents API
- Manages the work queue of paths to index
- Coordinates with ExtractorService for content extraction
- Writes indexed data to SQLiteStore and FTS5 index
- Applies CPU throttling based on user activity
- Tracks indexing progress and errors

### Inbound Messages

#### `startIndexing(roots: [String])`

**Request:**
```json
{
  "id": 1,
  "method": "startIndexing",
  "params": {
    "roots": ["/Users/alice/Documents", "/Users/alice/Desktop"]
  }
}
```

**Response:**
```json
{
  "id": 1,
  "result": {
    "success": true,
    "queuedPaths": 2341,
    "timestamp": 1707225600
  }
}
```

**Behavior:**
- Scans provided root directories recursively
- Queues all discovered paths
- Begins processing immediately
- Returns after scan completes (can take seconds for large directories)

---

#### `pauseIndexing()`

**Request:**
```json
{
  "id": 2,
  "method": "pauseIndexing",
  "params": {}
}
```

**Response:**
```json
{
  "id": 2,
  "result": {
    "paused": true,
    "queuedPaths": 847
  }
}
```

**Behavior:**
- Stops processing work queue
- Allows in-flight extractions to complete
- Does not clear queue
- FSEvents monitoring continues in background

---

#### `resumeIndexing()`

**Request:**
```json
{
  "id": 3,
  "method": "resumeIndexing",
  "params": {}
}
```

**Response:**
```json
{
  "id": 3,
  "result": {
    "resumed": true,
    "queuedPaths": 847
  }
}
```

**Behavior:**
- Resumes processing from paused state
- Processes remaining queue
- No-op if not paused

---

#### `reindexPath(path: String)`

**Request:**
```json
{
  "id": 4,
  "method": "reindexPath",
  "params": {
    "path": "/Users/alice/Documents/Important"
  }
}
```

**Response:**
```json
{
  "id": 4,
  "result": {
    "queued": true,
    "deletedEntries": 42
  }
}
```

**Behavior:**
- Removes all index entries for path and its contents
- Re-scans and queues path
- Prioritized in work queue
- Useful for manual refresh or handling external file changes

---

#### `rebuildAll()`

**Request:**
```json
{
  "id": 5,
  "method": "rebuildAll",
  "params": {}
}
```

**Response:**
```json
{
  "id": 5,
  "result": {
    "cleared": true,
    "deletedEntries": 127543,
    "reindexingStarted": true
  }
}
```

**Behavior:**
- Clears entire SQLiteStore and FTS5 index
- Truncates frequencies and feedback tables
- Restarts initial scan of all configured roots
- Long-running operation; UI polls via `getQueueStatus()`

---

#### `getQueueStatus()`

**Request:**
```json
{
  "id": 6,
  "method": "getQueueStatus",
  "params": {}
}
```

**Response:**
```json
{
  "id": 6,
  "result": {
    "pending": 347,
    "processing": 1,
    "failed": 3,
    "paused": false,
    "lastProgressReport": {
      "scanned": 45230,
      "total": 123456,
      "currentPath": "/Users/alice/Documents/Work"
    }
  }
}
```

**Behavior:**
- Snapshots current queue state
- Non-blocking, fast
- Used for progress UI updates (poll every 500ms)

---

### Outbound Notifications

#### `indexingProgress`

```json
{
  "method": "indexingProgress",
  "params": {
    "scanned": 5000,
    "total": 12000,
    "currentPath": "/Users/alice/Documents/Archive",
    "timestamp": 1707225605
  }
}
```

**Frequency:** Every ~1000 files processed or every 2 seconds, whichever is sooner
**UI Use:** Update progress bar, ETA calculation

---

#### `indexingComplete`

```json
{
  "method": "indexingComplete",
  "params": {
    "stats": {
      "totalIndexed": 127543,
      "totalFailed": 8,
      "duration": 342,
      "averageTimePerFile": 2.67,
      "finalPathCount": 127543,
      "ftsEntriesCreated": 127543,
      "duplicatePathsSkipped": 0
    },
    "timestamp": 1707225942
  }
}
```

**Frequency:** Once, at end of initial index run
**UI Use:** Enable search, show indexing complete toast

---

#### `indexingError`

```json
{
  "method": "indexingError",
  "params": {
    "path": "/Users/alice/Protected",
    "error": "Permission denied",
    "code": 13,
    "timestamp": 1707225678
  }
}
```

**Frequency:** One per errored path
**UI Use:** Log to error panel, optionally notify user of systemic issues (e.g., many permission denied)

---

### Internal Behavior

**FSEvents Monitoring:**
- Watches all indexed root directories
- Debounces rapid changes (100ms window)
- Queues affected paths when changes detected
- Continues monitoring even when paused

**Work Queue:**
- FIFO for filesystem changes
- Prioritized entries for manual `reindexPath()` calls
- Files already in index skip extraction if unmodified
- Failed paths retry up to 3 times before permanent failure

**CPU Throttling:**
- Detects user activity via keyboard/mouse events
- When active: yield execution, reduce concurrency
- When idle (>30s no activity): increase concurrency up to 4 parallel extractions
- Sleeps 50ms between paths during user activity

**Critical Path: Writing to Index**
```
For each queued path:
  1. Check if already indexed and unmodified (via mtime)
     → Skip extraction if so
  2. Call ExtractorService.extractText()
  3. Await extraction result (timeout: 30s)
  4. Insert/update in items table
  5. Insert content chunks into search_index table
  6. Trigger FTS5 re-index (automatic)
  7. Update frequencies.last_indexed timestamp
```

---

## ExtractorService

### Responsibilities

- Extracts text and metadata from files
- Supports multiple file types (.pdf, .docx, .txt, .md, code files, etc.)
- Runs in isolated process for crash containment
- Stateless: no persistent state between requests
- Bounded resource usage (timeouts, memory limits)

### Inbound Messages

#### `extractText(path: String, kind: ItemKind)`

**Request:**
```json
{
  "id": 10,
  "method": "extractText",
  "params": {
    "path": "/Users/alice/Documents/Report.pdf",
    "kind": "pdf"
  }
}
```

**Response:**
```json
{
  "id": 10,
  "result": {
    "text": "Executive Summary: Q4 2025 Performance...",
    "chunks": [
      {
        "offset": 0,
        "length": 256,
        "text": "Executive Summary: Q4 2025 Performance metrics show strong growth in..."
      },
      {
        "offset": 256,
        "length": 248,
        "text": "...user acquisition increased by 23% month-over-month. Key drivers include..."
      }
    ],
    "metadata": {
      "pageCount": 12,
      "creationDate": "2025-10-15T08:30:00Z",
      "author": "Finance Team",
      "title": "Q4 2025 Performance Report",
      "language": "en"
    },
    "duration": 234
  }
}
```

**Behavior:**
- Extracts text from file at path
- Returns full extracted text; chunking is handled by IndexerService per pipeline spec (500-2000 chars, 1000 default, no overlap for FTS5; see [03-indexing-pipeline.md, Stage 6])
- Extracts metadata if available in file format
- Enforces 30-second timeout (IndexerService enforces)

**Supported Kinds:** `text`, `pdf`, `docx`, `xlsx`, `pptx`, `rtf`, `markdown`, `plaintext`, `code`

---

#### `extractMetadata(path: String)`

**Request:**
```json
{
  "id": 11,
  "method": "extractMetadata",
  "params": {
    "path": "/Users/alice/Photos/Vacation2025.jpg"
  }
}
```

**Response:**
```json
{
  "id": 11,
  "result": {
    "fileSize": 4194304,
    "creationDate": "2025-06-10T14:22:00Z",
    "modificationDate": "2025-06-10T14:22:00Z",
    "owner": "alice",
    "isExecutable": false,
    "isSymlink": false,
    "symlinkTarget": null,
    "imageWidth": 3840,
    "imageHeight": 2160,
    "imageColorSpace": "sRGB",
    "videoDuration": null,
    "duration": 12
  }
}
```

**Behavior:**
- Returns file-system and format-specific metadata
- Non-blocking, fast
- Used to populate metadata columns in database

---

#### `isSupported(extension: String)`

**Request:**
```json
{
  "id": 12,
  "method": "isSupported",
  "params": {
    "extension": "pdf"
  }
}
```

**Response:**
```json
{
  "id": 12,
  "result": {
    "supported": true,
    "kind": "pdf"
  }
}
```

**Behavior:**
- Checks if extractor can handle file type
- Returns kind if supported, false otherwise
- Used by Indexer to skip unsupported files

---

#### `cancelExtraction(path: String)`

**Request:**
```json
{
  "id": 13,
  "method": "cancelExtraction",
  "params": {
    "path": "/Users/alice/LargeVideo.mov"
  }
}
```

**Response:**
```json
{
  "id": 13,
  "result": {
    "cancelled": true
  }
}
```

**Behavior:**
- Attempts to cancel in-flight extraction
- No-op if extraction already completed
- May not succeed if already in critical section

---

### Stateless Design

**Key Principle:** ExtractorService holds no persistent state between requests. Each request is independent:
- No connection pooling
- No cached file handles
- No state accumulation
- Process can be killed and restarted without data loss (Indexer retries)

**Concurrency:** Single-threaded request processing; IndexerService can spawn multiple Extractor instances for parallel extraction (not in M1, possible M2 optimization).

### Crash Recovery

```
IndexerService flow:
  1. Call ExtractorService.extractText()
  2. No response within timeout → broken socket detected
  3. Log: "ExtractorService crash detected"
  4. Kill ExtractorService process (SIGKILL)
  5. Wait 1s
  6. Spawn new ExtractorService process
  7. Retry request up to 2 times
  8. If 2nd retry fails, mark path as failed
```

---

## QueryService

### Responsibilities

- Executes search queries against indexed data
- Applies multi-signal ranking (recency, frequency, context)
- Returns ranked results with snippets and highlights
- Tracks user feedback for learning
- Monitors index health

### Inbound Messages

#### `search(query: String, limit: Int, context: QueryContext)`

**Request:**
```json
{
  "id": 20,
  "method": "search",
  "params": {
    "query": "quarterly report",
    "limit": 20,
    "context": {
      "cwdPath": "/Users/alice/Documents",
      "frontmostAppBundleId": "com.apple.Terminal",
      "recentPaths": [
        "/Users/alice/Documents/Work",
        "/Users/alice/Desktop"
      ]
    }
  }
}
```

**Response:**
```json
{
  "id": 20,
  "result": {
    "results": [
      {
        "itemId": 4521,
        "path": "/Users/alice/Documents/2025-Q4-Report.pdf",
        "name": "2025-Q4-Report.pdf",
        "kind": "pdf",
        "matchType": "prefixNameMatch",
        "score": 287.5,
        "snippet": "...quarterly report on key metrics and...",
        "highlights": [
          {"offset": 23, "length": 9},
          {"offset": 145, "length": 6}
        ],
        "metadata": {
          "fileSize": 2097152,
          "modificationDate": "2025-12-20T10:15:00Z"
        },
        "isPinned": false,
        "frequency": {
          "openCount": 7,
          "lastOpenDate": "2025-12-22T09:30:00Z"
        }
      },
      {
        "itemId": 4689,
        "path": "/Users/alice/Documents/Archive/2024-Q3-Report.pdf",
        "name": "2024-Q3-Report.pdf",
        "kind": "pdf",
        "matchType": "containsNameMatch",
        "score": 156.3,
        "snippet": "...previous quarterly report covered...",
        "highlights": [
          {"offset": 18, "length": 9}
        ],
        "metadata": {
          "fileSize": 1572864,
          "modificationDate": "2024-10-05T14:22:00Z"
        },
        "isPinned": false,
        "frequency": {
          "openCount": 2,
          "lastOpenDate": "2024-12-10T16:45:00Z"
        }
      }
    ],
    "queryTime": 45,
    "totalMatches": 127
  }
}
```

**Behavior:**
- Parses query string (handles quoted phrases, wildcards)
- Executes FTS5 search
- Ranks results using scoring function (see Ranking & Scoring Specification)
- Returns top `limit` results (default 20)
- Includes snippets with highlight offsets for UI rendering

**QueryContext Fields:**
- `cwdPath` (optional): Current working directory; boosts files in/near this path
- `frontmostAppBundleId` (optional): e.g., `com.apple.Terminal`, `com.microsoft.VSCode`; used for app-context ranking
- `recentPaths` (optional): Recently accessed directories; mild boost for nearby files

---

#### `getHealth()`

**Request:**
```json
{
  "id": 21,
  "method": "getHealth",
  "params": {}
}
```

**Response:**
```json
{
  "id": 21,
  "result": {
    "indexHealth": {
      "isHealthy": true,
      "totalIndexedItems": 127543,
      "lastIndexTime": "2025-12-22T14:30:00Z",
      "indexAge": 3600,
      "ftsIndexSize": 145892352,
      "itemsWithoutContent": 0
    },
    "serviceHealth": {
      "indexerRunning": true,
      "extractorRunning": true,
      "queryServiceRunning": true,
      "uptime": 86400
    },
    "issues": [
      {
        "severity": "warning",
        "message": "ExtractorService restarted 1 time in last hour"
      }
    ]
  }
}
```

**Behavior:**
- Returns index and service status
- Detects orphaned items (no content)
- Reports service restarts and crashes
- Used by UI health dashboard

---

#### `recordFeedback(itemId: Int, action: String, query: String, position: Int)`

**Request:**
```json
{
  "id": 22,
  "method": "recordFeedback",
  "params": {
    "itemId": 4521,
    "action": "opened",
    "query": "quarterly report",
    "position": 0
  }
}
```

**Response:**
```json
{
  "id": 22,
  "result": {
    "recorded": true
  }
}
```

**Behavior:**
- Records user interaction (opened, previewed, ignored, etc.)
- Updates frequencies table (increments `open_count`)
- Stores in feedback table for future learning (M2+)
- Used to adjust ranking over time

**Action types:** `opened`, `previewed`, `ignored`, `deleted`, `archived`

---

#### `getFrequency(itemId: Int)`

**Request:**
```json
{
  "id": 23,
  "method": "getFrequency",
  "params": {
    "itemId": 4521
  }
}
```

**Response:**
```json
{
  "id": 23,
  "result": {
    "openCount": 7,
    "lastOpenDate": "2025-12-22T09:30:00Z",
    "frequencyTier": 1,
    "boost": 20
  }
}
```

**Behavior:**
- Returns frequency data for item
- Useful for UI tooltips showing "opened 7 times"
- Internal use only (scoring already includes boost)

---

### Reading Data Sources

QueryService reads from three data sources:

1. **SQLiteStore**: items, metadata, pinned status
2. **FTS5 Index**: search_index, BM25 scores
3. **VectorIndex** (M2 only): embeddings, semantic similarity

All reads are read-only; no writes except via feedback path.

---

## Lifecycle Management

### Startup Sequence

```
UI App Launch:
  1. Create /tmp/betterspotlight-{uid}/ directory
  2. Spawn IndexerService (read PID from stderr, write to /tmp/.../indexer.pid)
  3. Spawn ExtractorService
  4. Spawn QueryService
  5. Connect to all three sockets
  6. Send ping to each service
  7. If all healthy, enable search UI
  8. If any fails, surface health error, retry with exponential backoff
```

### Shutdown Sequence

```
UI App Exit:
  For each service [IndexerService, ExtractorService, QueryService]:
    1. Send shutdown message: { "method": "shutdown", "params": {} }
    2. Wait up to 5 seconds for graceful exit
    3. If not exited, send SIGTERM
    4. Wait 2 seconds
    5. If still not exited, send SIGKILL
    6. Remove socket file and PID file
  7. Cleanup /tmp/betterspotlight-{uid}/
```

### Crash Detection and Restart

**Heartbeat Mechanism:**
- UI sends `ping()` message every 10 seconds to each service
- Expects `pong()` response within 5 seconds
- If no pong, socket is marked broken

**Restart Logic:**
```
On broken socket detected:
  1. Log: "ServiceName crashed or disconnected"
  2. SIGKILL the service process
  3. Wait 1 second, then restart
  4. Track restart count per service

  Exponential backoff for repeated crashes:
    1st restart:  immediate
    2nd restart:  1 second delay
    3rd restart:  2 second delay
    4th restart:  4 second delay
    5th+ restart: 30 second delay (max)

  If service crashes 3+ times in 60 seconds:
    → Stop restarting
    → Surface error in UI health dashboard
    → User can trigger manual restart via Settings
```

**PID Files:**
```
/tmp/betterspotlight-{uid}/indexer.pid
/tmp/betterspotlight-{uid}/extractor.pid
/tmp/betterspotlight-{uid}/query.pid
```

Each file contains the process ID as plain text. Used to confirm process is still alive via `kill -0 $pid`.

---

## Error Codes

Standard error codes used across all services:

| Code | Name | Meaning |
|------|------|---------|
| 1 | INVALID_PARAMS | Request parameters malformed or missing required field |
| 2 | TIMEOUT | Operation exceeded timeout |
| 3 | PERMISSION_DENIED | No permission to access path or resource |
| 4 | NOT_FOUND | Path or resource does not exist |
| 5 | ALREADY_RUNNING | Operation already in progress (e.g., startIndexing when already running) |
| 6 | INTERNAL_ERROR | Unexpected error in service |
| 7 | UNSUPPORTED | File type or operation not supported |
| 8 | CORRUPTED_INDEX | Database or FTS5 index corrupted, rebuildAll recommended |
| 9 | SERVICE_UNAVAILABLE | Service crashed or unavailable |

---

## Resilience Patterns

### Timeout Handling

- **IndexerService methods:** 5s default (longer for rebuildAll)
- **ExtractorService methods:** 30s (enforced by IndexerService)
- **QueryService methods:** 10s (search can return partial results)

### Partial Failures

- **File extraction fails:** Path marked failed, logged, indexing continues
- **FTS5 index corruption:** Logged as error; rebuildAll can fix
- **Service crash during search:** Query times out, returns cached results or empty set

### Data Consistency

- All writes to database happen in transactions
- Incomplete writes are rolled back if service crashes
- FTS5 index rebuilt on startup if inconsistencies detected (slow first search)

---

## Diagrams

### Service Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                        Qt UI App                             │
│  (Main process, manages service lifecycle & IPC)             │
└────────┬──────────────────────┬──────────────────────┬──────┘
         │                      │                      │
    [socket]              [socket]               [socket]
         │                      │                      │
    ┌────▼─────┐          ┌─────▼────┐         ┌──────▼───┐
    │ Indexer  │          │ Extractor│         │  Query   │
    │ Service  │          │ Service  │         │ Service  │
    │          │          │          │         │          │
    │ FSEvents │          │ PDF/DOCX │         │ FTS5     │
    │ Work Qm. │◄────────►│ Extractor│         │ Ranking  │
    │ DB Write │          │          │         │ Feedback │
    └────┬─────┘          └──────────┘         └──────────┘
         │
    ┌────▼──────────────────────┐
    │ SQLiteStore + FTS5 Index   │
    │ (Shared read-only)         │
    └───────────────────────────┘
```

### Message Flow: Search

```
UI App sends search() query to QueryService:

┌──────────────┐              ┌────────────────┐
│  UI (Qt)     │─request─────►│ QueryService   │
│              │              │                │
│              │              │ 1. Parse query │
│              │              │ 2. Execute FTS5│
│              │              │ 3. Score items │
│              │              │ 4. Sort results│
│              │◄────result───│                │
└──────────────┘              └────────────────┘
                                     △
                                     │
                              SQLiteStore
                                (read)
```

### Message Flow: Indexing

```
┌──────────────┐
│ Filesystem   │
│   Events     │
└──────┬───────┘
       │
       ▼
┌───────────────────────┐
│ IndexerService        │
│ 1. Detect change      │
│ 2. Queue path         │
│ 3. Call Extractor     │─────┐
│ 4. Insert to DB       │     │
└───────────────────────┘     │
       △                       │
       │                       ▼
       │               ┌─────────────────┐
       └───────────────│ ExtractorService│
         (retry on     │                 │
          crash)       │ Timeout: 30s    │
                       └─────────────────┘

Result → SQLiteStore (items, search_index tables)
```

---

## Testing Considerations

### Service Isolation Tests

- **Mock socket failures:** Kill service during operation, verify restart
- **Timeout scenarios:** Simulate slow extraction, verify timeout handling
- **Crash during commit:** Verify database consistency on recovery

### Protocol Conformance

- **Malformed JSON:** Service should return error code 1
- **Unknown method:** Service should return error code 6
- **Missing required params:** Service should return error code 1

### Integration Tests

- **Full indexing cycle:** Verify items appear in search results
- **Feedback loop:** Open item, verify frequency boost applies to next search
- **Service restart:** Kill Extractor mid-extraction, verify retry succeeds

---

## Configuration

All services read configuration from `~/.config/betterspotlight/config.json` on startup. Changes to socket paths, timeouts, or retry policies require service restart.

Example:
```json
{
  "socketDir": "/tmp/betterspotlight-{uid}",
  "indexerTimeout": 5000,
  "extractorTimeout": 30000,
  "queryTimeout": 10000,
  "heartbeatInterval": 10000,
  "restartBackoffMs": [0, 1000, 2000, 4000, 30000]
}
```

---

**End of Document**
