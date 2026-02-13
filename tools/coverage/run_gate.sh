#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BS_COVERAGE_BUILD_DIR:-${ROOT_DIR}/build-coverage}"
THRESHOLDS_FILE="${BS_COVERAGE_THRESHOLDS_FILE:-${ROOT_DIR}/Tests/coverage_thresholds.json}"
EXCLUSIONS_FILE="${BS_COVERAGE_EXCLUSIONS_FILE:-${ROOT_DIR}/Tests/coverage_exclusions.txt}"
ACTIVE_PHASE="${BS_COVERAGE_PHASE:-}"
CTEST_LABEL_REGEX="${BS_COVERAGE_CTEST_LABEL_REGEX:-^(unit|integration|service_ipc|relevance|docs_lint)$}"
CTEST_LABEL_EXCLUDE_REGEX="${BS_COVERAGE_CTEST_LABEL_EXCLUDE_REGEX:-^(relevance|relevance_stress)$}"

find_llvm_tool() {
    local tool="$1"
    if command -v "${tool}" >/dev/null 2>&1; then
        command -v "${tool}"
        return 0
    fi
    if command -v xcrun >/dev/null 2>&1; then
        xcrun --find "${tool}" 2>/dev/null || true
        return 0
    fi
    return 1
}

LLVM_PROFDATA_BIN="$(find_llvm_tool llvm-profdata)"
LLVM_COV_BIN="$(find_llvm_tool llvm-cov)"
if [[ -z "${LLVM_PROFDATA_BIN}" || -z "${LLVM_COV_BIN}" ]]; then
    echo "Failed to locate llvm-profdata/llvm-cov in PATH or via xcrun." >&2
    exit 1
fi

QT_PREFIX="${BS_QT_PREFIX_PATH:-}"
if [[ -z "${QT_PREFIX}" ]] && command -v brew >/dev/null 2>&1; then
    QT_PREFIX="$(brew --prefix qt@6 2>/dev/null || true)"
fi

echo "Configuring coverage build in ${BUILD_DIR}"
cmake_args=(
    -S "${ROOT_DIR}"
    -B "${BUILD_DIR}"
    -DCMAKE_BUILD_TYPE=Debug
    -DBETTERSPOTLIGHT_ENABLE_COVERAGE=ON
    -DBETTERSPOTLIGHT_FETCH_MODELS=OFF
    -DBETTERSPOTLIGHT_FETCH_MAX_QUALITY_MODEL=OFF
)
if [[ -n "${QT_PREFIX}" ]]; then
    cmake_args+=(-DCMAKE_PREFIX_PATH="${QT_PREFIX}")
fi
cmake "${cmake_args[@]}"

CPU_COUNT="$(sysctl -n hw.ncpu 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
cmake --build "${BUILD_DIR}" -j"${CPU_COUNT}"

mkdir -p "${BUILD_DIR}/profiles"
find "${BUILD_DIR}/profiles" -type f -name '*.profraw' -delete

echo "Running tests with profile instrumentation"
export LLVM_PROFILE_FILE="${BUILD_DIR}/profiles/%p-%m.profraw"
ctest_cmd=(ctest --test-dir "${BUILD_DIR}" --output-on-failure)
if [[ -n "${CTEST_LABEL_REGEX}" ]]; then
    ctest_cmd+=(-L "${CTEST_LABEL_REGEX}")
fi
if [[ -n "${CTEST_LABEL_EXCLUDE_REGEX}" ]]; then
    ctest_cmd+=(-LE "${CTEST_LABEL_EXCLUDE_REGEX}")
fi
"${ctest_cmd[@]}"

profraw_files=()
while IFS= read -r profraw; do
    profraw_files+=("${profraw}")
done < <(find "${BUILD_DIR}/profiles" -type f -name '*.profraw' | sort)
if [[ "${#profraw_files[@]}" -eq 0 ]]; then
    echo "No .profraw files were generated; coverage gate cannot run." >&2
    exit 1
fi

PROFDATA_PATH="${BUILD_DIR}/coverage.profdata"
"${LLVM_PROFDATA_BIN}" merge -sparse "${profraw_files[@]}" -o "${PROFDATA_PATH}"

test_bins=()
while IFS= read -r test_bin; do
    test_bins+=("${test_bin}")
done < <(find "${BUILD_DIR}/Tests" -type f -perm -111 -name 'test-*' | sort)

service_bins=()
while IFS= read -r service_bin; do
    service_bins+=("${service_bin}")
done < <(find "${BUILD_DIR}/src/services" -type f -perm -111 -name 'betterspotlight-*' | sort)
if [[ "${#test_bins[@]}" -eq 0 ]]; then
    echo "No test binaries found in ${BUILD_DIR}/Tests." >&2
    exit 1
fi

objects=()
for bin in "${test_bins[@]}" "${service_bins[@]}"; do
    if [[ -n "${bin}" ]]; then
        objects+=("-object" "${bin}")
    fi
done

JSON_PATH="${BUILD_DIR}/coverage_export.json"
"${LLVM_COV_BIN}" export \
    --instr-profile "${PROFDATA_PATH}" \
    "${objects[@]}" > "${JSON_PATH}"

"${LLVM_COV_BIN}" report \
    --instr-profile "${PROFDATA_PATH}" \
    "${objects[@]}" > "${BUILD_DIR}/coverage_report.txt"

python3 - <<'PY' "${JSON_PATH}" "${THRESHOLDS_FILE}" "${EXCLUSIONS_FILE}" "${ACTIVE_PHASE}" "${ROOT_DIR}"
import json
import os
import sys

coverage_path, thresholds_path, exclusions_path, forced_phase, root_dir = sys.argv[1:6]
core_root = os.path.realpath(os.path.join(root_dir, "src/core")) + os.sep
services_root = os.path.realpath(os.path.join(root_dir, "src/services")) + os.sep

with open(coverage_path, "r", encoding="utf-8") as f:
    coverage = json.load(f)
with open(thresholds_path, "r", encoding="utf-8") as f:
    thresholds = json.load(f)

active_phase = forced_phase or thresholds.get("active_phase", "phase1")
phase_cfg = thresholds.get("phases", {}).get(active_phase)
if not phase_cfg:
    print(f"Coverage threshold phase '{active_phase}' is not defined.", file=sys.stderr)
    sys.exit(1)

excluded = set()
if os.path.exists(exclusions_path):
    with open(exclusions_path, "r", encoding="utf-8") as f:
        for line in f:
            stripped = line.strip()
            if stripped and not stripped.startswith("#"):
                if os.path.isabs(stripped):
                    excluded_path = stripped
                else:
                    excluded_path = os.path.join(root_dir, stripped)
                excluded.add(os.path.realpath(excluded_path))

line_total = 0
line_covered = 0
branch_total = 0
branch_covered = 0
counted_files = 0

for dataset in coverage.get("data", []):
    for entry in dataset.get("files", []):
        path = os.path.realpath(entry.get("filename", ""))
        if not path.endswith((".cpp", ".mm")):
            continue
        if not path.startswith(core_root) and not path.startswith(services_root):
            continue
        if path in excluded:
            continue

        summary = entry.get("summary", {})
        lines = summary.get("lines", {})
        branches = summary.get("branches", {})

        line_total += int(lines.get("count", 0))
        line_covered += int(lines.get("covered", 0))
        branch_total += int(branches.get("count", 0))
        branch_covered += int(branches.get("covered", 0))
        counted_files += 1

line_pct = (100.0 * line_covered / line_total) if line_total else 100.0
branch_pct = (100.0 * branch_covered / branch_total) if branch_total else 100.0

required_line = float(phase_cfg.get("line", 0.0))
required_branch = float(phase_cfg.get("branch", 0.0))

print("Coverage gate summary")
print(f"  phase: {active_phase}")
print(f"  files_counted: {counted_files}")
print(f"  line: {line_pct:.2f}% (required {required_line:.2f}%)")
print(f"  branch: {branch_pct:.2f}% (required {required_branch:.2f}%)")

failed = []
if line_pct + 1e-9 < required_line:
    failed.append(f"line coverage {line_pct:.2f}% < {required_line:.2f}%")
if branch_pct + 1e-9 < required_branch:
    failed.append(f"branch coverage {branch_pct:.2f}% < {required_branch:.2f}%")

if failed:
    print("Coverage gate failed:", file=sys.stderr)
    for item in failed:
        print(f"  - {item}", file=sys.stderr)
    sys.exit(2)
PY

echo "Coverage gate passed."
