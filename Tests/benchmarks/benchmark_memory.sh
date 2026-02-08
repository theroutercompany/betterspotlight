#!/bin/bash
# benchmark_memory.sh - Monitor memory usage during indexing
#
# Usage: ./benchmark_memory.sh [DURATION_SECONDS] [BINARY_PATH]
#   DURATION_SECONDS: How long to monitor (default: 60)
#   BINARY_PATH: Path to betterspotlight binary (default: ./build/betterspotlight)
#
# Measures:
#   - Peak RSS memory usage
#   - Average RSS memory usage
#   - Memory samples collected at 1-second intervals
#
# Requirements: bash, ps, awk

set -euo pipefail

DURATION="${1:-60}"
BINARY_PATH="${2:-./build/betterspotlight}"

echo "=== BetterSpotlight Memory Benchmark ==="
echo "Duration: ${DURATION}s"
echo "Binary: $BINARY_PATH"
echo ""

if [[ ! -f "$BINARY_PATH" ]]; then
	echo "Error: Binary not found at $BINARY_PATH"
	exit 1
fi

echo "Starting process..."
"$BINARY_PATH" &
PID=$!

trap "kill $PID 2>/dev/null || true" EXIT

sleep 1

declare -a SAMPLES
sample_count=0

echo "Monitoring memory for ${DURATION}s..."
for i in $(seq 1 "$DURATION"); do
	rss=$(ps -o rss= -p "$PID" 2>/dev/null || echo "0")
	SAMPLES+=("$rss")
	((sample_count++))
	sleep 1
done

if ! kill $PID 2>/dev/null; then
	true
fi

if [[ $sample_count -eq 0 ]]; then
	echo "Error: No memory samples collected"
	exit 1
fi

stats=$(printf '%s\n' "${SAMPLES[@]}" | awk '
BEGIN { min = 999999999; max = 0; sum = 0; count = 0 }
{
    val = $1
    if (val < min) min = val
    if (val > max) max = val
    sum += val
    count++
}
END {
    avg = sum / count
    printf "min=%d max=%d avg=%.0f count=%d\n", min, max, avg, count
}
')

min=$(echo "$stats" | awk '{print $1}' | cut -d= -f2)
max=$(echo "$stats" | awk '{print $2}' | cut -d= -f2)
avg=$(echo "$stats" | awk '{print $3}' | cut -d= -f2)
count=$(echo "$stats" | awk '{print $4}' | cut -d= -f2)

peak_mb=$(echo "scale=2; $max / 1024" | bc)
avg_mb=$(echo "scale=2; $avg / 1024" | bc)

echo "Results:"
echo "  Peak memory: ${peak_mb} MB"
echo "  Avg memory:  ${avg_mb} MB"
echo "  Samples:     ${count}"
echo ""
echo "✓ Benchmark complete"
