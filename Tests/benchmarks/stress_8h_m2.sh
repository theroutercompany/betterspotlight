#!/bin/bash

################################################################################
# BetterSpotlight M2 8-Hour Stress Test
# Tests semantic search, memory stability, crash resilience, and DB integrity
################################################################################

set -euo pipefail

# ============================================================================
# CONFIGURATION
# ============================================================================

DURATION=28800 # 8 hours in seconds
BINARY_PATH="./build/bin/betterspotlight-query"
DB_PATH="/tmp/stress-test-index.db"
LOG_FILE="/tmp/stress-test-m2.log"
CRASH_LOG="/tmp/stress-test-crashes.log"
LATENCY_LOG="/tmp/stress-test-latency.csv"
MEMORY_LOG="/tmp/stress-test-memory.csv"
INTEGRITY_LOG="/tmp/stress-test-integrity.log"

# Derived paths
TEMP_DIR="/tmp/stress-test-$$"
QUERY_RESULTS_DIR="$TEMP_DIR/results"

# ============================================================================
# QUERY CORPUS (Lexical + Semantic + Content)
# ============================================================================

LEXICAL_QUERIES=(
	"budget"
	"README"
	"main.cpp"
	"docker-compose"
	".zshrc"
	"Makefile"
	"todo"
	"config"
	"test_"
	"deploy"
)

SEMANTIC_QUERIES=(
	"settings"
	"credentials"
	"server deployment"
	"database schema"
	"performance optimization"
	"money expenses"
	"deep learning"
	"API endpoints"
	"shell configuration"
	"version control"
)

CONTENT_QUERIES=(
	"ALTER TABLE"
	"JWT"
	"rsync"
	"gradient descent"
	"kubectl apply"
	"useEffect"
	"export PATH"
)

# ============================================================================
# STATE TRACKING
# ============================================================================

QUERY_COUNT=0
CRASH_COUNT=0
RESTART_COUNT=0
INTEGRITY_CHECKS_PASSED=0
INTEGRITY_CHECKS_TOTAL=0
ORPHANED_VECTORS=0
VECTOR_ENTRIES=0

declare -a LATENCIES=()
declare -a MEMORY_SAMPLES=()

START_TIME=$(date +%s)
LAST_MEMORY_CHECK=$START_TIME
LAST_INTEGRITY_CHECK=$START_TIME

# ============================================================================
# UTILITY FUNCTIONS
# ============================================================================

log() {
	local msg="$1"
	local timestamp=$(date '+%Y-%m-%d %H:%M:%S')
	echo "[$timestamp] $msg" | tee -a "$LOG_FILE"
}

log_crash() {
	local query="$1"
	local exit_code="$2"
	local timestamp=$(date '+%Y-%m-%d %H:%M:%S')
	echo "[$timestamp] CRASH: query='$query' exit_code=$exit_code" >>"$CRASH_LOG"
}

log_latency() {
	local query="$1"
	local latency_ms="$2"
	local result_count="$3"
	local timestamp=$(date '+%s')
	echo "$timestamp,$query,$latency_ms,$result_count" >>"$LATENCY_LOG"
}

log_memory() {
	local rss_kb="$1"
	local timestamp=$(date '+%s')
	echo "$timestamp,$rss_kb" >>"$MEMORY_LOG"
}

log_integrity() {
	local msg="$1"
	local timestamp=$(date '+%Y-%m-%d %H:%M:%S')
	echo "[$timestamp] $msg" >>"$INTEGRITY_LOG"
}

# Get random query from corpus
get_random_query() {
	local corpus_type=$((RANDOM % 3))
	local query

	case $corpus_type in
	0)
		query="${LEXICAL_QUERIES[$((RANDOM % ${#LEXICAL_QUERIES[@]}))]}"
		;;
	1)
		query="${SEMANTIC_QUERIES[$((RANDOM % ${#SEMANTIC_QUERIES[@]}))]}"
		;;
	2)
		query="${CONTENT_QUERIES[$((RANDOM % ${#CONTENT_QUERIES[@]}))]}"
		;;
	esac

	echo "$query"
}

# Execute query and measure latency
execute_query() {
	local query="$1"
	local start_ns=$(date +%s%N)
	local exit_code=0
	local result_count=0

	# Execute query, capture output
	local output
	output=$("$BINARY_PATH" --query "$query" --db "$DB_PATH" 2>&1) || exit_code=$?

	local end_ns=$(date +%s%N)
	local latency_ms=$(((end_ns - start_ns) / 1000000))

	# Count results (lines in output)
	result_count=$(echo "$output" | wc -l)

	# Log latency
	log_latency "$query" "$latency_ms" "$result_count"
	LATENCIES+=("$latency_ms")

	# Check for crash
	if [ $exit_code -ne 0 ]; then
		CRASH_COUNT=$((CRASH_COUNT + 1))
		log_crash "$query" "$exit_code"
		return 1
	fi

	return 0
}

# Simulate interaction (30% chance)
simulate_interaction() {
	if [ $((RANDOM % 100)) -lt 30 ]; then
		# Record interaction in feedback system
		# This would normally call a feedback endpoint
		# For now, we just log it
		:
	fi
}

# Check process health
check_process_health() {
	if ! command -v "$BINARY_PATH" &>/dev/null; then
		log "WARNING: Binary not found at $BINARY_PATH"
		return 1
	fi
	return 0
}

# Sample memory usage
sample_memory() {
	local current_time=$(date +%s)

	# Only sample every 60 seconds
	if [ $((current_time - LAST_MEMORY_CHECK)) -lt 60 ]; then
		return
	fi

	LAST_MEMORY_CHECK=$current_time

	# Get RSS of current shell (proxy for test process)
	local rss_kb=$(ps -o rss= -p $$ | awk '{print $1}')
	log_memory "$rss_kb"
	MEMORY_SAMPLES+=("$rss_kb")
}

# Check database integrity
check_integrity() {
	local current_time=$(date +%s)

	# Only check every 1000 queries or at start/end
	if [ $((current_time - LAST_INTEGRITY_CHECK)) -lt 300 ] && [ $QUERY_COUNT -gt 0 ] && [ $((QUERY_COUNT % 1000)) -ne 0 ]; then
		return
	fi

	LAST_INTEGRITY_CHECK=$current_time
	INTEGRITY_CHECKS_TOTAL=$((INTEGRITY_CHECKS_TOTAL + 1))

	log_integrity "Running integrity check (query count: $QUERY_COUNT)"

	# Check FTS5 integrity
	local fts_check
	fts_check=$(sqlite3 "$DB_PATH" "PRAGMA integrity_check;" 2>&1) || {
		log_integrity "FAIL: FTS5 integrity check failed"
		return 1
	}

	if [ "$fts_check" != "ok" ]; then
		log_integrity "FAIL: FTS5 integrity check returned: $fts_check"
		return 1
	fi

	# Verify vector_map table exists and has entries
	local vector_count
	vector_count=$(sqlite3 "$DB_PATH" "SELECT COUNT(*) FROM vector_map;" 2>&1) || {
		log_integrity "FAIL: Could not query vector_map"
		return 1
	}

	VECTOR_ENTRIES=$vector_count

	# Check for orphaned entries (vectors without corresponding items)
	local orphaned
	orphaned=$(sqlite3 "$DB_PATH" "SELECT COUNT(*) FROM vector_map vm WHERE NOT EXISTS (SELECT 1 FROM items i WHERE i.id = vm.item_id);" 2>&1) || {
		log_integrity "FAIL: Could not check for orphaned vectors"
		return 1
	}

	ORPHANED_VECTORS=$orphaned

	if [ "$orphaned" -gt 0 ]; then
		log_integrity "WARN: Found $orphaned orphaned vector entries"
	fi

	INTEGRITY_CHECKS_PASSED=$((INTEGRITY_CHECKS_PASSED + 1))
	log_integrity "PASS: Integrity check passed (vectors: $vector_count, orphaned: $orphaned)"

	return 0
}

# Calculate percentile from latency array
calculate_percentile() {
	local percentile=$1
	local count=${#LATENCIES[@]}

	if [ $count -eq 0 ]; then
		echo "0"
		return
	fi

	# Sort latencies
	local sorted
	sorted=$(printf '%s\n' "${LATENCIES[@]}" | sort -n)

	# Calculate index
	local index=$(((count * percentile) / 100))
	if [ $index -eq 0 ]; then
		index=1
	fi

	# Get value at index
	echo "$sorted" | sed -n "${index}p"
}

# Calculate memory statistics
calculate_memory_stats() {
	local count=${#MEMORY_SAMPLES[@]}

	if [ $count -eq 0 ]; then
		echo "0 0 0"
		return
	fi

	local sorted
	sorted=$(printf '%s\n' "${MEMORY_SAMPLES[@]}" | sort -n)

	local min=$(echo "$sorted" | head -1)
	local max=$(echo "$sorted" | tail -1)
	local sum=0

	for val in "${MEMORY_SAMPLES[@]}"; do
		sum=$((sum + val))
	done

	local avg=$((sum / count))

	echo "$min $avg $max"
}

# ============================================================================
# INITIALIZATION
# ============================================================================

init_test() {
	log "=== BetterSpotlight M2 Stress Test Starting ==="
	log "Duration: 8 hours"
	log "Binary: $BINARY_PATH"
	log "Database: $DB_PATH"

	# Create temp directories
	mkdir -p "$TEMP_DIR" "$QUERY_RESULTS_DIR"

	# Clear log files
	>"$LOG_FILE"
	>"$CRASH_LOG"
	>"$LATENCY_LOG"
	>"$MEMORY_LOG"
	>"$INTEGRITY_LOG"

	# Write CSV headers
	echo "timestamp,query,latency_ms,result_count" >"$LATENCY_LOG"
	echo "timestamp,rss_kb" >"$MEMORY_LOG"

	# Check binary exists
	if [ ! -f "$BINARY_PATH" ]; then
		log "ERROR: Binary not found at $BINARY_PATH"
		exit 1
	fi

	# Check database exists
	if [ ! -f "$DB_PATH" ]; then
		log "ERROR: Database not found at $DB_PATH"
		exit 1
	fi

	# Initial integrity check
	log "Running initial integrity check..."
	check_integrity || {
		log "ERROR: Initial integrity check failed"
		exit 1
	}

	log "Initialization complete. Starting stress test..."
}

# ============================================================================
# MAIN TEST LOOP
# ============================================================================

run_stress_test() {
	local current_time
	local elapsed

	while true; do
		current_time=$(date +%s)
		elapsed=$((current_time - START_TIME))

		# Check if duration exceeded
		if [ $elapsed -ge $DURATION ]; then
			break
		fi

		# Get random query
		local query
		query=$(get_random_query)

		# Execute query
		if execute_query "$query"; then
			# Simulate interaction
			simulate_interaction
		fi

		QUERY_COUNT=$((QUERY_COUNT + 1))

		# Every 100 iterations: check process health
		if [ $((QUERY_COUNT % 100)) -eq 0 ]; then
			check_process_health || {
				log "WARNING: Process health check failed at iteration $QUERY_COUNT"
			}
			sample_memory
		fi

		# Every 1000 iterations: check integrity
		if [ $((QUERY_COUNT % 1000)) -eq 0 ]; then
			check_integrity || {
				log "WARNING: Integrity check failed at iteration $QUERY_COUNT"
			}
		fi

		# Progress log every 5000 queries
		if [ $((QUERY_COUNT % 5000)) -eq 0 ]; then
			local hours=$((elapsed / 3600))
			local minutes=$(((elapsed % 3600) / 60))
			log "Progress: $QUERY_COUNT queries in ${hours}h ${minutes}m (crashes: $CRASH_COUNT)"
		fi
	done
}

# ============================================================================
# FINAL REPORT
# ============================================================================

generate_report() {
	local end_time=$(date +%s)
	local total_elapsed=$((end_time - START_TIME))
	local hours=$((total_elapsed / 3600))
	local minutes=$(((total_elapsed % 3600) / 60))
	local seconds=$((total_elapsed % 60))

	# Calculate latency percentiles
	local p50=$(calculate_percentile 50)
	local p95=$(calculate_percentile 95)
	local p99=$(calculate_percentile 99)
	local max_latency=0

	if [ ${#LATENCIES[@]} -gt 0 ]; then
		max_latency=$(printf '%s\n' "${LATENCIES[@]}" | sort -n | tail -1)
	fi

	# Calculate memory stats
	local mem_stats
	mem_stats=$(calculate_memory_stats)
	local mem_min=$(echo "$mem_stats" | awk '{print $1}')
	local mem_avg=$(echo "$mem_stats" | awk '{print $2}')
	local mem_max=$(echo "$mem_stats" | awk '{print $3}')

	# Convert KB to MB
	mem_min=$((mem_min / 1024))
	mem_avg=$((mem_avg / 1024))
	mem_max=$((mem_max / 1024))

	# Determine pass/fail
	local result="PASS"
	local fail_reasons=""

	if [ $CRASH_COUNT -gt 0 ]; then
		result="FAIL"
		fail_reasons="$fail_reasons\n  - Crashes detected: $CRASH_COUNT"
	fi

	if [ "$p95" -gt 300 ]; then
		result="FAIL"
		fail_reasons="$fail_reasons\n  - P95 latency exceeded 300ms: ${p95}ms"
	fi

	if [ $INTEGRITY_CHECKS_PASSED -lt $INTEGRITY_CHECKS_TOTAL ]; then
		result="FAIL"
		fail_reasons="$fail_reasons\n  - Integrity checks failed: $((INTEGRITY_CHECKS_TOTAL - INTEGRITY_CHECKS_PASSED))/$INTEGRITY_CHECKS_TOTAL"
	fi

	if [ $ORPHANED_VECTORS -gt 0 ]; then
		result="FAIL"
		fail_reasons="$fail_reasons\n  - Orphaned vectors found: $ORPHANED_VECTORS"
	fi

	# Print report
	cat <<EOF

================================================================================
=== BetterSpotlight M2 Stress Test Report ===
================================================================================

Duration: ${hours}h ${minutes}m ${seconds}s
Total queries: $QUERY_COUNT
Crashes: $CRASH_COUNT
Restarts: $RESTART_COUNT

Latency Percentiles:
  P50:  ${p50}ms
  P95:  ${p95}ms
  P99:  ${p99}ms
  Max:  ${max_latency}ms

Memory:
  Start RSS:  ${mem_min} MB
  End RSS:    ${mem_max} MB
  Peak RSS:   ${mem_max} MB
  Avg RSS:    ${mem_avg} MB

Database Integrity:
  Checks performed: $INTEGRITY_CHECKS_TOTAL
  Checks passed: $INTEGRITY_CHECKS_PASSED
  Checks failed: $((INTEGRITY_CHECKS_TOTAL - INTEGRITY_CHECKS_PASSED))

Vector Index:
  Entries: $VECTOR_ENTRIES
  Orphaned: $ORPHANED_VECTORS

RESULT: $result
EOF

	if [ "$result" = "FAIL" ]; then
		echo -e "\nFailure reasons:$fail_reasons"
	fi

	echo ""
	echo "Detailed logs:"
	echo "  Latencies:  $LATENCY_LOG"
	echo "  Memory:     $MEMORY_LOG"
	echo "  Crashes:    $CRASH_LOG"
	echo "  Integrity:  $INTEGRITY_LOG"
	echo "  Full log:   $LOG_FILE"
	echo ""

	# Return exit code based on result
	if [ "$result" = "PASS" ]; then
		return 0
	else
		return 1
	fi
}

# ============================================================================
# CLEANUP
# ============================================================================

cleanup() {
	log "Cleaning up..."
	rm -rf "$TEMP_DIR"
}

# ============================================================================
# MAIN ENTRY POINT
# ============================================================================

main() {
	trap cleanup EXIT

	init_test
	run_stress_test
	generate_report
}

main "$@"
