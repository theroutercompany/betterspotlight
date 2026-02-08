#!/bin/bash
# benchmark_search.sh - Benchmark query service search latency via IPC
#
# Usage: ./benchmark_search.sh
#
# Measures p50/p95/max latency across deterministic query corpus.

set -euo pipefail

QUERY_BIN="/Users/rexliu/betterspotlight/build/src/services/query/betterspotlight-query"

if [[ ! -x "$QUERY_BIN" ]]; then
  echo "Error: query service binary not found. Build first."
  exit 1
fi

echo "=== BetterSpotlight Query IPC Benchmark ==="

python3 - "$QUERY_BIN" <<'PY'
import json
import os
import socket
import statistics
import struct
import subprocess
import sys
import time

query_bin = sys.argv[1]
uid = os.getuid()
query_sock = f"/tmp/betterspotlight-{uid}/query.sock"
query_log = "/tmp/bs_benchmark_query_only.log"

queries = [
    "test",
    "README",
    "main.cpp",
    "useEffect",
    "kubectl apply",
    "settings",
    "credentials",
    "database schema",
    "performance optimization",
    "benchmark_token",
]


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
    msg = {"type": "request", "id": rid, "method": method, "params": params}
    payload = json.dumps(msg, separators=(",", ":")).encode("utf-8")
    framed = struct.pack(">I", len(payload)) + payload

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(sock_path)
    s.sendall(framed)

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


try:
    os.remove(query_sock)
except FileNotFoundError:
    pass

proc = subprocess.Popen([query_bin], stdout=open(query_log, "w"), stderr=subprocess.STDOUT)
if not wait_socket(query_sock):
    raise RuntimeError("query socket did not appear")

latencies = []
result_counts = []
rid = 1

for _ in range(5):
    for q in queries:
        t0 = time.time()
        resp = rpc(query_sock, "search", {"query": q, "limit": 20}, rid=rid, timeout=10.0)
        rid += 1
        dt = (time.time() - t0) * 1000.0

        if resp.get("type") == "response":
            latencies.append(dt)
            results = resp.get("result", {}).get("results", [])
            result_counts.append(len(results))

if latencies:
    p50 = statistics.median(latencies)
    p95 = statistics.quantiles(latencies, n=20)[18] if len(latencies) >= 20 else max(latencies)
    print("Results:")
    print(f"  Query count: {len(latencies)}")
    print(f"  p50 latency: {p50:.2f} ms")
    print(f"  p95 latency: {p95:.2f} ms")
    print(f"  max latency: {max(latencies):.2f} ms")
    print(f"  avg results: {statistics.mean(result_counts):.2f}")
else:
    print("No successful query responses received.")

try:
    rpc(query_sock, "shutdown", {}, rid=rid, timeout=5.0)
except Exception:
    pass

try:
    proc.wait(timeout=5.0)
except subprocess.TimeoutExpired:
    proc.kill()
PY

echo "✓ Benchmark complete"
