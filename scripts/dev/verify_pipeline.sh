#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SETUP_SCRIPT="${ROOT_DIR}/scripts/dev/setup_env.sh"
BUILD_LAUNCH_SCRIPT="${ROOT_DIR}/scripts/dev/build_launch.sh"
TARGETS_SCRIPT="${ROOT_DIR}/scripts/dev/test_targets.sh"
ENV_FILE="${ROOT_DIR}/.build/dev-env.sh"
BUILD_DIR_DEFAULT="${ROOT_DIR}/build-dev-verify"

BUILD_DIR="${BS_DEV_VERIFY_BUILD_DIR:-${BUILD_DIR_DEFAULT}}"
BUILD_TYPE="${BS_DEV_VERIFY_BUILD_TYPE:-Release}"
CLEAN_BUILD=1
SKIP_LAUNCH=0

usage() {
    cat <<'EOF'
Usage: scripts/dev/verify_pipeline.sh [--build-dir PATH] [--build-type TYPE] [--no-clean] [--skip-launch]

Runs the stabilization verification flow for BetterSpotlight:
1. validates the dev environment,
2. configures/builds app, helpers, and pipeline/indexer/inference tests,
3. runs the focused stabilization test suite,
4. performs a startup smoke check by launching the app and helpers unless --skip-launch is used.
EOF
}

log() {
    printf '[verify-pipeline] %s\n' "$*"
}

fail() {
    printf '[verify-pipeline] Error: %s\n' "$*" >&2
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)
            [[ $# -ge 2 ]] || fail "--build-dir requires a path"
            BUILD_DIR="$2"
            shift 2
            ;;
        --build-type)
            [[ $# -ge 2 ]] || fail "--build-type requires a value"
            BUILD_TYPE="$2"
            shift 2
            ;;
        --no-clean)
            CLEAN_BUILD=0
            shift
            ;;
        --skip-launch)
            SKIP_LAUNCH=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            fail "unknown argument: $1"
            ;;
    esac
done

[[ -x "${SETUP_SCRIPT}" ]] || fail "setup script is missing or not executable: ${SETUP_SCRIPT}"
[[ -x "${BUILD_LAUNCH_SCRIPT}" ]] || fail "build launch script is missing or not executable: ${BUILD_LAUNCH_SCRIPT}"
[[ -f "${TARGETS_SCRIPT}" ]] || fail "targets script is missing: ${TARGETS_SCRIPT}"

# shellcheck disable=SC1090
source "${TARGETS_SCRIPT}"

"${SETUP_SCRIPT}" --write-env-file "${ENV_FILE}"
# shellcheck disable=SC1090
source "${ENV_FILE}"

if [[ "${CLEAN_BUILD}" -eq 1 ]]; then
    rm -rf "${BUILD_DIR}"
fi

export BS_BUILD_DIR="${BUILD_DIR}"
export BS_BUILD_TYPE="${BUILD_TYPE}"
export BS_FETCH_MODELS=OFF
export BS_ENABLE_SPARKLE=OFF
export BS_PREFER_COREML=ON

CMAKE_ARGS=(
    "-DQt6_DIR=${Qt6_DIR}"
    "-DQt6Qml_DIR=${Qt6Qml_DIR}"
    "-DQt6Quick_DIR=${Qt6Quick_DIR}"
    "-DQt6QuickControls2_DIR=${Qt6QuickControls2_DIR}"
    "-DQt6QuickTemplates2_DIR=${Qt6QuickTemplates2_DIR}"
    "-DQt6QmlTools_DIR=${Qt6QmlTools_DIR}"
    "-DONNXRuntime_INCLUDE_DIR=${ONNXRuntime_INCLUDE_DIR}"
    "-DONNXRuntime_LIBRARY=${ONNXRuntime_LIBRARY}"
)

log "Configuring ${BUILD_DIR} (${BUILD_TYPE})..."
"${ROOT_DIR}/scripts/ci/configure.sh" "${BUILD_DIR}" "${BUILD_TYPE}" "${CMAKE_ARGS[@]}"

mapfile -t APP_TARGETS < <(bs_dev_app_targets)
mapfile -t TEST_TARGETS < <(bs_dev_extended_reliability_test_targets)
BUILD_TARGETS=("${APP_TARGETS[@]}" "${TEST_TARGETS[@]}")

log "Building stabilization targets..."
"${ROOT_DIR}/scripts/ci/build.sh" "${BUILD_DIR}" --target "${BUILD_TARGETS[@]}"

TEST_REGEX="$(bs_dev_targets_regex extended)"
log "Running extended reliability tests..."
ctest --test-dir "${BUILD_DIR}" --output-on-failure --timeout 300 -R "${TEST_REGEX}"

if [[ "${SKIP_LAUNCH}" -eq 0 ]]; then
    log "Running startup smoke check..."
    "${BUILD_LAUNCH_SCRIPT}" \
        --build-dir "${BUILD_DIR}" \
        --build-type "${BUILD_TYPE}" \
        --no-clean \
        --with-tests
fi

log "Stabilization verification complete."
