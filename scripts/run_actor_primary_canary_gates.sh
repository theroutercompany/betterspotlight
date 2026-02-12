#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROFILE_FILE="${ROOT_DIR}/scripts/profiles/actor_primary_canary.env"
STRESS_SCRIPT="${ROOT_DIR}/Tests/benchmarks/stress_48h.sh"
MEMORY_SCRIPT="${ROOT_DIR}/Tests/benchmarks/memory_drift_24h.sh"

DEFAULT_STRESS_SECONDS=$((48 * 3600))
DEFAULT_MEMORY_SECONDS=$((24 * 3600))
SMOKE_SECONDS=300

usage() {
    cat <<'EOF'
Usage:
  ./scripts/run_actor_primary_canary_gates.sh [stress_seconds] [memory_seconds]
  ./scripts/run_actor_primary_canary_gates.sh --smoke [stress_seconds] [memory_seconds]

Description:
  Applies the actor-primary canary profile and runs both long-run gates:
    1) Tests/benchmarks/stress_48h.sh
    2) Tests/benchmarks/memory_drift_24h.sh

Environment:
  BS_STRESS_DURATION_SECONDS   Override stress duration
  BS_MEM_DURATION_SECONDS      Override memory drift duration
  BS_MEM_SAMPLE_INTERVAL       Memory sample interval seconds (default in harness: 60)
  BS_MEM_DRIFT_LIMIT_MB        Memory drift threshold MB (default in harness: 10)
  BS_CANARY_ARTIFACT_ROOT      Root artifact dir (default: /tmp/bs_actor_primary_canary_<ts>)
  BS_STRESS_ARTIFACT_DIR       Optional explicit stress artifact dir
  BS_MEM_ARTIFACT_DIR          Optional explicit memory artifact dir
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

if [[ ! -f "${PROFILE_FILE}" ]]; then
    echo "Missing canary profile: ${PROFILE_FILE}" >&2
    exit 1
fi
if [[ ! -x "${STRESS_SCRIPT}" ]]; then
    echo "Missing executable stress harness: ${STRESS_SCRIPT}" >&2
    exit 1
fi
if [[ ! -x "${MEMORY_SCRIPT}" ]]; then
    echo "Missing executable memory harness: ${MEMORY_SCRIPT}" >&2
    exit 1
fi

stress_default="${DEFAULT_STRESS_SECONDS}"
memory_default="${DEFAULT_MEMORY_SECONDS}"
if [[ "${smoke_mode}" -eq 1 ]]; then
    stress_default="${SMOKE_SECONDS}"
    memory_default="${SMOKE_SECONDS}"
fi

stress_duration="${BS_STRESS_DURATION_SECONDS:-${1:-${stress_default}}}"
memory_duration="${BS_MEM_DURATION_SECONDS:-${2:-${memory_default}}}"

timestamp="$(date +%Y%m%d_%H%M%S)"
artifact_root="${BS_CANARY_ARTIFACT_ROOT:-/tmp/bs_actor_primary_canary_${timestamp}}"
export BS_STRESS_ARTIFACT_DIR="${BS_STRESS_ARTIFACT_DIR:-${artifact_root}/stress_48h}"
export BS_MEM_ARTIFACT_DIR="${BS_MEM_ARTIFACT_DIR:-${artifact_root}/memory_drift_24h}"
mkdir -p "${BS_STRESS_ARTIFACT_DIR}" "${BS_MEM_ARTIFACT_DIR}"

set -a
# shellcheck disable=SC1090
source "${PROFILE_FILE}"
set +a

echo "=== BetterSpotlight Actor-Primary Canary Gates ==="
echo "Profile: ${PROFILE_FILE}"
echo "Control plane mode: ${BETTERSPOTLIGHT_CONTROL_PLANE_MODE:-unset}"
echo "Health source mode: ${BETTERSPOTLIGHT_HEALTH_SOURCE_MODE:-unset}"
echo "Pipeline actor mode: ${BETTERSPOTLIGHT_PIPELINE_ACTOR_MODE:-unset}"
echo "Inference supervisor mode: ${BETTERSPOTLIGHT_INFERENCE_SUPERVISOR_MODE:-unset}"
echo "Stress duration: ${stress_duration}s"
echo "Memory duration: ${memory_duration}s"
echo "Artifacts root: ${artifact_root}"

echo
echo "[1/2] Running stress gate..."
"${STRESS_SCRIPT}" "${stress_duration}"

echo
echo "[2/2] Running memory drift gate..."
"${MEMORY_SCRIPT}" "${memory_duration}"

echo
echo "All canary gates completed."
echo "Stress artifacts: ${BS_STRESS_ARTIFACT_DIR}"
echo "Memory artifacts: ${BS_MEM_ARTIFACT_DIR}"
