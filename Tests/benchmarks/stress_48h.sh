#!/bin/bash
# stress_48h.sh - Service-driven long-run stress harness (default 48h)
#
# Usage:
#   ./stress_48h.sh [duration_seconds]
#
# Optional env:
#   BS_STRESS_DURATION_SECONDS (overrides arg/default)
#   BS_STRESS_QUERY_BIN
#   BS_STRESS_INDEXER_BIN
#   BS_STRESS_EXTRACTOR_BIN
#   BS_STRESS_SAMPLE_INTERVAL (default 5)
#   BS_STRESS_INTEGRITY_INTERVAL (default 120)
#
# Artifacts are written to /tmp/bs_stress_48h_<timestamp>/

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEFAULT_DURATION=$((48 * 3600))
DURATION_SECONDS="${BS_STRESS_DURATION_SECONDS:-${1:-$DEFAULT_DURATION}}"
QUERY_BIN="${BS_STRESS_QUERY_BIN:-$ROOT_DIR/build/src/services/query/betterspotlight-query}"
INDEXER_BIN="${BS_STRESS_INDEXER_BIN:-$ROOT_DIR/build/src/services/indexer/betterspotlight-indexer}"
EXTRACTOR_BIN="${BS_STRESS_EXTRACTOR_BIN:-$ROOT_DIR/build/src/services/extractor/betterspotlight-extractor}"
SAMPLE_INTERVAL="${BS_STRESS_SAMPLE_INTERVAL:-5}"
INTEGRITY_INTERVAL="${BS_STRESS_INTEGRITY_INTERVAL:-120}"

for bin in "$QUERY_BIN" "$INDEXER_BIN" "$EXTRACTOR_BIN"; do
    if [[ ! -x "$bin" ]]; then
        echo "Error: missing executable: $bin"
        exit 1
    fi
done

echo "=== BetterSpotlight 48h Stress Harness ==="
echo "Duration: ${DURATION_SECONDS}s"
echo "Query:    $QUERY_BIN"
echo "Indexer:  $INDEXER_BIN"
echo "Extractor:$EXTRACTOR_BIN"

python3 - "$DURATION_SECONDS" "$QUERY_BIN" "$INDEXER_BIN" "$EXTRACTOR_BIN" \
    "$SAMPLE_INTERVAL" "$INTEGRITY_INTERVAL" <<'PY'
import json
import os
import random
import socket
import sqlite3
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

duration = int(sys.argv[1])
query_bin = sys.argv[2]
indexer_bin = sys.argv[3]
extractor_bin = sys.argv[4]
sample_interval = int(sys.argv[5])
integrity_interval = int(sys.argv[6])

ts = time.strftime("%Y%m%d_%H%M%S")
artifact_dir = Path(f"/tmp/bs_stress_48h_{ts}")
artifact_dir.mkdir(parents=True, exist_ok=True)
home_dir = artifact_dir / "home"
home_dir.mkdir(parents=True, exist_ok=True)
corpus_dir = artifact_dir / "corpus"
corpus_dir.mkdir(parents=True, exist_ok=True)

uid = os.getuid()
socket_dir = Path(f"/tmp/betterspotlight-{uid}")
query_sock = socket_dir / "query.sock"
indexer_sock = socket_dir / "indexer.sock"
extractor_sock = socket_dir / "extractor.sock"

db_path = home_dir / "Library" / "Application Support" / "betterspotlight" / "index.db"
db_path.parent.mkdir(parents=True, exist_ok=True)

env = os.environ.copy()
env["HOME"] = str(home_dir)
env["TMPDIR"] = str(artifact_dir / "tmp")
Path(env["TMPDIR"]).mkdir(parents=True, exist_ok=True)

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

services = {
    "query": subprocess.Popen([query_bin], stdout=service_logs["query"], stderr=subprocess.STDOUT, env=env),
    "indexer": subprocess.Popen([indexer_bin], stdout=service_logs["indexer"], stderr=subprocess.STDOUT, env=env),
    "extractor": subprocess.Popen([extractor_bin], stdout=service_logs["extractor"], stderr=subprocess.STDOUT, env=env),
}


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


for name, sock in (("query", query_sock), ("indexer", indexer_sock), ("extractor", extractor_sock)):
    if not wait_socket(sock):
        raise RuntimeError(f"{name} socket did not appear: {sock}")

rid = 1
start_indexing = rpc(indexer_sock, "startIndexing", {"roots": [str(corpus_dir)]}, rid=rid, timeout=30.0)
rid += 1
if start_indexing.get("type") == "error":
    raise RuntimeError(f"startIndexing failed: {start_indexing}")

seed_files = {
    "readme.txt": "betterspotlight stress test corpus\nquery indexing baseline\n",
    "notes.md": "meeting notes about deployment reliability and memory drift\n",
    "finance.txt": "credit report statement account transaction history\n",
}
for name, content in seed_files.items():
    (corpus_dir / name).write_text(content, encoding="utf-8")
rpc(indexer_sock, "reindexPath", {"path": str(corpus_dir)}, rid=rid, timeout=15.0)
rid += 1
time.sleep(1.0)

queries = [
    "credit report",
    "deployment reliability",
    "memory drift",
    "query service",
    "readme stress",
    "transaction history",
    "notes about deployment",
    "where is my finance file",
]

search_latencies_ms = []
errors = []
integrity_failures = 0
iterations = 0
last_integrity = 0.0
last_sample = 0.0
metrics_rows = []

begin = time.time()
while time.time() - begin < duration:
    iterations += 1
    now = time.time()

    for name, proc in services.items():
        code = proc.poll()
        if code is not None:
            errors.append(f"{name} exited unexpectedly with code {code}")
            raise RuntimeError(errors[-1])

    # Mutate corpus periodically.
    if iterations % 3 == 0:
        f = corpus_dir / f"dynamic_{iterations % 12}.txt"
        f.write_text(
            f"iteration={iterations}\nconcept=semantic_probe\nnoise={random.randint(1, 9999)}\n",
            encoding="utf-8",
        )
        try:
            rpc(indexer_sock, "reindexPath", {"path": str(f)}, rid=rid, timeout=10.0)
        except Exception as ex:
            errors.append(f"reindexPath failed for {f}: {ex}")
        rid += 1

    # Query burst.
    for q in queries:
        t0 = time.time()
        try:
            resp = rpc(query_sock, "search", {"query": q, "limit": 20, "debug": True}, rid=rid, timeout=20.0)
        except Exception as ex:
            # One retry for slow cold-start / transient socket backpressure.
            try:
                time.sleep(0.2)
                resp = rpc(query_sock, "search",
                           {"query": q, "limit": 20, "debug": True},
                           rid=rid,
                           timeout=30.0)
            except Exception as retry_ex:
                errors.append(f"search RPC failed for '{q}': {retry_ex}")
                rid += 1
                continue
        rid += 1
        dt = (time.time() - t0) * 1000.0
        search_latencies_ms.append(dt)
        if resp.get("type") == "error":
            errors.append(f"query error for '{q}': {resp}")

    if now - last_sample >= sample_interval:
        try:
            h = rpc(query_sock, "getHealthDetails", {"limit": 20, "offset": 0}, rid=rid, timeout=15.0)
            rid += 1
            if h.get("type") == "response":
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
            errors.append(f"getHealthDetails failed: {ex}")
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

summary = {
    "durationSeconds": int(time.time() - begin),
    "iterations": iterations,
    "queryCount": len(search_latencies_ms),
    "latencyAvgMs": round(sum(search_latencies_ms) / len(search_latencies_ms), 2) if search_latencies_ms else 0.0,
    "latencyP95Ms": round(sorted(search_latencies_ms)[int(max(0, len(search_latencies_ms) * 0.95 - 1))], 2)
    if search_latencies_ms else 0.0,
    "latencyMaxMs": round(max(search_latencies_ms), 2) if search_latencies_ms else 0.0,
    "integrityFailures": integrity_failures,
    "errors": errors,
    "artifactDir": str(artifact_dir),
    "dbPath": str(db_path),
}

with open(artifact_dir / "summary.json", "w", encoding="utf-8") as f:
    json.dump(summary, f, indent=2)
with open(artifact_dir / "metrics.json", "w", encoding="utf-8") as f:
    json.dump(metrics_rows, f, indent=2)

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
    except subprocess.TimeoutExpired:
        proc.kill()

for fp in service_logs.values():
    fp.close()

print(json.dumps(summary, indent=2))
if summary["errors"] or summary["integrityFailures"] > 0:
    raise SystemExit(2)
PY

echo "✓ stress_48h complete"
