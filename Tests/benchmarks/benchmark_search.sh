#!/bin/bash
# benchmark_search.sh - Benchmark FTS5 search query performance
#
# Usage: ./benchmark_search.sh [DB_PATH]
#   DB_PATH: Path to index.db (default: ~/Library/Application\ Support/betterspotlight/index.db)
#
# Measures:
#   - Query latency (min/max/avg/p95) across 100 search queries
#   - Multiple query patterns: short, medium, long, special characters
#
# Requirements: sqlite3, awk, bc

set -euo pipefail

DB_PATH="${1:-${HOME}/Library/Application\ Support/betterspotlight/index.db}"

# Verify database exists
if [[ ! -f "$DB_PATH" ]]; then
	echo "Error: Database not found at $DB_PATH"
	exit 1
fi

# Array of test queries (short, medium, long, special)
QUERIES=(
	"test"
	"hello"
	"function"
	"import"
	"README"
	"config"
	"main"
	"swift"
	"error"
	"debug"
	"hello world"
	"search function"
	"file system"
	"index database"
	"query service"
	"memory management"
	"performance optimization"
	"concurrent processing"
	"error handling"
	"data structure"
)

echo "=== BetterSpotlight Search Benchmark ==="
echo "Database: $DB_PATH"
echo "Query count: 100 (5 iterations × 20 queries)"
echo ""

# Collect latencies in array
declare -a LATENCIES

# Run 5 iterations of all queries
for iteration in {1..5}; do
	for query in "${QUERIES[@]}"; do
		# Time the FTS5 query
		start_ns=$(date +%s%N)

		# Execute FTS5 search (count results)
		sqlite3 "$DB_PATH" "SELECT COUNT(*) FROM items_fts WHERE items_fts MATCH '$query' LIMIT 100;" 2>/dev/null || true

		end_ns=$(date +%s%N)

		# Calculate latency in milliseconds
		latency_ms=$(echo "scale=3; ($end_ns - $start_ns) / 1000000" | bc)
		LATENCIES+=("$latency_ms")
	done
done

# Calculate statistics using awk
stats=$(printf '%s\n' "${LATENCIES[@]}" | awk '
BEGIN { min = 999999; max = 0; sum = 0; count = 0 }
{
    val = $1
    if (val < min) min = val
    if (val > max) max = val
    sum += val
    count++
    latencies[count] = val
}
END {
    avg = sum / count
    
    # Sort for percentile calculation
    for (i = 1; i <= count; i++) {
        for (j = i + 1; j <= count; j++) {
            if (latencies[i] > latencies[j]) {
                temp = latencies[i]
                latencies[i] = latencies[j]
                latencies[j] = temp
            }
        }
    }
    
    # P95 (95th percentile)
    p95_idx = int(count * 0.95)
    if (p95_idx < 1) p95_idx = 1
    p95 = latencies[p95_idx]
    
    printf "min=%.3f max=%.3f avg=%.3f p95=%.3f count=%d\n", min, max, avg, p95, count
}
')

# Parse and display results
min=$(echo "$stats" | awk '{print $1}' | cut -d= -f2)
max=$(echo "$stats" | awk '{print $2}' | cut -d= -f2)
avg=$(echo "$stats" | awk '{print $3}' | cut -d= -f2)
p95=$(echo "$stats" | awk '{print $4}' | cut -d= -f2)
count=$(echo "$stats" | awk '{print $5}' | cut -d= -f2)

echo "Results:"
echo "  Min latency:  ${min} ms"
echo "  Max latency:  ${max} ms"
echo "  Avg latency:  ${avg} ms"
echo "  P95 latency:  ${p95} ms"
echo "  Total queries: ${count}"
echo ""
echo "✓ Benchmark complete"
