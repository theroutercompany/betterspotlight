#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SETUP_SCRIPT="${ROOT_DIR}/scripts/dev/setup_env.sh"
TARGETS_SCRIPT="${ROOT_DIR}/scripts/dev/test_targets.sh"
ENV_FILE="${ROOT_DIR}/.build/dev-env.sh"
RUNTIME_ROOT_DIR="${ROOT_DIR}/.build/dev-runtime"
BUILD_DIR_DEFAULT="${ROOT_DIR}/build-dev"

BUILD_DIR="${BS_DEV_BUILD_DIR:-${BUILD_DIR_DEFAULT}}"
BUILD_TYPE="${BS_DEV_BUILD_TYPE:-Release}"
CLEAN_BUILD=1
FETCH_BOOTSTRAP_MODELS=1
BUILD_PIPELINE_TESTS=0

usage() {
    cat <<'EOF'
Usage: scripts/dev/build_launch.sh [--build-dir PATH] [--build-type TYPE] [--no-clean] [--skip-model-fetch] [--with-tests]

Validates the local dev environment, performs a fresh BetterSpotlight dev build,
starts the helper services against an isolated runtime directory, and launches
the app binary with the required Qt/runtime environment. Use --with-tests to
also build the pipeline/indexer/inference stabilization test targets.
EOF
}

log() {
    printf '[dev-launch] %s\n' "$*"
}

fail() {
    printf '[dev-launch] Error: %s\n' "$*" >&2
    exit 1
}

kill_if_alive() {
    local pid="$1"
    if [[ -z "${pid}" ]]; then
        return 0
    fi

    if kill -0 "${pid}" 2>/dev/null; then
        kill "${pid}" 2>/dev/null || true
        for _ in {1..20}; do
            if ! kill -0 "${pid}" 2>/dev/null; then
                return 0
            fi
            sleep 0.25
        done
        kill -9 "${pid}" 2>/dev/null || true
    fi
}

wait_for_path() {
    local path="$1"
    local timeout_sec="$2"
    local elapsed=0

    while (( elapsed < timeout_sec )); do
        if [[ -e "${path}" ]]; then
            return 0
        fi
        sleep 1
        elapsed=$((elapsed + 1))
    done

    return 1
}

wait_for_log_line() {
    local log_file="$1"
    local needle="$2"
    local timeout_sec="$3"
    local elapsed=0

    while (( elapsed < timeout_sec )); do
        if [[ -f "${log_file}" ]] && grep -Fq "${needle}" "${log_file}"; then
            return 0
        fi
        sleep 1
        elapsed=$((elapsed + 1))
    done

    return 1
}

tail_log_on_failure() {
    local log_file="$1"
    if [[ -f "${log_file}" ]]; then
        printf '\n----- %s (tail) -----\n' "${log_file}" >&2
        tail -n 80 "${log_file}" >&2 || true
        printf '%s\n' '----------------------------' >&2
    fi
}

ensure_bootstrap_models() {
    local models_dir="${BETTERSPOTLIGHT_MODELS_DIR}"
    local missing=0

    [[ -f "${models_dir}/manifest.json" ]] || fail "runtime model manifest missing at ${models_dir}/manifest.json"

    for required_file in \
        "${models_dir}/vocab.txt" \
        "${models_dir}/bge-small-en-v1.5-int8.onnx" \
        "${models_dir}/bge-large-en-v1.5-f32.onnx"; do
        if [[ ! -s "${required_file}" ]]; then
            missing=1
        fi
    done

    if [[ "${missing}" -eq 0 || "${FETCH_BOOTSTRAP_MODELS}" -eq 0 ]]; then
        return 0
    fi

    log "Fetching bootstrap embedding models..."
    "${ROOT_DIR}/tools/fetch_embedding_models.sh" --max-quality
}

stop_repo_processes() {
    local line pid
    while IFS= read -r line; do
        [[ -n "${line}" ]] || continue
        pid="${line%% *}"
        kill_if_alive "${pid}"
    done < <(
        ROOT_FOR_PY="${ROOT_DIR}" python3 - <<'PY'
import os
import subprocess

root = os.environ["ROOT_FOR_PY"]
out = subprocess.check_output(["ps", "-axo", "pid=,args="], text=True)
for raw in out.splitlines():
    line = raw.strip()
    if not line:
        continue
    if root not in line or "betterspotlight" not in line:
        continue
    print(line.split(None, 1)[0])
PY
    )
}

stop_previous_session() {
    local state_file="${RUNTIME_ROOT_DIR}/current-session.sh"
    local previous_app_pid=""
    local previous_service_pids=""
    if [[ -f "${state_file}" ]]; then
        previous_app_pid="$(grep -E '^export APP_PID=' "${state_file}" | head -n 1 | cut -d'"' -f2 || true)"
        previous_service_pids="$(grep -E '^export SERVICE_PIDS=' "${state_file}" | head -n 1 | cut -d'"' -f2 || true)"
        kill_if_alive "${previous_app_pid}"
        for pid in ${previous_service_pids}; do
            kill_if_alive "${pid}"
        done
    fi
    stop_repo_processes
}

write_session_state() {
    local state_file="${RUNTIME_ROOT_DIR}/current-session.sh"
    cat > "${state_file}" <<EOF
#!/usr/bin/env bash
export APP_PID="${APP_PID}"
export SERVICE_PIDS="${SERVICE_PIDS}"
export BUILD_DIR="${BUILD_DIR}"
export BUILD_TYPE="${BUILD_TYPE}"
export APP_BUNDLE="${APP_BUNDLE}"
export APP_BINARY="${APP_BINARY}"
export HELPERS_DIR="${HELPERS_DIR}"
export LOG_DIR="${LOG_DIR}"
export INSTANCE_ID="${INSTANCE_ID}"
export RUNTIME_DIR="${RUNTIME_DIR}"
export SOCKET_DIR="${SOCKET_DIR}"
export PID_DIR="${PID_DIR}"
EOF
    chmod +x "${state_file}"
}

launch_service() {
    local name="$1"
    local binary="$2"
    local log_file="${LOG_DIR}/${name}.log"
    local socket_path="${SOCKET_DIR}/${name}.sock"

    [[ -x "${binary}" ]] || fail "service binary is not executable: ${binary}"

    nohup env \
        BETTERSPOTLIGHT_INSTANCE_ID="${INSTANCE_ID}" \
        BETTERSPOTLIGHT_RUNTIME_DIR="${RUNTIME_DIR}" \
        BETTERSPOTLIGHT_SOCKET_DIR="${SOCKET_DIR}" \
        BETTERSPOTLIGHT_PID_DIR="${PID_DIR}" \
        BETTERSPOTLIGHT_MODELS_DIR="${BETTERSPOTLIGHT_MODELS_DIR}" \
        QT_PLUGIN_PATH="${QT_PLUGIN_PATH}" \
        QML_IMPORT_PATH="${QML_IMPORT_PATH}" \
        QML2_IMPORT_PATH="${QML2_IMPORT_PATH}" \
        "${binary}" >"${log_file}" 2>&1 < /dev/null &
    local pid=$!
    SERVICE_PIDS+=" ${pid}"

    if ! wait_for_path "${socket_path}" 20; then
        tail_log_on_failure "${log_file}"
        fail "service '${name}' did not create ${socket_path}"
    fi
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
        --skip-model-fetch)
            FETCH_BOOTSTRAP_MODELS=0
            shift
            ;;
        --with-tests)
            BUILD_PIPELINE_TESTS=1
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
[[ -f "${TARGETS_SCRIPT}" ]] || fail "targets script is missing: ${TARGETS_SCRIPT}"

# shellcheck disable=SC1090
source "${TARGETS_SCRIPT}"

"${SETUP_SCRIPT}" --write-env-file "${ENV_FILE}"
# shellcheck disable=SC1090
source "${ENV_FILE}"

mkdir -p "${RUNTIME_ROOT_DIR}"
ensure_bootstrap_models
stop_previous_session

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

log "Building fresh BetterSpotlight dev targets..."
mapfile -t BUILD_TARGETS < <(bs_dev_app_targets)
if [[ "${BUILD_PIPELINE_TESTS}" -eq 1 ]]; then
    mapfile -t PIPELINE_TEST_TARGETS < <(bs_dev_stabilization_test_targets)
    BUILD_TARGETS+=("${PIPELINE_TEST_TARGETS[@]}")
fi
"${ROOT_DIR}/scripts/ci/build.sh" "${BUILD_DIR}" --target "${BUILD_TARGETS[@]}"

APP_BUNDLE="${BUILD_DIR}/src/app/betterspotlight.app"
APP_BINARY="${APP_BUNDLE}/Contents/MacOS/betterspotlight"
HELPERS_DIR="${APP_BUNDLE}/Contents/Helpers"

[[ -x "${APP_BINARY}" ]] || fail "app binary is missing: ${APP_BINARY}"
for service_name in indexer extractor query inference; do
    [[ -x "${HELPERS_DIR}/betterspotlight-${service_name}" ]] \
        || fail "helper binary missing from bundle: ${HELPERS_DIR}/betterspotlight-${service_name}"
done

INSTANCE_ID="dev-$(date +%Y%m%d-%H%M%S)-$$"
RUNTIME_DIR="/tmp/betterspotlight-$(id -u)/${INSTANCE_ID}"
SOCKET_DIR="${RUNTIME_DIR}/sockets"
PID_DIR="${RUNTIME_DIR}/pids"
LOG_DIR="${RUNTIME_ROOT_DIR}/${INSTANCE_ID}"
APP_LOG="${LOG_DIR}/app.log"
SERVICE_PIDS=""
APP_PID=""

mkdir -p "${SOCKET_DIR}" "${PID_DIR}" "${LOG_DIR}"

trap '
    if [[ -n "${APP_PID}" ]]; then
        kill_if_alive "${APP_PID}"
    fi
    for pid in ${SERVICE_PIDS:-}; do
        kill_if_alive "${pid}"
    done
' ERR INT TERM

log "Starting helper services..."
launch_service "indexer" "${HELPERS_DIR}/betterspotlight-indexer"
launch_service "extractor" "${HELPERS_DIR}/betterspotlight-extractor"
launch_service "query" "${HELPERS_DIR}/betterspotlight-query"
launch_service "inference" "${HELPERS_DIR}/betterspotlight-inference"

log "Launching BetterSpotlight app..."
nohup env \
    BETTERSPOTLIGHT_INSTANCE_ID="${INSTANCE_ID}" \
    BETTERSPOTLIGHT_RUNTIME_DIR="${RUNTIME_DIR}" \
    BETTERSPOTLIGHT_SOCKET_DIR="${SOCKET_DIR}" \
    BETTERSPOTLIGHT_PID_DIR="${PID_DIR}" \
    BETTERSPOTLIGHT_MODELS_DIR="${BETTERSPOTLIGHT_MODELS_DIR}" \
    QT_PLUGIN_PATH="${QT_PLUGIN_PATH}" \
    QML_IMPORT_PATH="${QML_IMPORT_PATH}" \
    QML2_IMPORT_PATH="${QML2_IMPORT_PATH}" \
    "${APP_BINARY}" >"${APP_LOG}" 2>&1 < /dev/null &
APP_PID=$!

if ! wait_for_log_line "${APP_LOG}" "BetterSpotlight ready" 30; then
    if ! kill -0 "${APP_PID}" 2>/dev/null; then
        tail_log_on_failure "${APP_LOG}"
        fail "app exited before reaching ready state"
    fi
    tail_log_on_failure "${APP_LOG}"
    fail "app did not report readiness within 30s"
fi

if ! kill -0 "${APP_PID}" 2>/dev/null; then
    tail_log_on_failure "${APP_LOG}"
    fail "app reached ready state but did not remain alive"
fi

if grep -Fq "QObject::startTimer: Timers cannot be started from another thread" "${APP_LOG}"; then
    tail_log_on_failure "${APP_LOG}"
    fail "app log reported cross-thread timer startup"
fi

for pid in ${SERVICE_PIDS}; do
    if ! kill -0 "${pid}" 2>/dev/null; then
        fail "helper service pid ${pid} exited after launch"
    fi
done

write_session_state
trap - ERR INT TERM

log "BetterSpotlight dev build is running."
printf '\n'
printf 'Build dir: %s\n' "${BUILD_DIR}"
printf 'App PID: %s\n' "${APP_PID}"
printf 'Runtime dir: %s\n' "${RUNTIME_DIR}"
printf 'Sockets:\n'
printf '  %s\n' "${SOCKET_DIR}/indexer.sock"
printf '  %s\n' "${SOCKET_DIR}/extractor.sock"
printf '  %s\n' "${SOCKET_DIR}/query.sock"
printf '  %s\n' "${SOCKET_DIR}/inference.sock"
printf 'Logs: %s\n' "${LOG_DIR}"
printf 'Env file: %s\n' "${ENV_FILE}"
