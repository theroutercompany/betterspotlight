# BetterSpotlight M1/M2 Manual Inspection Guide

Last validated against repo state: 2026-02-08 (post-fixes)

This guide is based on:

- M1/M2 requirements in `docs/milestones/`
- The current implementation under `src/`
- Actual runtime behavior observed from the built binaries in `build/`

It is intentionally practical: each step has concrete commands, expected outcomes, and troubleshooting.

---

## 1. Scope and Ground Rules

1. This guide verifies both M1 and M2 behavior, but it distinguishes between:

- documented target behavior (spec)
- current implementation behavior (what the code/binaries do today)

2. Some M2 features are partially wired in code but not fully integrated into runtime flows. This guide calls those out explicitly.

3. The service/database paths used by the current binaries are:

- Socket directory: `/tmp/betterspotlight-$(id -u)/`
- Database: `$HOME/Library/Application Support/betterspotlight/index.db`
- App settings (runtime): `$HOME/Library/Application Support/betterspotlight/BetterSpotlight/settings.json`
- Additional settings file used by services/onboarding logic: `$HOME/Library/Application Support/betterspotlight/settings.json`

---

## 2. Preflight

From repo root:

```bash
cd /Users/rexliu/betterspotlight
```

Check toolchain:

```bash
xcode-select -p
cmake --version
ctest --version
python3 --version
sqlite3 --version
```

Optional dependency checks (affect PDF/OCR/semantic behavior):

```bash
pkg-config --modversion poppler-qt6 || echo "poppler-qt6 not detected"
tesseract --version || echo "tesseract not detected"
```

Expected:

- CMake/CTest/Python/SQLite available.
- If Poppler is missing, PDF extraction will log as unavailable.

---

## 3. Build and Automated Baseline

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(sysctl -n hw.ncpu)"
ctest --test-dir build --output-on-failure
```

Expected:

- Build succeeds.
- All tests pass (currently 26/26 in this repo state).

If tests fail, stop and fix build/test issues before manual UI verification.

Optional focused M2 subset:

```bash
ctest --test-dir build -R "semantic|embedding|boost|context|interaction|feedback|path-preferences|type-affinity" --output-on-failure
```

---

## 4. Critical Gates Before Full Manual QA

### Gate A: App QML startup

```bash
build/src/app/betterspotlight.app/Contents/MacOS/betterspotlight
```

Expected:

- App starts and remains running.
- No QML component load failures.

If startup fails with QML errors, use Troubleshooting T1 (Section 12).

### Gate B: Schema sanity for M2 tables

```bash
DB="$HOME/Library/Application Support/betterspotlight/index.db"
sqlite3 "$DB" ".tables" | tr ' ' '\n' | rg '^(items|search_index|feedback|frequencies|vector_map|interactions)$'
```

Expected minimum tables for M2 checks:

- `items`
- `search_index`
- `feedback`
- `frequencies`
- `vector_map`
- `interactions`

Also verify schema version:

```bash
sqlite3 "$DB" "SELECT value FROM settings WHERE key='schema_version';"
```

Expected: `2`

If `interactions` is missing or schema version is `1`, run Troubleshooting T2.

### Gate C: Semantic model assets

Query service expects model assets near its executable:

- `build/src/services/Resources/models/bge-small-en-v1.5-int8.onnx`
- `build/src/services/Resources/models/vocab.txt`

If missing, semantic search falls back to lexical only (expected fallback, see T3).

Bootstrap/fix command:

```bash
./tools/fetch_embedding_models.sh
cmake -S . -B build
cmake --build build -j8
```

Re-check assets:

```bash
ls -l build/src/services/Resources/models/bge-small-en-v1.5-int8.onnx
ls -l build/src/services/Resources/models/vocab.txt
```

---

## 5. UI Manual Verification (M1 + onboarding/settings)

Run this section only after Gate A is fixed.

### Step 5.1: Launch and tray/hotkey smoke test

1. Start app:

```bash
build/src/app/betterspotlight.app/Contents/MacOS/betterspotlight
```

2. Verify:

- Tray icon appears.
- Tray menu contains `Show Search`, `Settings...`, `Quit BetterSpotlight`.
- Search panel opens from tray action.

3. Hotkey behavior:

- Default hotkey is `Cmd+Space`.
- Verify open/close toggle.
- Verify panel reopens immediately after closing.

Pass criteria:

- No crash.
- Search panel visible and focused when opened.

### Step 5.2: Search panel interaction checks

Inside search panel:

1. Type a query and verify non-blocking UI.
2. Press `Esc` and verify panel closes.
3. Reopen panel and verify it still works.
4. Use arrow keys to move selection.
5. Press Enter to open selected item.
6. Use reveal/copy shortcuts:

- Reveal in Finder: `Cmd+R` (Qt key modifier mapping can vary; see T10)
- Copy path: `Cmd+Shift+C`

### Step 5.3: Onboarding checks

To force onboarding:

```bash
rm -f "$HOME/Library/Application Support/betterspotlight/BetterSpotlight/settings.json"
```

Restart app and verify:

- Step 1: Welcome.
- Step 2: FDA flow (`Open System Settings`, `Verify Access`, `Skip for now`).
- Step 3: Home directory map with per-folder mode dropdown.

After finishing onboarding, verify persisted keys:

```bash
cat "$HOME/Library/Application Support/betterspotlight/BetterSpotlight/settings.json"
```

Expected keys include:

- `onboarding_completed` (or equivalent onboarding flag)
- `home_directories`

### Step 5.4: Settings panel checks

Open `Settings...` from tray and verify tabs:

- General
- Indexing
- Exclusions
- Privacy
- Index Health

Validate persistence by changing values, quitting app, relaunching, and confirming values persist.

Important current implementation notes:

- Indexing actions are wired to indexer IPC (`pause/resume/rebuild/reindex`).
- Health tab merges query health with indexer queue stats.
- `Rebuild Vector Index` is wired to query service RPC (`rebuildVectorIndex`).
- `Clear Cache` is wired to extractor service RPC (`clearExtractionCache`).

---

## 6. Deterministic Backend Verification (without UI)

Use this when UI is blocked or when you need reproducible checks.

### Step 6.1: Create IPC helper

```bash
cat > /tmp/bs_ipc.py <<'PY'
#!/usr/bin/env python3
import json, os, socket, struct, sys

def call(service, method, params):
    sock_path = f"/tmp/betterspotlight-{os.getuid()}/{service}.sock"
    req = {"type": "request", "id": 1, "method": method, "params": params}
    payload = json.dumps(req, separators=(",", ":")).encode()
    msg = struct.pack(">I", len(payload)) + payload

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock_path)
    s.sendall(msg)

    header = s.recv(4)
    if len(header) != 4:
        raise RuntimeError("short header")
    n = struct.unpack(">I", header)[0]

    body = b""
    while len(body) < n:
        chunk = s.recv(n - len(body))
        if not chunk:
            raise RuntimeError("socket closed early")
        body += chunk
    s.close()
    print(json.dumps(json.loads(body.decode()), indent=2))

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("usage: bs_ipc.py <service> <method> [json-params]")
        sys.exit(2)
    service = sys.argv[1]
    method = sys.argv[2]
    params = json.loads(sys.argv[3]) if len(sys.argv) > 3 else {}
    call(service, method, params)
PY
chmod +x /tmp/bs_ipc.py
```

### Step 6.2: Start services

```bash
rm -f /tmp/betterspotlight-$(id -u)/indexer.sock /tmp/betterspotlight-$(id -u)/query.sock
build/src/services/indexer/betterspotlight-indexer > /tmp/bs-indexer.log 2>&1 &
build/src/services/query/betterspotlight-query > /tmp/bs-query.log 2>&1 &
```

Verify sockets:

```bash
ls -l /tmp/betterspotlight-$(id -u)/*.sock
```

Ping both services:

```bash
/tmp/bs_ipc.py indexer ping
/tmp/bs_ipc.py query ping
```

---

## 7. Controlled Fixture Run (M1 indexing/search)

Use fixture:

- `/Users/rexliu/betterspotlight/tests/Fixtures/standard_home_v1`

### Step 7.1: Backup your live DB first

```bash
DATA_DIR="$HOME/Library/Application Support/betterspotlight"
BACKUP_DIR="$DATA_DIR/manual-backup-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$BACKUP_DIR"
for f in index.db index.db-wal index.db-shm; do
  [ -f "$DATA_DIR/$f" ] && cp -f "$DATA_DIR/$f" "$BACKUP_DIR/$f"
  rm -f "$DATA_DIR/$f"
done
echo "Backed up DB files to: $BACKUP_DIR"
```

### Step 7.2: Start indexing fixture root

```bash
/tmp/bs_ipc.py indexer startIndexing '{"roots":["/Users/rexliu/betterspotlight/tests/Fixtures/standard_home_v1"]}'
```

Poll queue status:

```bash
/tmp/bs_ipc.py indexer getQueueStatus
```

### Step 7.3: Commit visibility check for small datasets

The pipeline now flushes transactions when the queue drains, including small batches. After indexing completes, run:

```bash
/tmp/bs_ipc.py query getHealth
```

Expected:

- `totalIndexedItems > 0`
- `queuePending` approaches `0` after completion

### Step 7.4: Verify DB content

```bash
DB="$HOME/Library/Application Support/betterspotlight/index.db"
sqlite3 "$DB" "SELECT COUNT(*) AS items FROM items;"
sqlite3 "$DB" "SELECT COUNT(*) AS chunks FROM content;"
sqlite3 "$DB" "SELECT COUNT(*) AS fts_rows FROM search_index;"
```

Expected:

- `items > 0`
- `search_index > 0` for extracted text content

### Step 7.5: Verify lexical search

```bash
/tmp/bs_ipc.py query search '{"query":"useEffect","limit":5}'
/tmp/bs_ipc.py query search '{"query":"kubectl apply","limit":5}'
/tmp/bs_ipc.py query search '{"query":"verifyJWT","limit":5}'
```

Expected:

- non-empty results for tokens present in indexed text files.

### Step 7.6: Verify staged concurrency (`preparing > 1`) in idle mode

Run a larger fixture/root and poll queue telemetry:

```bash
for i in $(seq 1 50); do
  /tmp/bs_ipc.py indexer getQueueStatus | python3 -c '
import json,sys
r=json.load(sys.stdin).get("result",{})
print(f"pending={r.get(\"pending\",0)} processing={r.get(\"processing\",0)} preparing={r.get(\"preparing\",0)} writing={r.get(\"writing\",0)} prepWorkers={r.get(\"prepWorkers\",0)} writerBatchDepth={r.get(\"writerBatchDepth\",0)} staleDropped={r.get(\"staleDropped\",0)}")
'
  sleep 0.1
done
```

Expected on M4 Pro in idle mode:

- `prepWorkers` reports `2` or `3` (current policy target: up to `3`).
- `preparing` should exceed `1` during non-trivial indexing bursts.
- `writing` remains `0` or `1` (single writer by design).

Note: very small corpora may complete too fast to observe `preparing > 1`.

### Step 7.7: Verify latest-wins stale-drop handling

Create a same-path event storm while indexing is active:

```bash
RACE_FILE="/Users/rexliu/betterspotlight/tests/Fixtures/standard_home_v1/Desktop/manual-race.txt"
for i in $(seq 1 150); do
  echo "race-$i" > "$RACE_FILE"
done
rm -f "$RACE_FILE"
/tmp/bs_ipc.py indexer reindexPath '{"path":"/Users/rexliu/betterspotlight/tests/Fixtures/standard_home_v1/Desktop"}'
```

Poll status:

```bash
/tmp/bs_ipc.py indexer getQueueStatus
```

Expected:

- Final search/DB state reflects newest path state only (deleted in this example).
- `staleDropped` may increase during storms (non-zero is a healthy sign of generation filtering).
- No crash/deadlock; queue drains to idle.

---

## 8. Incremental Update Checks (M1)

Use a temp file under fixture root:

```bash
TEST_FILE="/Users/rexliu/betterspotlight/tests/Fixtures/standard_home_v1/Desktop/manual-incremental-check.txt"
echo "alpha-token-123" > "$TEST_FILE"
```

Restart indexer service if needed, then start indexing fixture root again.

Search token:

```bash
/tmp/bs_ipc.py query search '{"query":"alpha-token-123","limit":5}'
```

Modify file:

```bash
echo "beta-token-456" >> "$TEST_FILE"
/tmp/bs_ipc.py query search '{"query":"beta-token-456","limit":5}'
```

Delete file:

```bash
rm -f "$TEST_FILE"
/tmp/bs_ipc.py indexer reindexPath '{"path":"/Users/rexliu/betterspotlight/tests/Fixtures/standard_home_v1/Desktop"}'
/tmp/bs_ipc.py query search '{"query":"alpha-token-123","limit":5}'
```

Expected:

- create/modify/delete reflected after indexing cycle.

---

## 9. Exclusions and Privacy Checks

### Step 9.1: `.bsignore` enforcement

Create/update `~/.bsignore`:

```bash
cat > "$HOME/.bsignore" <<'EOF2'
manual-incremental-check.txt
EOF2
```

Restart indexer and reindex fixture root. Verify excluded file does not appear in search.

### Step 9.2: Sensitive path behavior

Sensitive patterns include `.ssh`, `.gnupg`, `.aws`, `Library/Preferences`, `Library/Keychains`.

Check classification behavior by indexing files under those paths and confirming metadata-only behavior in logs.

---

## 10. M2 Feedback / Frequency / Interaction Checks

### Step 10.1: Feedback and frequency

1. Run a search and capture `itemId` from the top result.
2. Record feedback:

```bash
/tmp/bs_ipc.py query recordFeedback '{"itemId":123,"action":"open","query":"useEffect","position":0}'
```

3. Check frequency:

```bash
/tmp/bs_ipc.py query getFrequency '{"itemId":123}'
```

Expected:

- `openCount` increments.
- `frequencyTier`/`boost` reflect opens.

### Step 10.2: Interaction/path/type APIs

```bash
/tmp/bs_ipc.py query record_interaction '{"query":"useEffect","selectedItemId":123,"selectedPath":"/path/to/file","matchType":"content","resultPosition":1,"frontmostApp":"com.microsoft.VSCode"}'
/tmp/bs_ipc.py query get_path_preferences '{"limit":10}'
/tmp/bs_ipc.py query get_file_type_affinity '{}'
/tmp/bs_ipc.py query run_aggregation '{}'
/tmp/bs_ipc.py query export_interaction_data '{}'
```

If `record_interaction` fails with `no such table: interactions`, use T2 (legacy DB not migrated).

---

## 11. M2 Semantic Search Checks

### Step 11.1: Fallback behavior (no model)

If model/vocab are missing, query logs should show semantic disabled and lexical fallback.

Check logs:

```bash
rg -n "EmbeddingManager unavailable|tokenizer vocab not loaded|semantic search disabled" /tmp/bs-query.log
```

Expected:

- search still returns lexical results (no crash).

### Step 11.2: Semantic enabled path

Preconditions:

- ONNX Runtime available at build time.
- Model and vocab present at runtime paths.
- `vector_map` populated (currently not auto-populated by indexer pipeline in this snapshot).

Conceptual query check:

```bash
/tmp/bs_ipc.py query search '{"query":"settings","limit":5}'
```

Expected (when semantic is fully wired and populated):

- conceptually related results even without direct token overlap.

---

## 12. Troubleshooting Guide

### T1. App fails at startup with QML duplicate signal errors

Symptom:

- App exits with QML load failure.

Cause:

- A QML component declares an explicit `...Changed` signal for a property that already has an auto-generated notifier (for example `property var model` plus `signal modelChanged()`).

Fix:

- Remove explicit duplicate notifier signal declarations and any manual calls to them.
- Rebuild app target and relaunch.

### T2. M2 interaction APIs fail: `no such table: interactions`

Symptom:

- `record_interaction` returns internal error.
- Query logs show prepare failures on `interactions`.

Cause:

- Existing DB predates schema version `2` and was not migrated.

Fix options:

1. Restart services and let built-in migration run to schema version `2`.
2. If migration still did not run, backup DB and recreate schema from clean DB files.

### T3. Semantic unavailable at runtime

Symptom:

- Logs show tokenizer/model missing or ONNX unavailable.

Cause:

- Missing `vocab.txt`/ONNX model at expected runtime path, or ONNX Runtime not found at build.

Fix:

- Fetch model assets and rebuild so post-build sync copies them into runtime resources:
  - `./tools/fetch_embedding_models.sh`
  - `cmake -S . -B build && cmake --build build -j8`
- Reconfigure/rebuild with ONNX Runtime detected.

### T4. `tests/relevance/run_relevance_test.sh` hangs or is unusable

Symptoms:

- Script times out.
- It expects CLI flags on service binaries that currently run as socket daemons.

Cause:

- Script assumes non-existent CLI mode for `betterspotlight-indexer`/`betterspotlight-query`.

Fix:

- The script now fails fast with an explicit daemon-mode warning.
- Use CTest integration tests and IPC-driven checks from this manual instead.

### T5. Index Health tab shows many `--` values

Symptom:

- Health UI placeholders remain empty after refresh.

Cause:

- Query and/or indexer service is not connected, or health refresh happened before services were ready.

Fix:

- Confirm both sockets are alive (`query.sock`, `indexer.sock`), then click Refresh again.
- Check `/tmp/bs-query.log` and `/tmp/bs-indexer.log` for IPC errors.

### T6. Settings actions appear to do nothing

Symptoms:

- Indexing/vector/cache actions do not have visible backend effect.

Cause:

- Relevant service is disconnected, or action failed server-side.

Fix:

- Verify all services are connected (`indexer`, `query`, `extractor`) before triggering actions.
- Check `/tmp/bs-indexer.log`, `/tmp/bs-query.log`, `/tmp/bs-extractor.log` for the corresponding RPC error.

### T7. Small fixture indexed, but search/health stay empty

Symptom:

- `startIndexing` succeeds; logs show processing; query health still zero.

Cause:

- Indexing did not actually start on the expected root, or query service is reading a different DB path.

Workaround:

- Call `getQueueStatus` and verify pending/processing counters move.
- Check DB path and row counts directly with sqlite3 queries.

### T8. PDF files never searchable

Symptom:

- PDF extraction failures in logs (`Poppler not found`).

Cause:

- Poppler dependency not detected at build/runtime.

Fix:

- Install Poppler Qt6 development/runtime libs and rebuild.

### T9. OCR failures on image fixtures

Symptom:

- Leptonica pix read failures for placeholder `.jpg` files in fixture.

Cause:

- Some fixture “image” files are textual placeholders, not real image binaries.

Fix:

- Use real binary images when validating OCR path.

### T10. Reveal/copy shortcuts don’t trigger as expected on macOS

Symptom:

- `Cmd+R` or `Cmd+Shift+C` not firing.

Cause:

- Qt modifier mapping differences (`ControlModifier` vs `MetaModifier`) can vary in QML key handling.

Fix:

- Validate actual modifier mapping on your Qt/macOS runtime and adjust shortcut handling.

### T11. Results include unrelated files from your real home index

Symptom:

- Fixture queries return files outside fixture.

Cause:

- Existing live DB content still present.

Fix:

- Use backup/delete/restore DB procedure in Section 7.

### T12. `preparing` never exceeds `1`

Symptoms:

- `prepWorkers` is `1`, or `preparing` stays `0/1` during large indexing runs.

Causes:

- Active-mode cap is engaged (`setUserActive(true)` behavior).
- Workload is too small/fast to observe parallel prep.
- Heavy extractor classes (OCR/PDF) are correctly capped to single lane.

Fix:

- Re-run with a larger text-heavy corpus and poll every `50-100ms`.
- Confirm `prepWorkers` in queue status is `2-3` in idle conditions.
- If `prepWorkers` remains `1`, verify indexer received `setUserActive(false)` after query bursts.

### T13. Writer bottleneck diagnosis (`preparing` high, throughput low)

Symptoms:

- `preparing` often > 1, but `pending` drains slowly.
- `writerBatchDepth` frequently grows before commits.

Cause:

- Single SQLite writer is saturated (expected design tradeoff for correctness).

Fix:

- Confirm this is acceptable for current workload/latency budget.
- Prefer reducing expensive per-item write amplification (for example, avoiding unnecessary chunk rewrites) before changing writer model.
- Keep writer single-threaded unless you are prepared to redesign SQLite ownership/locking semantics.

### T14. Queue status counters look inconsistent

Symptoms:

- `processing` differs from expectations.

Cause:

- `processing` is a compatibility aggregate: `preparing + writing`.

Fix:

- Treat `preparing`, `writing`, `coalesced`, `staleDropped`, `prepWorkers`, and `writerBatchDepth` as the authoritative staged counters.

---

## 13. Cleanup and Restore

Stop services:

```bash
pkill -f betterspotlight-indexer || true
pkill -f betterspotlight-query || true
pkill -f betterspotlight-extractor || true
```

If you ran controlled DB tests, restore backup:

```bash
DATA_DIR="$HOME/Library/Application Support/betterspotlight"
BACKUP_DIR="<the backup dir you created>"
rm -f "$DATA_DIR/index.db" "$DATA_DIR/index.db-wal" "$DATA_DIR/index.db-shm"
for f in index.db index.db-wal index.db-shm; do
  [ -f "$BACKUP_DIR/$f" ] && mv -f "$BACKUP_DIR/$f" "$DATA_DIR/$f"
done
```

---

## 14. Recommended Pass/Fail Recording Template

For each major block, record:

- command(s) run
- expected outcome
- actual outcome
- pass/fail
- logs/screenshots path

Minimum evidence set:

1. Build + ctest output
2. App launch output
3. Socket ping output
4. DB table/count checks
5. Sample search responses
6. Troubleshooting actions taken
