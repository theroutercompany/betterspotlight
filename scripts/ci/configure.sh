#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

BUILD_DIR="${BS_BUILD_DIR:-${ROOT_DIR}/build-release}"
BUILD_TYPE="${BS_BUILD_TYPE:-Release}"

if [[ $# -ge 1 ]]; then
    BUILD_DIR="$1"
    shift
fi
if [[ $# -ge 1 ]]; then
    BUILD_TYPE="$1"
    shift
fi

export LC_ALL="${LC_ALL:-C}"
export LANG="${LANG:-C}"
export TZ="${TZ:-UTC}"
if [[ -z "${SOURCE_DATE_EPOCH:-}" ]]; then
    SOURCE_DATE_EPOCH="$(git -C "${ROOT_DIR}" log -1 --pretty=%ct 2>/dev/null || date +%s)"
    export SOURCE_DATE_EPOCH
fi

FETCH_MODELS="${BS_FETCH_MODELS:-ON}"
FETCH_MAX_QUALITY="${BS_FETCH_MAX_QUALITY_MODEL:-OFF}"
ENABLE_SPARKLE="${BS_ENABLE_SPARKLE:-OFF}"
ENABLE_COVERAGE="${BS_ENABLE_COVERAGE:-OFF}"
PREFER_COREML="${BS_PREFER_COREML:-ON}"

CMAKE_ARGS=(
    -S "${ROOT_DIR}"
    -B "${BUILD_DIR}"
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
    -DBETTERSPOTLIGHT_FETCH_MODELS="${FETCH_MODELS}"
    -DBETTERSPOTLIGHT_FETCH_MAX_QUALITY_MODEL="${FETCH_MAX_QUALITY}"
    -DBETTERSPOTLIGHT_ENABLE_SPARKLE="${ENABLE_SPARKLE}"
    -DBETTERSPOTLIGHT_ENABLE_COVERAGE="${ENABLE_COVERAGE}"
    -DBETTERSPOTLIGHT_PREFER_COREML="${PREFER_COREML}"
)

if [[ -n "${BS_QT_PREFIX_PATH:-}" ]]; then
    CMAKE_ARGS+=("-DCMAKE_PREFIX_PATH=${BS_QT_PREFIX_PATH}")
elif command -v brew >/dev/null 2>&1; then
    QT_PREFIX="$(brew --prefix qt@6 2>/dev/null || true)"
    if [[ -n "${QT_PREFIX}" ]]; then
        CMAKE_ARGS+=("-DCMAKE_PREFIX_PATH=${QT_PREFIX}")
    fi
fi

if [[ $# -gt 0 ]]; then
    CMAKE_ARGS+=("$@")
fi

cmake "${CMAKE_ARGS[@]}"
