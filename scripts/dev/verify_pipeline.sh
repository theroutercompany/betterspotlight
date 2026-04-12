#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SETUP_SCRIPT="${ROOT_DIR}/scripts/dev/setup_env.sh"
BUILD_LAUNCH_SCRIPT="${ROOT_DIR}/scripts/dev/build_launch.sh"
PREFLIGHT_SCRIPT="${ROOT_DIR}/scripts/dev/verify_preflight.py"
ENV_FILE="${ROOT_DIR}/.build/dev-env.sh"
BUILD_DIR_DEFAULT="${ROOT_DIR}/build-stabilize"

BUILD_DIR="${BS_DEV_VERIFY_BUILD_DIR:-${BUILD_DIR_DEFAULT}}"
BUILD_TYPE="${BS_DEV_VERIFY_BUILD_TYPE:-Release}"
PROFILE="${BS_DEV_VERIFY_PROFILE:-core-hermetic}"
CLEAN_BUILD=1
SKIP_LAUNCH=0
ALLOW_DEGRADED=0

usage() {
    cat <<'EOF'
Usage: scripts/dev/verify_pipeline.sh [--build-dir PATH] [--build-type TYPE] [--profile PROFILE] [--allow-degraded] [--no-clean] [--skip-launch]

Runs the full default verification flow for BetterSpotlight:
1. validates the dev environment,
2. checks docs parity plus an explicit capability/runtime-mode matrix preflight,
3. configures/builds the runtime plus the full declared verification test inventory,
4. checks that every declared CTest executable was materialized and dylib links resolve,
5. runs verification labels based on PROFILE:
   - core-hermetic: unit|integration|service_ipc|docs_lint|relevance
   - extended-capabilities: same labels, stricter capability requirements
   - stress: core-hermetic plus relevance_stress
6. performs a startup smoke check by launching the app and helpers unless --skip-launch is used.
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
        --profile)
            [[ $# -ge 2 ]] || fail "--profile requires a value"
            PROFILE="$2"
            shift 2
            ;;
        --allow-degraded)
            ALLOW_DEGRADED=1
            shift
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
[[ -x "${PREFLIGHT_SCRIPT}" ]] || fail "preflight helper is missing or not executable: ${PREFLIGHT_SCRIPT}"

case "${PROFILE}" in
    core-hermetic|extended-capabilities|stress)
        ;;
    *)
        fail "unsupported profile '${PROFILE}' (expected core-hermetic|extended-capabilities|stress)"
        ;;
esac

SETUP_ARGS=(--write-env-file "${ENV_FILE}")
if [[ "${ALLOW_DEGRADED}" -eq 1 ]]; then
    SETUP_ARGS+=(--allow-degraded-pdf)
fi
"${SETUP_SCRIPT}" "${SETUP_ARGS[@]}"
# shellcheck disable=SC1090
source "${ENV_FILE}"

log "Checking docs parity..."
"${PREFLIGHT_SCRIPT}" docs-parity --root-dir "${ROOT_DIR}"

log "Running capability/runtime-mode preflight..."
CAPABILITY_ARGS=(capabilities --root-dir "${ROOT_DIR}" --profile "${PROFILE}")
if [[ "${ALLOW_DEGRADED}" -eq 1 ]]; then
    CAPABILITY_ARGS+=(--allow-degraded)
fi
"${PREFLIGHT_SCRIPT}" "${CAPABILITY_ARGS[@]}"

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

log "Building full default verification target..."
"${ROOT_DIR}/scripts/ci/build.sh" "${BUILD_DIR}" --target betterspotlight-default-verification

log "Checking configured build contract for profile '${PROFILE}'..."
BUILD_CONTRACT_ARGS=(build-contract --build-dir "${BUILD_DIR}" --profile "${PROFILE}")
if [[ "${ALLOW_DEGRADED}" -eq 1 ]]; then
    BUILD_CONTRACT_ARGS+=(--allow-degraded)
fi
"${PREFLIGHT_SCRIPT}" "${BUILD_CONTRACT_ARGS[@]}"

log "Checking CTest inventory for missing executables..."
python3 - "${BUILD_DIR}" <<'PY'
import json
import os
import subprocess
import sys

build_dir = sys.argv[1]
raw = subprocess.check_output(
    ["ctest", "--test-dir", build_dir, "--show-only=json-v1"],
    text=True,
)
report = json.loads(raw)
missing = []
for test in report.get("tests", []):
    command = test.get("command") or []
    if not command:
        missing.append(f"{test.get('name', '<unnamed>')}: missing command")
        continue
    executable = command[0]
    if os.path.isabs(executable) and not os.path.exists(executable):
        missing.append(f"{test.get('name', '<unnamed>')}: {executable}")

if missing:
    sys.stderr.write("Default verification inventory mismatch; declared tests are missing executables:\n")
    for line in missing:
        sys.stderr.write(f"  - {line}\n")
    sys.exit(1)
PY

log "Checking binary link-health..."
"${PREFLIGHT_SCRIPT}" link-health --build-dir "${BUILD_DIR}"

DEFAULT_LABEL_REGEX='^(unit|integration|service_ipc|docs_lint|relevance)$'
EXCLUDE_LABEL_REGEX='^relevance_stress$'
log "Running verification labels for profile '${PROFILE}'..."
ctest \
    --test-dir "${BUILD_DIR}" \
    --output-on-failure \
    --timeout 300 \
    -L "${DEFAULT_LABEL_REGEX}" \
    -LE "${EXCLUDE_LABEL_REGEX}"

if [[ "${PROFILE}" == "stress" ]]; then
    ctest \
        --test-dir "${BUILD_DIR}" \
        --output-on-failure \
        --timeout 300 \
        -L "^relevance_stress$"
fi

if [[ "${SKIP_LAUNCH}" -eq 0 ]]; then
    [[ -x "${BUILD_LAUNCH_SCRIPT}" ]] \
        || fail "startup smoke requested but build launch script is missing: ${BUILD_LAUNCH_SCRIPT} (use --skip-launch)"
    log "Running startup smoke check..."
    "${BUILD_LAUNCH_SCRIPT}" \
        --build-dir "${BUILD_DIR}" \
        --build-type "${BUILD_TYPE}" \
        --no-clean \
        --with-tests
fi

log "Default verification complete."
