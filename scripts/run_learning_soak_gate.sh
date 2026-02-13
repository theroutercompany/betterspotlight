#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOAK_SCRIPT="${ROOT_DIR}/Tests/benchmarks/learning_soak_48h.sh"

DEFAULT_DURATION=$((48 * 3600))
SMOKE_DURATION=300

usage() {
    cat <<'EOF'
Usage:
  ./scripts/run_learning_soak_gate.sh [duration_seconds]
  ./scripts/run_learning_soak_gate.sh --smoke [duration_seconds]

Description:
  Runs the continual-learning soak gate with default learning settings:
    - behavior stream enabled
    - learning enabled
    - rollout mode: shadow_training
    - pause-on-user-input: false

Environment overrides:
  BS_LEARNING_ROLLOUT_MODE
  BS_LEARNING_PAUSE_ON_USER_INPUT
  BS_LEARNING_CYCLE_INTERVAL
  BS_STRESS_SAMPLE_INTERVAL
  BS_STRESS_INTEGRITY_INTERVAL
  BS_STRESS_ARTIFACT_DIR
  BS_STRESS_QUERY_BIN / BS_STRESS_INDEXER_BIN / BS_STRESS_EXTRACTOR_BIN
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi

smoke_mode=0
if [[ "${1:-}" == "--smoke" ]]; then
    smoke_mode=1
    shift
fi

if [[ ! -x "${SOAK_SCRIPT}" ]]; then
    echo "Missing executable soak harness: ${SOAK_SCRIPT}" >&2
    exit 1
fi

duration="${1:-$DEFAULT_DURATION}"
if [[ "${smoke_mode}" -eq 1 ]]; then
    duration="${1:-$SMOKE_DURATION}"
fi

export BS_LEARNING_ROLLOUT_MODE="${BS_LEARNING_ROLLOUT_MODE:-shadow_training}"
export BS_LEARNING_PAUSE_ON_USER_INPUT="${BS_LEARNING_PAUSE_ON_USER_INPUT:-0}"

echo "=== BetterSpotlight Learning Soak Gate ==="
echo "Duration: ${duration}s"
echo "Rollout mode: ${BS_LEARNING_ROLLOUT_MODE}"
echo "Pause on input: ${BS_LEARNING_PAUSE_ON_USER_INPUT}"
echo "Harness: ${SOAK_SCRIPT}"
echo

"${SOAK_SCRIPT}" "${duration}"

