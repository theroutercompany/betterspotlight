#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BS_BUILD_DIR:-${ROOT_DIR}/build-release}"

if [[ $# -ge 1 ]]; then
    BUILD_DIR="$1"
    shift
fi

JOBS="${BS_BUILD_JOBS:-}"
if [[ -z "${JOBS}" ]]; then
    JOBS="$(sysctl -n hw.ncpu 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
fi

cmake --build "${BUILD_DIR}" -j"${JOBS}" "$@"
