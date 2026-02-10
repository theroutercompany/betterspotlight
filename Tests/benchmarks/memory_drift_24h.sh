#!/bin/bash
# memory_drift_24h.sh - 24-hour idle memory drift check for services
#
# Usage:
#   ./memory_drift_24h.sh [duration_seconds]
#
# Optional env:
#   BS_MEM_DURATION_SECONDS (default 86400)
#   BS_MEM_SAMPLE_INTERVAL (default 60)
#   BS_MEM_DRIFT_LIMIT_MB (default 10)
#   BS_MEM_QUERY_BIN / BS_MEM_INDEXER_BIN / BS_MEM_EXTRACTOR_BIN
#   BS_MEM_ARTIFACT_DIR (optional explicit artifact directory)
#
# Artifacts are written to /tmp/bs_memory_drift_<timestamp>/

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEFAULT_DURATION=$((24 * 3600))
DURATION_SECONDS="${BS_MEM_DURATION_SECONDS:-${1:-$DEFAULT_DURATION}}"
SAMPLE_INTERVAL="${BS_MEM_SAMPLE_INTERVAL:-60}"
DRIFT_LIMIT_MB="${BS_MEM_DRIFT_LIMIT_MB:-10}"
QUERY_BIN="${BS_MEM_QUERY_BIN:-$ROOT_DIR/build/src/services/query/betterspotlight-query}"
INDEXER_BIN="${BS_MEM_INDEXER_BIN:-$ROOT_DIR/build/src/services/indexer/betterspotlight-indexer}"
EXTRACTOR_BIN="${BS_MEM_EXTRACTOR_BIN:-$ROOT_DIR/build/src/services/extractor/betterspotlight-extractor}"
ARTIFACT_DIR_OVERRIDE="${BS_MEM_ARTIFACT_DIR:-}"

for bin in "$QUERY_BIN" "$INDEXER_BIN" "$EXTRACTOR_BIN"; do
    if [[ ! -x "$bin" ]]; then
        echo "Error: missing executable: $bin"
        exit 1
    fi
done

echo "=== BetterSpotlight 24h Memory Drift Harness ==="
echo "Duration: ${DURATION_SECONDS}s"
echo "Sample interval: ${SAMPLE_INTERVAL}s"
echo "Drift limit: ${DRIFT_LIMIT_MB} MB"

python3 - "$DURATION_SECONDS" "$SAMPLE_INTERVAL" "$DRIFT_LIMIT_MB" \
    "$QUERY_BIN" "$INDEXER_BIN" "$EXTRACTOR_BIN" "$ARTIFACT_DIR_OVERRIDE" <<'PY'
import json
import os
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

duration = int(sys.argv[1])
sample_interval = int(sys.argv[2])
drift_limit_mb = float(sys.argv[3])
query_bin = sys.argv[4]
indexer_bin = sys.argv[5]
extractor_bin = sys.argv[6]
artifact_dir_override = sys.argv[7].strip()

ts = time.strftime("%Y%m%d_%H%M%S")
if artifact_dir_override:
    artifact_dir = Path(artifact_dir_override).expanduser()
else:
    artifact_dir = Path(f"/tmp/bs_memory_drift_{ts}")
artifact_dir.mkdir(parents=True, exist_ok=True)
home_dir = artifact_dir / "home"
home_dir.mkdir(parents=True, exist_ok=True)
corpus_dir = artifact_dir / "corpus"
corpus_dir.mkdir(parents=True, exist_ok=True)
(corpus_dir / "idle.txt").write_text("idle drift baseline corpus\n", encoding="utf-8")

uid = os.getuid()
env = os.environ.copy()
env["HOME"] = str(home_dir)
env["TMPDIR"] = str(artifact_dir / "tmp")
env["BETTERSPOTLIGHT_SOCKET_DIR"] = str(artifact_dir / "sockets")
Path(env["TMPDIR"]).mkdir(parents=True, exist_ok=True)

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

logs = {
    "query": open(artifact_dir / "query.log", "w", encoding="utf-8"),
    "indexer": open(artifact_dir / "indexer.log", "w", encoding="utf-8"),
    "extractor": open(artifact_dir / "extractor.log", "w", encoding="utf-8"),
}
services = {
    "query": subprocess.Popen([query_bin], stdout=logs["query"], stderr=subprocess.STDOUT, env=env),
    "indexer": subprocess.Popen([indexer_bin], stdout=logs["indexer"], stderr=subprocess.STDOUT, env=env),
    "extractor": subprocess.Popen([extractor_bin], stdout=logs["extractor"], stderr=subprocess.STDOUT, env=env),
}


def wait_socket(path: Path, timeout: float = 30.0) -> bool:
    end = time.time() + timeout
    while time.time() < end:
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
        raise RuntimeError("short header")
    n = struct.unpack(">I", hdr)[0]
    body = b""
    while len(body) < n:
        chunk = s.recv(n - len(body))
        if not chunk:
            break
        body += chunk
    s.close()
    if len(body) != n:
        raise RuntimeError("short body")
    return json.loads(body.decode("utf-8"))

def rpc_with_retry(sock_path: Path, method: str, params=None, rid: int = 1,
                   timeout: float = 10.0, attempts: int = 3, delay: float = 0.25):
    last_error = None
    for _ in range(max(1, attempts)):
        try:
            return rpc(sock_path, method, params=params, rid=rid, timeout=timeout)
        except Exception as ex:
            last_error = ex
            time.sleep(delay)
    raise last_error


for sock in (query_sock, indexer_sock, extractor_sock):
    if not wait_socket(sock):
        raise RuntimeError(f"socket unavailable: {sock}")

rid = 1
rpc_with_retry(indexer_sock, "startIndexing", {"roots": [str(corpus_dir)]},
               rid=rid, timeout=30.0, attempts=4)
rid += 1
rpc_with_retry(indexer_sock, "reindexPath", {"path": str(corpus_dir)},
               rid=rid, timeout=20.0, attempts=4)
rid += 1
rpc_with_retry(query_sock, "search", {"query": "idle drift baseline", "limit": 5},
               rid=rid, timeout=20.0, attempts=4)
rid += 1

samples = {"query": [], "indexer": [], "extractor": []}
start = time.time()
while time.time() - start < duration:
    for name, proc in services.items():
        if proc.poll() is not None:
            raise RuntimeError(f"{name} exited unexpectedly with code {proc.returncode}")
        rss = 0
        out = subprocess.check_output(["ps", "-o", "rss=", "-p", str(proc.pid)], text=True).strip()
        if out:
            rss = int(out)
        samples[name].append({"t": int(time.time()), "rssKb": rss})
    time.sleep(sample_interval)

summary = {
    "durationSeconds": int(time.time() - start),
    "sampleIntervalSeconds": sample_interval,
    "driftLimitMb": drift_limit_mb,
    "artifactDir": str(artifact_dir),
    "services": {},
}

failed = False
for name, rows in samples.items():
    if not rows:
        continue
    first = rows[0]["rssKb"]
    last = rows[-1]["rssKb"]
    peak = max(r["rssKb"] for r in rows)
    drift_mb = (last - first) / 1024.0
    peak_mb = peak / 1024.0
    summary["services"][name] = {
        "samples": len(rows),
        "firstRssKb": first,
        "lastRssKb": last,
        "peakRssKb": peak,
        "driftMb": round(drift_mb, 3),
        "peakMb": round(peak_mb, 3),
        "pass": drift_mb <= drift_limit_mb,
    }
    if drift_mb > drift_limit_mb:
        failed = True

with open(artifact_dir / "memory_samples.json", "w", encoding="utf-8") as f:
    json.dump(samples, f, indent=2)
with open(artifact_dir / "summary.json", "w", encoding="utf-8") as f:
    json.dump(summary, f, indent=2)

for sock, method in ((query_sock, "shutdown"), (indexer_sock, "shutdown"), (extractor_sock, "shutdown")):
    try:
        rpc_with_retry(sock, method, {}, rid=rid, timeout=3.0, attempts=2, delay=0.1)
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
for fp in logs.values():
    fp.close()

print(json.dumps(summary, indent=2))
if failed:
    raise SystemExit(2)
PY

echo "✓ memory_drift_24h complete"
