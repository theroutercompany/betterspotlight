#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

export BS_COVERAGE_PHASE="${BS_COVERAGE_PHASE:-phase1}"
export BS_QT_PREFIX_PATH="${BS_QT_PREFIX_PATH:-}"

exec /bin/bash "${ROOT_DIR}/tools/coverage/run_gate.sh" "$@"
