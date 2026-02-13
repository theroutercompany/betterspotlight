#!/bin/bash
# benchmark_indexing.sh - Deterministic indexing benchmark via IPC services
#
# Usage: ./benchmark_indexing.sh [FILE_COUNT]
#
# Reports:
#   - Total indexing elapsed time
#   - Indexing throughput (files/sec)
#   - Max observed preparing concurrency
#   - Query latency p50/p95/max while indexing is active

set -euo pipefail

FILE_COUNT="${1:-1000}"
TEMP_DIR=$(mktemp -d)
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BS_BENCHMARK_BUILD_DIR:-${ROOT_DIR}/build}"
INDEXER_BIN="${BS_INDEXER_BIN:-${BUILD_DIR}/src/services/indexer/betterspotlight-indexer}"
QUERY_BIN="${BS_QUERY_BIN:-${BUILD_DIR}/src/services/query/betterspotlight-query}"

trap "rm -rf '$TEMP_DIR'" EXIT

echo "=== BetterSpotlight Indexing IPC Benchmark ==="
echo "File count: $FILE_COUNT"
echo "Temp directory: $TEMP_DIR"

if [[ ! -x "$INDEXER_BIN" || ! -x "$QUERY_BIN" ]]; then
  echo "Error: service binaries not found. Build first."
  exit 1
fi

echo "Creating deterministic benchmark corpus..."
for i in $(seq 1 "$FILE_COUNT"); do
  cat >"$TEMP_DIR/file_${i}.txt" <<TXT
benchmark_token file_$i
This file is used for deterministic indexing throughput measurement.
$(printf "alpha beta gamma delta epsilon %.0s" $(seq 1 80))
TXT
done
echo "✓ Created $FILE_COUNT files"

python3 - "$TEMP_DIR" "$INDEXER_BIN" "$QUERY_BIN" "$FILE_COUNT" <<'PY'
import json
import os
import socket
import statistics
import struct
import subprocess
import sys
import threading
import time

root, indexer_bin, query_bin, expected = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
uid = os.getuid()
indexer_sock = f"/tmp/betterspotlight-{uid}/indexer.sock"
query_sock = f"/tmp/betterspotlight-{uid}/query.sock"
indexer_log = "/tmp/bs_benchmark_indexer.log"
query_log = "/tmp/bs_benchmark_query.log"


def wait_socket(path, timeout=20.0):
    start = time.time()
    while time.time() - start < timeout:
        if os.path.exists(path):
            return True
        time.sleep(0.05)
    return False


def rpc(sock_path, method, params=None, rid=1, timeout=10.0):
    if params is None:
        params = {}
    msg = {
        "type": "request",
        "id": rid,
        "method": method,
        "params": params,
    }
    payload = json.dumps(msg, separators=(",", ":")).encode("utf-8")
    framed = struct.pack(">I", len(payload)) + payload

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(sock_path)
    s.sendall(framed)

    header = s.recv(4)
    if len(header) != 4:
        raise RuntimeError("short IPC header")
    n = struct.unpack(">I", header)[0]

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


# Start fresh service instances for deterministic benchmark
for sock in (indexer_sock, query_sock):
    try:
        os.remove(sock)
    except FileNotFoundError:
        pass

idx = subprocess.Popen([indexer_bin], stdout=open(indexer_log, "w"), stderr=subprocess.STDOUT)
qry = subprocess.Popen([query_bin], stdout=open(query_log, "w"), stderr=subprocess.STDOUT)

if not wait_socket(indexer_sock):
    raise RuntimeError("indexer socket did not appear")
if not wait_socket(query_sock):
    raise RuntimeError("query socket did not appear")

rid = 1
rid_lock = threading.Lock()


def next_id():
    global rid
    with rid_lock:
        current = rid
        rid += 1
        return current


start_resp = rpc(indexer_sock, "startIndexing", {"roots": [root]}, rid=next_id(), timeout=30.0)
if start_resp.get("type") == "error":
    raise RuntimeError(f"startIndexing failed: {start_resp}")

search_latencies_ms = []
status = {
    "pending": 0,
    "processing": 0,
    "max_preparing": 0,
    "max_writing": 0,
    "max_processing": 0,
}
status_lock = threading.Lock()
stop_status = threading.Event()
start = time.time()


def sample_queue_status():
    # High-frequency status probe to capture short-lived prep bursts.
    while not stop_status.is_set():
        try:
            qresp = rpc(indexer_sock, "getQueueStatus", {}, rid=next_id(), timeout=10.0)
            if qresp.get("type") == "response":
                result = qresp.get("result", {})
                pending = int(result.get("pending", 0))
                processing = int(result.get("processing", 0))
                preparing = int(result.get("preparing", 0))
                writing = int(result.get("writing", 0))
                with status_lock:
                    status["pending"] = pending
                    status["processing"] = processing
                    status["max_preparing"] = max(status["max_preparing"], preparing)
                    status["max_writing"] = max(status["max_writing"], writing)
                    status["max_processing"] = max(status["max_processing"], processing)
        except Exception:
            # Service may be shutting down; ignore transient socket errors.
            pass
        time.sleep(0.01)


sampler = threading.Thread(target=sample_queue_status, daemon=True)
sampler.start()

while True:
    s0 = time.time()
    try:
        sresp = rpc(query_sock, "search", {"query": "benchmark_token", "limit": 20},
                    rid=next_id(), timeout=10.0)
        if sresp.get("type") != "error":
            search_latencies_ms.append((time.time() - s0) * 1000.0)
    except Exception:
        pass

    with status_lock:
        pending = status["pending"]
        processing = status["processing"]

    if pending == 0 and processing == 0 and (time.time() - start) > 0.5:
        break

    if time.time() - start > 300:
        print("WARN: benchmark timeout reached", file=sys.stderr)
        break

    time.sleep(0.1)

stop_status.set()
sampler.join(timeout=1.0)
with status_lock:
    max_preparing = status["max_preparing"]
    max_writing = status["max_writing"]
    max_processing = status["max_processing"]

elapsed = time.time() - start
throughput = float(expected) / elapsed if elapsed > 0 else 0.0

if search_latencies_ms:
    p50 = statistics.median(search_latencies_ms)
    p95 = statistics.quantiles(search_latencies_ms, n=20)[18] if len(search_latencies_ms) >= 20 else max(search_latencies_ms)
    mx = max(search_latencies_ms)
else:
    p50 = p95 = mx = 0.0

print("Results:")
print(f"  Elapsed time: {elapsed:.2f}s")
print(f"  Throughput: {throughput:.2f} files/second")
print(f"  Max preparing: {max_preparing}")
print(f"  Max writing: {max_writing}")
print(f"  Max processing: {max_processing}")
print(f"  Query latency p50: {p50:.2f} ms")
print(f"  Query latency p95: {p95:.2f} ms")
print(f"  Query latency max: {mx:.2f} ms")

# Shutdown benchmark service processes.
try:
    rpc(indexer_sock, "shutdown", {}, rid=next_id(), timeout=5.0)
except Exception:
    pass
try:
    rpc(query_sock, "shutdown", {}, rid=next_id(), timeout=5.0)
except Exception:
    pass

for proc in (idx, qry):
    try:
        proc.wait(timeout=5.0)
    except subprocess.TimeoutExpired:
        proc.kill()
PY

echo "✓ Benchmark complete"
