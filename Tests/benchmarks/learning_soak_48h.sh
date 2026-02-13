#!/bin/bash
# learning_soak_48h.sh - Continual-learning soak harness (default 48h)
#
# Purpose:
#   Validate BetterSpotlight v1 continual ranker loop under long-running load:
#   ingest behavior events -> attribution labels -> train cycles -> health snapshots.
#
# Usage:
#   ./learning_soak_48h.sh [duration_seconds]
#
# Optional env:
#   BS_STRESS_DURATION_SECONDS           Override arg/default duration
#   BS_STRESS_QUERY_BIN                  Query service binary path
#   BS_STRESS_INDEXER_BIN                Indexer service binary path
#   BS_STRESS_EXTRACTOR_BIN              Extractor service binary path
#   BS_STRESS_SAMPLE_INTERVAL            Health sampling interval seconds (default: 5)
#   BS_STRESS_INTEGRITY_INTERVAL         SQLite integrity interval seconds (default: 120)
#   BS_STRESS_ARTIFACT_DIR               Optional explicit artifact directory
#   BS_LEARNING_ROLLOUT_MODE             instrumentation_only|shadow_training|blended_ranking
#                                        (default: shadow_training)
#   BS_LEARNING_PAUSE_ON_USER_INPUT      0/1 (default: 0 for harness determinism)
#   BS_LEARNING_CYCLE_INTERVAL           Manual trigger cadence seconds (default: 60)
#   BS_LEARNING_MIN_CYCLES               Minimum cycles expected (default: 1)
#   BS_LEARNING_ASSERT_PROGRESS          0/1 fail if cycles do not progress (default: 1)
#
# Artifacts:
#   /tmp/bs_learning_soak_48h_<timestamp>/
#     - summary.json
#     - metrics.json
#     - learning_health_samples.json
#     - learning_cycle_events.json
#     - query.log/indexer.log/extractor.log

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEFAULT_DURATION=$((48 * 3600))
DURATION_SECONDS="${BS_STRESS_DURATION_SECONDS:-${1:-$DEFAULT_DURATION}}"
QUERY_BIN="${BS_STRESS_QUERY_BIN:-$ROOT_DIR/build/src/services/query/betterspotlight-query}"
INDEXER_BIN="${BS_STRESS_INDEXER_BIN:-$ROOT_DIR/build/src/services/indexer/betterspotlight-indexer}"
EXTRACTOR_BIN="${BS_STRESS_EXTRACTOR_BIN:-$ROOT_DIR/build/src/services/extractor/betterspotlight-extractor}"
SAMPLE_INTERVAL="${BS_STRESS_SAMPLE_INTERVAL:-5}"
INTEGRITY_INTERVAL="${BS_STRESS_INTEGRITY_INTERVAL:-120}"
ARTIFACT_DIR_OVERRIDE="${BS_STRESS_ARTIFACT_DIR:-}"
LEARNING_ROLLOUT_MODE="${BS_LEARNING_ROLLOUT_MODE:-shadow_training}"
LEARNING_PAUSE_ON_USER_INPUT="${BS_LEARNING_PAUSE_ON_USER_INPUT:-0}"
LEARNING_CYCLE_INTERVAL="${BS_LEARNING_CYCLE_INTERVAL:-60}"
LEARNING_MIN_CYCLES="${BS_LEARNING_MIN_CYCLES:-1}"
LEARNING_ASSERT_PROGRESS="${BS_LEARNING_ASSERT_PROGRESS:-1}"

for bin in "$QUERY_BIN" "$INDEXER_BIN" "$EXTRACTOR_BIN"; do
    if [[ ! -x "$bin" ]]; then
        echo "Error: missing executable: $bin"
        exit 1
    fi
done

echo "=== BetterSpotlight Continual Learning Soak Harness ==="
echo "Duration: ${DURATION_SECONDS}s"
echo "Rollout mode: ${LEARNING_ROLLOUT_MODE}"
echo "Manual cycle interval: ${LEARNING_CYCLE_INTERVAL}s"
echo "Pause on input: ${LEARNING_PAUSE_ON_USER_INPUT}"
echo "Query:    $QUERY_BIN"
echo "Indexer:  $INDEXER_BIN"
echo "Extractor:$EXTRACTOR_BIN"

python3 - "$DURATION_SECONDS" "$QUERY_BIN" "$INDEXER_BIN" "$EXTRACTOR_BIN" \
    "$SAMPLE_INTERVAL" "$INTEGRITY_INTERVAL" "$ARTIFACT_DIR_OVERRIDE" \
    "$LEARNING_ROLLOUT_MODE" "$LEARNING_PAUSE_ON_USER_INPUT" "$LEARNING_CYCLE_INTERVAL" \
    "$LEARNING_MIN_CYCLES" "$LEARNING_ASSERT_PROGRESS" <<'PY'
import hashlib
import json
import os
import random
import socket
import sqlite3
import struct
import subprocess
import sys
import time
from pathlib import Path


def parse_bool(value: str, default: bool = False) -> bool:
    normalized = str(value).strip().lower()
    if not normalized:
        return default
    return normalized in ("1", "true", "yes", "on")


duration = int(sys.argv[1])
query_bin = sys.argv[2]
indexer_bin = sys.argv[3]
extractor_bin = sys.argv[4]
sample_interval = int(sys.argv[5])
integrity_interval = int(sys.argv[6])
artifact_dir_override = sys.argv[7].strip()
learning_rollout_mode = sys.argv[8].strip().lower()
learning_pause_on_user_input = parse_bool(sys.argv[9], default=False)
learning_cycle_interval = max(5, int(sys.argv[10]))
learning_min_cycles = max(0, int(sys.argv[11]))
learning_assert_progress = parse_bool(sys.argv[12], default=True)

if learning_rollout_mode not in ("instrumentation_only", "shadow_training", "blended_ranking"):
    raise SystemExit(f"Invalid BS_LEARNING_ROLLOUT_MODE: {learning_rollout_mode}")

ts = time.strftime("%Y%m%d_%H%M%S")
if artifact_dir_override:
    artifact_dir = Path(artifact_dir_override).expanduser()
else:
    artifact_dir = Path(f"/tmp/bs_learning_soak_48h_{ts}")
artifact_dir.mkdir(parents=True, exist_ok=True)
home_dir = artifact_dir / "home"
home_dir.mkdir(parents=True, exist_ok=True)
corpus_dir = artifact_dir / "corpus"
corpus_dir.mkdir(parents=True, exist_ok=True)

db_path = home_dir / "Library" / "Application Support" / "betterspotlight" / "index.db"
db_path.parent.mkdir(parents=True, exist_ok=True)

env = os.environ.copy()
env["HOME"] = str(home_dir)
env["TMPDIR"] = str(artifact_dir / "tmp")
env["BETTERSPOTLIGHT_SOCKET_DIR"] = str(artifact_dir / "sockets")
env["BETTERSPOTLIGHT_DATA_DIR"] = str(
    home_dir / "Library" / "Application Support" / "betterspotlight"
)
Path(env["TMPDIR"]).mkdir(parents=True, exist_ok=True)
Path(env["BETTERSPOTLIGHT_DATA_DIR"]).mkdir(parents=True, exist_ok=True)

socket_dir = Path(env["BETTERSPOTLIGHT_SOCKET_DIR"])
socket_dir.mkdir(parents=True, exist_ok=True)
query_sock = socket_dir / "query.sock"
indexer_sock = socket_dir / "indexer.sock"
extractor_sock = socket_dir / "extractor.sock"

for stale in (query_sock, indexer_sock, extractor_sock):
    try:
        stale.unlink()
    except FileNotFoundError:
        pass

service_logs = {
    "query": open(artifact_dir / "query.log", "w", encoding="utf-8"),
    "indexer": open(artifact_dir / "indexer.log", "w", encoding="utf-8"),
    "extractor": open(artifact_dir / "extractor.log", "w", encoding="utf-8"),
}

services = {}
errors = []
search_latencies_ms = []
metrics_rows = []
learning_health_samples = []
learning_cycle_events = []
integrity_failures = 0
iterations = 0
behavior_events_sent = 0
positive_events_sent = 0
manual_cycle_attempts = 0
manual_cycle_promotions = 0
learning_configured = False
initial_learning_health = {}
final_learning_health = {}
indexed_items = []


def wait_socket(path: Path, timeout: float = 30.0) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if path.exists():
            return True
        time.sleep(0.05)
    return False


def rpc(sock_path: Path, method: str, params=None, rid: int = 1, timeout: float = 10.0):
    if params is None:
        params = {}
    payload = json.dumps({
        "type": "request",
        "id": rid,
        "method": method,
        "params": params,
    }, separators=(",", ":")).encode("utf-8")
    msg = struct.pack(">I", len(payload)) + payload
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(str(sock_path))
    s.sendall(msg)
    hdr = s.recv(4)
    if len(hdr) != 4:
        raise RuntimeError("short IPC header")
    n = struct.unpack(">I", hdr)[0]
    body = b""
    while len(body) < n:
        chunk = s.recv(n - len(body))
        if not chunk:
            break
        body += chunk
    s.close()
    if len(body) != n:
        raise RuntimeError("short IPC body")
    return json.loads(body.decode("utf-8"))


def resp_error(resp):
    if resp.get("type") == "error":
        return resp.get("error", {}).get("message", str(resp))
    return None


def digest_for(query: str, context_event_id: str, iteration: int) -> str:
    payload = f"{query}|{context_event_id}|{iteration}|{int(time.time())}"
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()[:32]


def load_indexed_items(db_file: Path, path_prefix: str):
    if not db_file.exists():
        return []
    con = sqlite3.connect(str(db_file))
    try:
        rows = con.execute(
            "SELECT id, path FROM items WHERE path LIKE ? ORDER BY id ASC",
            (f"{path_prefix}%",),
        ).fetchall()
        out = []
        for row in rows:
            if not row or len(row) < 2:
                continue
            item_id = int(row[0] or 0)
            path = str(row[1] or "")
            if item_id > 0 and path:
                out.append({"itemId": item_id, "path": path})
        return out
    finally:
        con.close()


rid = 1
started_at = time.time()
last_integrity = 0.0
last_sample = 0.0
last_manual_cycle = 0.0

try:
    services = {
        "query": subprocess.Popen([query_bin], stdout=service_logs["query"],
                                  stderr=subprocess.STDOUT, env=env),
        "indexer": subprocess.Popen([indexer_bin], stdout=service_logs["indexer"],
                                    stderr=subprocess.STDOUT, env=env),
        "extractor": subprocess.Popen([extractor_bin], stdout=service_logs["extractor"],
                                      stderr=subprocess.STDOUT, env=env),
    }

    for name, sock in (("query", query_sock), ("indexer", indexer_sock), ("extractor", extractor_sock)):
        if not wait_socket(sock):
            raise RuntimeError(f"{name} socket did not appear: {sock}")

    start_indexing = rpc(indexer_sock, "startIndexing", {"roots": [str(corpus_dir)]}, rid=rid, timeout=30.0)
    rid += 1
    start_err = resp_error(start_indexing)
    if start_err:
        raise RuntimeError(f"startIndexing failed: {start_err}")

    seed_files = {
        "readme.txt": "betterspotlight learning soak corpus\nrank adaptation baseline\n",
        "notes.md": "meeting notes about deployment reliability and memory drift\n",
        "finance.txt": "credit report statement account transaction history\n",
        "infra.md": "query router heuristic, semantic need score, ranking blend alpha\n",
    }
    for name, content in seed_files.items():
        (corpus_dir / name).write_text(content, encoding="utf-8")
    rpc(indexer_sock, "reindexPath", {"path": str(corpus_dir)}, rid=rid, timeout=20.0)
    rid += 1
    # Wait briefly for indexing and resolve indexed item ids for stable positive events.
    items_deadline = time.time() + 20.0
    while time.time() < items_deadline:
        indexed_items = load_indexed_items(db_path, str(corpus_dir))
        if indexed_items:
            break
        time.sleep(0.5)
    if not indexed_items:
        errors.append("No indexed items resolved from SQLite after seed reindex")

    consent_params = {
        "behaviorStreamEnabled": True,
        "learningEnabled": True,
        "learningPauseOnUserInput": learning_pause_on_user_input,
        "onlineRankerRolloutMode": learning_rollout_mode,
        "captureScope": {
            "appActivityEnabled": False,
            "inputActivityEnabled": False,
            "searchEventsEnabled": True,
            "windowTitleHashEnabled": False,
            "browserHostHashEnabled": False,
        },
        "denylistApps": [],
    }
    consent_resp = rpc(query_sock, "set_learning_consent", consent_params, rid=rid, timeout=15.0)
    rid += 1
    consent_err = resp_error(consent_resp)
    if consent_err:
        raise RuntimeError(f"set_learning_consent failed: {consent_err}")
    learning_configured = True

    health_resp = rpc(query_sock, "get_learning_health", {}, rid=rid, timeout=15.0)
    rid += 1
    health_err = resp_error(health_resp)
    if health_err:
        raise RuntimeError(f"get_learning_health failed after consent: {health_err}")
    initial_learning_health = health_resp.get("result", {}).get("learning", {}) or {}

    queries = [
        "credit report",
        "deployment reliability",
        "memory drift",
        "query service",
        "readme stress",
        "transaction history",
        "notes about deployment",
        "semantic need score",
        "ranking blend alpha",
        "heuristic router",
    ]

    while time.time() - started_at < duration:
        iterations += 1
        now = time.time()

        for name, proc in services.items():
            code = proc.poll()
            if code is not None:
                raise RuntimeError(f"{name} exited unexpectedly with code {code}")

        if iterations % 3 == 0:
            f = corpus_dir / f"dynamic_{iterations % 12}.txt"
            f.write_text(
                f"iteration={iterations}\nconcept=continual_learning\nnoise={random.randint(1, 9999)}\n",
                encoding="utf-8",
            )
            try:
                reindex_resp = rpc(indexer_sock, "reindexPath", {"path": str(f)}, rid=rid, timeout=10.0)
                reindex_err = resp_error(reindex_resp)
                if reindex_err:
                    errors.append(f"reindexPath failed for {f}: {reindex_err}")
            except Exception as ex:
                errors.append(f"reindexPath failed for {f}: {ex}")
            rid += 1

        for q in queries:
            context_event_id = f"ctx-{rid}-{int(time.time() * 1000)}"
            activity_digest = digest_for(q, context_event_id, iterations)

            # Build behavior stream context event.
            query_event = {
                "eventId": context_event_id,
                "source": "betterspotlight",
                "eventType": "query_submitted",
                "timestamp": int(time.time()),
                "query": q,
                "contextEventId": context_event_id,
                "activityDigest": activity_digest,
                "inputMeta": {
                    "keyEventCount": len(q),
                    "shortcutCount": 0,
                    "scrollCount": 0,
                    "metadataOnly": True,
                },
                "attributionConfidence": 0.2,
            }
            try:
                query_event_resp = rpc(query_sock, "record_behavior_event", query_event, rid=rid, timeout=10.0)
                event_err = resp_error(query_event_resp)
                if event_err:
                    errors.append(f"query_submitted record_behavior_event failed: {event_err}")
                else:
                    behavior_events_sent += 1
            except Exception as ex:
                errors.append(f"query_submitted record_behavior_event exception: {ex}")
            rid += 1

            search_params = {
                "query": q,
                "limit": 20,
                "debug": True,
                "context": {
                    "contextEventId": context_event_id,
                    "contextFeatureVersion": 1,
                    "activityDigest": activity_digest,
                },
            }
            t0 = time.time()
            try:
                resp = rpc(query_sock, "search", search_params, rid=rid, timeout=20.0)
            except Exception as ex:
                try:
                    time.sleep(0.2)
                    resp = rpc(query_sock, "search", search_params, rid=rid, timeout=30.0)
                except Exception as retry_ex:
                    errors.append(f"search RPC failed for '{q}': {retry_ex}")
                    rid += 1
                    continue
            rid += 1
            search_latencies_ms.append((time.time() - t0) * 1000.0)
            search_err = resp_error(resp)
            if search_err:
                errors.append(f"search error for '{q}': {search_err}")
                continue

            result_payload = resp.get("result", {})
            results = result_payload.get("results", [])
            top = results[0] if isinstance(results, list) and results else {}
            item_id = int(top.get("itemId", 0) or 0)
            item_path = str(top.get("path", "") or "")
            if (item_id <= 0 or not item_path) and indexed_items:
                fallback = indexed_items[(rid + iterations) % len(indexed_items)]
                item_id = int(fallback["itemId"])
                item_path = str(fallback["path"])
            if item_id <= 0 or not item_path:
                continue

            # Emit positive interaction event mapped to indexed item.
            positive_event = {
                "eventId": f"open-{rid}-{int(time.time() * 1000)}",
                "source": "betterspotlight",
                "eventType": "result_open",
                "timestamp": int(time.time()),
                "query": q,
                "itemId": item_id,
                "itemPath": item_path,
                "contextEventId": context_event_id,
                "activityDigest": activity_digest,
                "attributionConfidence": 1.0,
            }
            try:
                positive_resp = rpc(query_sock, "record_behavior_event", positive_event, rid=rid, timeout=10.0)
                positive_err = resp_error(positive_resp)
                if positive_err:
                    errors.append(f"result_open record_behavior_event failed: {positive_err}")
                else:
                    behavior_events_sent += 1
                    positive_events_sent += 1
            except Exception as ex:
                errors.append(f"result_open record_behavior_event exception: {ex}")
            rid += 1

        if now - last_manual_cycle >= learning_cycle_interval:
            manual_cycle_attempts += 1
            try:
                cycle_resp = rpc(query_sock,
                                 "trigger_learning_cycle",
                                 {"manual": True},
                                 rid=rid,
                                 timeout=20.0)
                cycle_err = resp_error(cycle_resp)
                if cycle_err:
                    errors.append(f"trigger_learning_cycle failed: {cycle_err}")
                    cycle_entry = {
                        "t": int(now),
                        "error": cycle_err,
                    }
                else:
                    cycle_result = cycle_resp.get("result", {})
                    promoted = bool(cycle_result.get("promoted", False))
                    if promoted:
                        manual_cycle_promotions += 1
                    cycle_entry = {
                        "t": int(now),
                        "promoted": promoted,
                        "reason": cycle_result.get("reason", ""),
                        "lastCycleStatus": cycle_result.get("learning", {}).get("lastCycleStatus", ""),
                        "lastCycleReason": cycle_result.get("learning", {}).get("lastCycleReason", ""),
                    }
                learning_cycle_events.append(cycle_entry)
            except Exception as ex:
                errors.append(f"trigger_learning_cycle exception: {ex}")
                learning_cycle_events.append({"t": int(now), "error": str(ex)})
            rid += 1
            last_manual_cycle = now

        if now - last_sample >= sample_interval:
            # General health.
            try:
                h = rpc(query_sock, "getHealthDetails", {"limit": 20, "offset": 0}, rid=rid, timeout=15.0)
                rid += 1
                h_err = resp_error(h)
                if h_err:
                    errors.append(f"getHealthDetails failed: {h_err}")
                else:
                    result = h.get("result", {})
                    idx = result.get("indexHealth", {})
                    details = result.get("details", {})
                    metrics_rows.append({
                        "t": int(now),
                        "overallStatus": idx.get("overallStatus"),
                        "queuePending": idx.get("queuePending", 0),
                        "queueInProgress": idx.get("queueInProgress", 0),
                        "searchCount": idx.get("searchCount", 0),
                        "criticalFailureRows": details.get("criticalFailureRows", 0),
                        "expectedGapFailureRows": details.get("expectedGapFailureRows", 0),
                    })
            except Exception as ex:
                errors.append(f"getHealthDetails exception: {ex}")

            # Learning health.
            try:
                learning_health_resp = rpc(query_sock, "get_learning_health", {}, rid=rid, timeout=15.0)
                rid += 1
                learning_health_err = resp_error(learning_health_resp)
                if learning_health_err:
                    errors.append(f"get_learning_health failed: {learning_health_err}")
                else:
                    learning = learning_health_resp.get("result", {}).get("learning", {}) or {}
                    learning_health_samples.append({
                        "t": int(now),
                        "modelVersion": learning.get("modelVersion", ""),
                        "activeBackend": learning.get("activeBackend", ""),
                        "queueDepth": learning.get("queueDepth", 0),
                        "replaySize": learning.get("replaySize", 0),
                        "cyclesRun": learning.get("cyclesRun", 0),
                        "cyclesSucceeded": learning.get("cyclesSucceeded", 0),
                        "cyclesRejected": learning.get("cyclesRejected", 0),
                        "lastCycleStatus": learning.get("lastCycleStatus", ""),
                        "lastCycleReason": learning.get("lastCycleReason", ""),
                        "rolloutMode": learning.get("onlineRankerRolloutMode", ""),
                        "scheduler": learning.get("scheduler", {}),
                        "fallbackMissingModel": learning.get("fallbackMissingModel", 0),
                        "fallbackLearningDisabled": learning.get("fallbackLearningDisabled", 0),
                        "fallbackResourceBudget": learning.get("fallbackResourceBudget", 0),
                        "fallbackRolloutMode": learning.get("fallbackRolloutMode", 0),
                    })
                    final_learning_health = learning
            except Exception as ex:
                errors.append(f"get_learning_health exception: {ex}")

            last_sample = now

        if now - last_integrity >= integrity_interval:
            if db_path.exists():
                try:
                    con = sqlite3.connect(str(db_path))
                    row = con.execute("PRAGMA integrity_check").fetchone()
                    con.close()
                    if not row or row[0] != "ok":
                        integrity_failures += 1
                        errors.append(f"integrity_check failed: {row}")
                except Exception as ex:
                    integrity_failures += 1
                    errors.append(f"integrity_check exception: {ex}")
            last_integrity = now

    # Final learning snapshot.
    try:
        final_resp = rpc(query_sock, "get_learning_health", {}, rid=rid, timeout=15.0)
        rid += 1
        final_err = resp_error(final_resp)
        if final_err:
            errors.append(f"final get_learning_health failed: {final_err}")
        else:
            final_learning_health = final_resp.get("result", {}).get("learning", {}) or final_learning_health
    except Exception as ex:
        errors.append(f"final get_learning_health exception: {ex}")

finally:
    for sock, method in ((query_sock, "shutdown"), (indexer_sock, "shutdown"), (extractor_sock, "shutdown")):
        try:
            rpc(sock, method, {}, rid=rid, timeout=3.0)
            rid += 1
        except Exception:
            pass

    time.sleep(0.5)
    for proc in services.values():
        if proc.poll() is None:
            proc.terminate()
    for proc in services.values():
        try:
            proc.wait(timeout=5.0)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass
    for fp in service_logs.values():
        fp.close()


def latency_p95(values):
    if not values:
        return 0.0
    ordered = sorted(values)
    idx = int(max(0, len(ordered) * 0.95 - 1))
    return ordered[idx]


def metric_delta(key: str) -> int:
    before = int(initial_learning_health.get(key, 0) or 0)
    after = int(final_learning_health.get(key, 0) or 0)
    return after - before


summary = {
    "durationSeconds": int(time.time() - started_at),
    "iterations": iterations,
    "queryCount": len(search_latencies_ms),
    "latencyAvgMs": round(sum(search_latencies_ms) / len(search_latencies_ms), 2)
    if search_latencies_ms else 0.0,
    "latencyP95Ms": round(latency_p95(search_latencies_ms), 2)
    if search_latencies_ms else 0.0,
    "latencyMaxMs": round(max(search_latencies_ms), 2) if search_latencies_ms else 0.0,
    "integrityFailures": integrity_failures,
    "artifactDir": str(artifact_dir),
    "dbPath": str(db_path),
    "behaviorEventsSent": behavior_events_sent,
    "positiveEventsSent": positive_events_sent,
    "manualCycleAttempts": manual_cycle_attempts,
    "manualCyclePromotions": manual_cycle_promotions,
    "learningConfigured": learning_configured,
    "learningConfig": {
        "rolloutMode": learning_rollout_mode,
        "pauseOnUserInput": learning_pause_on_user_input,
        "manualCycleIntervalSec": learning_cycle_interval,
        "minCyclesRequired": learning_min_cycles,
        "assertProgress": learning_assert_progress,
    },
    "learningDelta": {
        "cyclesRun": metric_delta("cyclesRun"),
        "cyclesSucceeded": metric_delta("cyclesSucceeded"),
        "cyclesRejected": metric_delta("cyclesRejected"),
    },
    "learningModelVersionChanged": (
        bool(initial_learning_health.get("modelVersion", ""))
        and bool(final_learning_health.get("modelVersion", ""))
        and initial_learning_health.get("modelVersion") != final_learning_health.get("modelVersion")
    ),
    "errors": errors,
}

if learning_assert_progress and learning_configured:
    if summary["learningDelta"]["cyclesRun"] < learning_min_cycles:
        summary["errors"].append(
            f"learning cycles did not progress enough: "
            f"delta={summary['learningDelta']['cyclesRun']} required={learning_min_cycles}"
        )

with open(artifact_dir / "summary.json", "w", encoding="utf-8") as f:
    json.dump(summary, f, indent=2)
with open(artifact_dir / "metrics.json", "w", encoding="utf-8") as f:
    json.dump(metrics_rows, f, indent=2)
with open(artifact_dir / "learning_health_samples.json", "w", encoding="utf-8") as f:
    json.dump(learning_health_samples, f, indent=2)
with open(artifact_dir / "learning_cycle_events.json", "w", encoding="utf-8") as f:
    json.dump(learning_cycle_events, f, indent=2)
with open(artifact_dir / "initial_learning_health.json", "w", encoding="utf-8") as f:
    json.dump(initial_learning_health, f, indent=2)
with open(artifact_dir / "final_learning_health.json", "w", encoding="utf-8") as f:
    json.dump(final_learning_health, f, indent=2)

print(json.dumps(summary, indent=2))
if summary["errors"] or summary["integrityFailures"] > 0:
    raise SystemExit(2)
PY

echo "✓ learning_soak_48h complete"
