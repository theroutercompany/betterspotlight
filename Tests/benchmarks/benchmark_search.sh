#!/bin/bash
# benchmark_search.sh - Benchmark query service search latency via IPC
#
# Usage: ./benchmark_search.sh
#
# Measures p50/p95/max latency across deterministic query corpus.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BS_BENCHMARK_BUILD_DIR:-${ROOT_DIR}/build}"
QUERY_BIN="${BS_QUERY_BIN:-${BUILD_DIR}/src/services/query/betterspotlight-query}"

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
duplicate_rates = []
strict_hits = []
relaxed_hits = []
semantic_candidates = []
rewrites = 0
rid = 1

for _ in range(5):
    for q in queries:
        t0 = time.time()
        resp = rpc(query_sock, "search", {
            "query": q,
            "limit": 20,
            "queryMode": "auto",
            "debug": True
        }, rid=rid, timeout=10.0)
        rid += 1
        dt = (time.time() - t0) * 1000.0

        if resp.get("type") == "response":
            latencies.append(dt)
            results = resp.get("result", {}).get("results", [])
            result_counts.append(len(results))
            top10 = results[:10]
            paths = [r.get("path", "") for r in top10 if r.get("path")]
            unique = len(set(paths))
            dup_rate = 0.0 if not paths else (1.0 - (unique / len(paths)))
            duplicate_rates.append(dup_rate)
            dbg = resp.get("result", {}).get("debugInfo", {})
            strict_hits.append(int(dbg.get("lexicalStrictHits", 0)))
            relaxed_hits.append(int(dbg.get("lexicalRelaxedHits", 0)))
            semantic_candidates.append(int(dbg.get("semanticCandidates", 0)))
            if dbg.get("rewrittenQuery"):
                rewrites += 1

health = {}
try:
    health_resp = rpc(query_sock, "getHealth", {}, rid=rid, timeout=10.0)
    rid += 1
    if health_resp.get("type") == "response":
        health = health_resp.get("result", {}).get("indexHealth", {})
except Exception:
    health = {}

if latencies:
    p50 = statistics.median(latencies)
    p95 = statistics.quantiles(latencies, n=20)[18] if len(latencies) >= 20 else max(latencies)
    print("Results:")
    print(f"  Query count: {len(latencies)}")
    print(f"  p50 latency: {p50:.2f} ms")
    print(f"  p95 latency: {p95:.2f} ms")
    print(f"  max latency: {max(latencies):.2f} ms")
    print(f"  avg results: {statistics.mean(result_counts):.2f}")
    print(f"  avg top-10 duplicate rate: {statistics.mean(duplicate_rates) * 100.0:.2f}%")
    print(f"  avg strict lexical hits: {statistics.mean(strict_hits):.2f}")
    print(f"  avg relaxed lexical hits: {statistics.mean(relaxed_hits):.2f}")
    print(f"  avg semantic candidates: {statistics.mean(semantic_candidates):.2f}")
    print(f"  rewritten query count: {rewrites}")

    report = {
        "queryCount": len(latencies),
        "p50Ms": round(p50, 2),
        "p95Ms": round(p95, 2),
        "maxMs": round(max(latencies), 2),
        "avgResults": round(statistics.mean(result_counts), 2),
        "avgTop10DuplicateRatePct": round(statistics.mean(duplicate_rates) * 100.0, 2),
        "avgStrictLexicalHits": round(statistics.mean(strict_hits), 2) if strict_hits else 0.0,
        "avgRelaxedLexicalHits": round(statistics.mean(relaxed_hits), 2) if relaxed_hits else 0.0,
        "avgSemanticCandidates": round(statistics.mean(semantic_candidates), 2) if semantic_candidates else 0.0,
        "rewrittenQueryCount": rewrites,
        "indexHealth": {
            "totalIndexedItems": health.get("totalIndexedItems"),
            "totalChunks": health.get("totalChunks"),
            "totalEmbeddedVectors": health.get("totalEmbeddedVectors"),
            "contentCoveragePct": health.get("contentCoveragePct"),
            "overallStatus": health.get("overallStatus"),
        },
    }
    report_path = "/tmp/bs_search_benchmark_report.json"
    with open(report_path, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)
    print(f"  report: {report_path}")
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
