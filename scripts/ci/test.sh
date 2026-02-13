#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BS_BUILD_DIR:-${ROOT_DIR}/build-release}"

if [[ $# -ge 1 ]]; then
    BUILD_DIR="$1"
    shift
fi

LABEL_REGEX="${BS_CTEST_LABEL_REGEX:-^(unit|integration|service_ipc|relevance|docs_lint)$}"
LABEL_EXCLUDE_REGEX="${BS_CTEST_LABEL_EXCLUDE_REGEX:-}"

CTEST_ARGS=(--test-dir "${BUILD_DIR}" --output-on-failure)
if [[ -n "${LABEL_REGEX}" ]]; then
    CTEST_ARGS+=(-L "${LABEL_REGEX}")
fi
if [[ -n "${LABEL_EXCLUDE_REGEX}" ]]; then
    CTEST_ARGS+=(-LE "${LABEL_EXCLUDE_REGEX}")
fi
if [[ $# -gt 0 ]]; then
    CTEST_ARGS+=("$@")
fi

ctest "${CTEST_ARGS[@]}"
