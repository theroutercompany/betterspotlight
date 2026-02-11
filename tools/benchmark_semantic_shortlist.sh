#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
REPORT_PATH="${1:-${ROOT_DIR}/docs/audit/semantic-shortlist-benchmark-report.json}"
MANIFEST_PATH="${ROOT_DIR}/data/models/manifest.json"

if [[ ! -f "${MANIFEST_PATH}" ]]; then
  echo "manifest not found: ${MANIFEST_PATH}" >&2
  exit 1
fi

if [[ ! -d "${BUILD_DIR}" ]]; then
  echo "build directory not found: ${BUILD_DIR}" >&2
  exit 1
fi

mkdir -p "$(dirname "${REPORT_PATH}")"

run_ctest_case() {
  local label="$1"
  local regex="$2"
  local start
  local end
  start="$(date +%s)"
  local status="passed"
  if ! ctest --test-dir "${BUILD_DIR}" -R "${regex}" --output-on-failure >/tmp/bs-${label}.log 2>&1; then
    status="failed"
  fi
  end="$(date +%s)"
  local elapsed=$((end - start))
  printf '{"label":"%s","regex":"%s","status":"%s","elapsedSec":%d}' \
    "${label}" "${regex}" "${status}" "${elapsed}"
}

relevance_case="$(run_ctest_case "relevance_fixture" "test-query-service-relevance-fixture")"
ui_case="$(run_ctest_case "ui_sim_suite" "test-ui-sim-query-suite$")"

readarray -t manifest_primary < <(python3 - "${MANIFEST_PATH}" <<'PY'
import json
import sys

manifest_path = sys.argv[1]
with open(manifest_path, "r", encoding="utf-8") as fh:
    data = json.load(fh)

models = data.get("models", {})
primary = models.get("bi-encoder", {})
print(primary.get("file", ""))
print(int(primary.get("dimensions", 0) or 0))
print(primary.get("generationId", ""))
PY
)

primary_model_file="${manifest_primary[0]:-}"
primary_model_dimensions="${manifest_primary[1]:-0}"
primary_generation="${manifest_primary[2]:-unknown}"
primary_model_path="${ROOT_DIR}/data/models/${primary_model_file}"

hq_model_present=0
if [[ "${primary_model_dimensions}" -ge 1024 && -n "${primary_model_file}" && -f "${primary_model_path}" ]]; then
  hq_model_present=1
fi

cat > "${REPORT_PATH}" <<JSON
{
  "generatedAt": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "manifest": "${MANIFEST_PATH}",
  "primaryModelFile": "${primary_model_file}",
  "primaryGeneration": "${primary_generation}",
  "primaryDimensions": ${primary_model_dimensions},
  "highQualityModelPresent": ${hq_model_present},
  "longRunGates": {
    "memoryDrift24h": "deferred_runner_unavailable",
    "stress48h": "deferred_runner_unavailable"
  },
  "cases": [
    ${relevance_case},
    ${ui_case}
  ]
}
JSON

echo "Wrote semantic shortlist report: ${REPORT_PATH}"
