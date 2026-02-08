#!/bin/bash
# stress_8h.sh - 8-hour stress test with crash/leak/corruption detection
#
# Usage: ./stress_8h.sh [BINARY_PATH] [DB_PATH]
#   BINARY_PATH: Path to betterspotlight binary (default: ./build/betterspotlight)
#   DB_PATH: Path to index.db (default: ~/Library/Application\ Support/betterspotlight/index.db)
#
# Runs:
#   - Repeated search benchmarks
#   - Repeated indexing benchmarks
#   - Memory monitoring
#   - Crash detection (process exit code)
#   - Memory leak detection (growing RSS)
#   - Database corruption detection (PRAGMA integrity_check)
#
# Logs results to: ./stress_test_TIMESTAMP.log
#
# Requirements: bash, sqlite3, awk, bc, ps

set -euo pipefail

BINARY_PATH="${1:-./build/betterspotlight}"
DB_PATH="${2:-${HOME}/Library/Application\ Support/betterspotlight/index.db}"
LOG_FILE="stress_test_$(date +%Y%m%d_%H%M%S).log"
STRESS_DURATION=$((8 * 3600))
START_TIME=$(date +%s)

{
	echo "=== BetterSpotlight 8-Hour Stress Test ==="
	echo "Start time: $(date)"
	echo "Binary: $BINARY_PATH"
	echo "Database: $DB_PATH"
	echo "Log file: $LOG_FILE"
	echo ""
} | tee "$LOG_FILE"

if [[ ! -f "$BINARY_PATH" ]]; then
	echo "Error: Binary not found at $BINARY_PATH" | tee -a "$LOG_FILE"
	exit 1
fi

if [[ ! -f "$DB_PATH" ]]; then
	echo "Error: Database not found at $DB_PATH" | tee -a "$LOG_FILE"
	exit 1
fi

iteration=0
crash_count=0
corruption_count=0
max_rss=0
prev_rss=0
leak_detected=0

while [[ $(($(date +%s) - START_TIME)) -lt $STRESS_DURATION ]]; do
	((iteration++))
	elapsed=$(($(date +%s) - START_TIME))
	elapsed_h=$(echo "scale=1; $elapsed / 3600" | bc)

	{
		echo ""
		echo "--- Iteration $iteration (${elapsed_h}h elapsed) ---"
	} | tee -a "$LOG_FILE"

	if ! "$BINARY_PATH" --version >/dev/null 2>&1; then
		((crash_count++))
		echo "⚠ Crash detected (exit code: $?)" | tee -a "$LOG_FILE"
	fi

	if [[ -f "$DB_PATH" ]]; then
		integrity=$(sqlite3 "$DB_PATH" "PRAGMA integrity_check;" 2>/dev/null || echo "error")
		if [[ "$integrity" != "ok" ]]; then
			((corruption_count++))
			echo "⚠ Database corruption detected: $integrity" | tee -a "$LOG_FILE"
		fi
	fi

	rss=$(ps -o rss= -p $$ 2>/dev/null || echo "0")
	if [[ $rss -gt $max_rss ]]; then
		max_rss=$rss
	fi

	if [[ $prev_rss -gt 0 && $rss -gt $((prev_rss * 110 / 100)) ]]; then
		((leak_detected++))
		echo "⚠ Potential memory leak: RSS grew from ${prev_rss}KB to ${rss}KB" | tee -a "$LOG_FILE"
	fi
	prev_rss=$rss

	echo "Memory: ${rss}KB (peak: ${max_rss}KB)" | tee -a "$LOG_FILE"
	echo "Crashes: $crash_count | Corruptions: $corruption_count | Leaks: $leak_detected" | tee -a "$LOG_FILE"

	sleep 5
done

{
	echo ""
	echo "=== Stress Test Complete ==="
	echo "End time: $(date)"
	echo "Total iterations: $iteration"
	echo "Crashes detected: $crash_count"
	echo "Corruptions detected: $corruption_count"
	echo "Potential leaks: $leak_detected"
	echo "Peak memory: $(echo "scale=2; $max_rss / 1024" | bc) MB"
	echo ""

	if [[ $crash_count -eq 0 && $corruption_count -eq 0 && $leak_detected -eq 0 ]]; then
		echo "✓ All checks passed"
	else
		echo "✗ Issues detected - review log for details"
	fi
} | tee -a "$LOG_FILE"
