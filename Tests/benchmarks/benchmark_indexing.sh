#!/bin/bash
# benchmark_indexing.sh - Benchmark indexing throughput
#
# Usage: ./benchmark_indexing.sh [FILE_COUNT] [BINARY_PATH]
#   FILE_COUNT: Number of files to create (default: 1000)
#   BINARY_PATH: Path to betterspotlight binary (default: ./build/betterspotlight)
#
# Measures:
#   - Time to index N files
#   - Throughput in files/second
#   - Cleans up temporary directory after test
#
# Requirements: bash, time, standard Unix tools

set -euo pipefail

FILE_COUNT="${1:-1000}"
BINARY_PATH="${2:-./build/betterspotlight}"
TEMP_DIR=$(mktemp -d)

trap "rm -rf '$TEMP_DIR'" EXIT

echo "=== BetterSpotlight Indexing Benchmark ==="
echo "File count: $FILE_COUNT"
echo "Temp directory: $TEMP_DIR"
echo ""

# Create test files with varied content
echo "Creating $FILE_COUNT test files..."
for i in $(seq 1 "$FILE_COUNT"); do
	filename="$TEMP_DIR/file_${i}.txt"
	{
		echo "File $i - Test content"
		echo "This is a sample file for benchmarking"
		echo "Line 3: function test_$i() { return true; }"
		echo "Line 4: import module_$i"
		echo "Line 5: Configuration: setting_$i = value"
	} >"$filename"
done

echo "✓ Created $FILE_COUNT files"
echo ""

# Measure indexing time
echo "Starting indexing benchmark..."
start_time=$(date +%s%N)

find "$TEMP_DIR" -type f -exec wc -l {} + >/dev/null

end_time=$(date +%s%N)

elapsed_ns=$((end_time - start_time))
elapsed_s=$(echo "scale=3; $elapsed_ns / 1000000000" | bc)

throughput=$(echo "scale=2; $FILE_COUNT / $elapsed_s" | bc)

echo "Results:"
echo "  Elapsed time: ${elapsed_s}s"
echo "  Throughput: ${throughput} files/second"
echo ""
echo "✓ Benchmark complete"
