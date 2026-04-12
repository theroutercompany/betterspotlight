#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SETUP_SCRIPT="${ROOT_DIR}/scripts/dev/setup_env.sh"
IPC_CALL_SCRIPT="${ROOT_DIR}/scripts/dev/ipc_call.py"
ENV_FILE="${ROOT_DIR}/.build/dev-env.sh"
RUNTIME_ROOT_DIR="${ROOT_DIR}/.build/dev-runtime"
BUILD_DIR_DEFAULT="${ROOT_DIR}/build-dev"

BUILD_DIR="${BS_DEV_BUILD_DIR:-${BUILD_DIR_DEFAULT}}"
BUILD_TYPE="${BS_DEV_BUILD_TYPE:-Release}"
CLEAN_BUILD=1
FETCH_BOOTSTRAP_MODELS=1
BUILD_PIPELINE_TESTS=0
ALLOW_DEGRADED_PDF=0

DATA_DIR=""
SHARED_USER_DATA=0
START_INDEX=0
REBUILD_VECTORS=0
WAIT_INDEX=0
WAIT_EMBED=0
ROOTS=()
VECTOR_REBUILD_EXPECTED=0

SERVICE_READY_TIMEOUT_SEC="${BS_DEV_SERVICE_READY_TIMEOUT_SEC:-30}"
INFERENCE_READY_TIMEOUT_SEC="${BS_DEV_INFERENCE_READY_TIMEOUT_SEC:-120}"
APP_READY_TIMEOUT_SEC="${BS_DEV_APP_READY_TIMEOUT_SEC:-45}"
INDEX_WAIT_TIMEOUT_SEC="${BS_DEV_INDEX_WAIT_TIMEOUT_SEC:-3600}"
EMBED_WAIT_TIMEOUT_SEC="${BS_DEV_EMBED_WAIT_TIMEOUT_SEC:-3600}"
LAST_IPC_ERROR=""

usage() {
    cat <<'EOF'
Usage: scripts/dev/build_launch.sh [options]

Builds and launches BetterSpotlight in an isolated runtime by default.

Core options:
  --build-dir PATH           Build directory (default: ./build-dev)
  --build-type TYPE          Build type (default: Release)
  --no-clean                 Reuse existing build dir
  --skip-model-fetch         Do not auto-fetch bootstrap embedding models
  --with-tests               Build full default verification target

Manual-run options:
  --data-dir PATH            Use explicit BETTERSPOTLIGHT_DATA_DIR
  --shared-user-data         Opt in to shared user data dir (non-hermetic)
  --roots A,B,C              Index roots for --start-index/--rebuild-vectors (repeatable)
  --start-index              Trigger indexer startIndexing after launch
  --rebuild-vectors          Trigger query rebuildVectorIndex after launch
  --wait-index               Wait for indexer queue drain
  --wait-embed               Wait for vector rebuild completion
  --allow-degraded-pdf       Allow unsupported no-PDF local forensics mode

Notes:
  - Isolated data dir is the default behavior.
  - --shared-user-data and --data-dir are mutually exclusive.
EOF
}

log() {
    printf '[dev-launch] %s\n' "$*"
}

fail() {
    printf '[dev-launch] Error: %s\n' "$*" >&2
    exit 1
}

trim() {
    local value="$1"
    value="${value#"${value%%[![:space:]]*}"}"
    value="${value%"${value##*[![:space:]]}"}"
    printf '%s' "${value}"
}

array_contains() {
    local needle="$1"
    shift
    local item
    for item in "$@"; do
        if [[ "${item}" == "${needle}" ]]; then
            return 0
        fi
    done
    return 1
}

append_roots_arg() {
    local raw="$1"
    local part parsed
    IFS=',' read -r -a parsed <<< "${raw}"
    for part in "${parsed[@]}"; do
        part="$(trim "${part}")"
        [[ -n "${part}" ]] || continue
        if ! array_contains "${part}" "${ROOTS[@]}"; then
            ROOTS+=("${part}")
        fi
    done
}

pid_cmdline() {
    local pid="$1"
    ps -p "${pid}" -o args= 2>/dev/null || true
}

pid_owned_by_session() {
    local pid="$1"
    local app_binary="$2"
    local helpers_dir="$3"
    local cmd
    cmd="$(pid_cmdline "${pid}")"
    [[ -n "${cmd}" ]] || return 1

    if [[ -n "${app_binary}" && "${cmd}" == *"${app_binary}"* ]]; then
        return 0
    fi
    if [[ -n "${helpers_dir}" && "${cmd}" == *"${helpers_dir}/betterspotlight-"* ]]; then
        return 0
    fi
    return 1
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

kill_if_owned() {
    local pid="$1"
    local app_binary="$2"
    local helpers_dir="$3"
    [[ -n "${pid}" ]] || return 0
    if ! kill -0 "${pid}" 2>/dev/null; then
        return 0
    fi
    if ! pid_owned_by_session "${pid}" "${app_binary}" "${helpers_dir}"; then
        log "Skipping pid ${pid}; command no longer matches previous BetterSpotlight session"
        return 0
    fi
    kill_if_alive "${pid}"
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

json_array_from_args() {
    python3 - "$@" <<'PY'
import json
import sys
print(json.dumps(sys.argv[1:]))
PY
}

ipc_request_json() {
    local socket_path="$1"
    local method="$2"
    local params_json="${3-}"
    local timeout_sec="${4:-2}"
    if [[ -z "${params_json}" ]]; then
        params_json="{}"
    fi
    python3 "${IPC_CALL_SCRIPT}" \
        --socket "${socket_path}" \
        --method "${method}" \
        --params "${params_json}" \
        --timeout-ms "$(( timeout_sec * 1000 ))"
}

capture_ipc_response() {
    local output_file="$1"
    local socket_path="$2"
    local method="$3"
    local params_json="${4-}"
    local timeout_sec="${5:-2}"
    local error_file
    error_file="$(mktemp "${RUNTIME_ROOT_DIR}/ipc-error.XXXXXX")"
    if [[ -z "${params_json}" ]]; then
        params_json="{}"
    fi

    if ipc_request_json "${socket_path}" "${method}" "${params_json}" "${timeout_sec}" \
        >"${output_file}" 2>"${error_file}"; then
        LAST_IPC_ERROR=""
        rm -f "${error_file}"
        return 0
    fi

    LAST_IPC_ERROR="$(tr '\n' ' ' <"${error_file}" | sed 's/[[:space:]]\\+/ /g')"
    rm -f "${error_file}"
    : > "${output_file}"
    return 1
}

is_ipc_success_response() {
    local response_json="$1"
    python3 - "${response_json}" <<'PY'
import json
import sys

payload = json.loads(sys.argv[1])
if payload.get("type") == "error":
    raise SystemExit(1)
if payload.get("type") != "response":
    raise SystemExit(1)
raise SystemExit(0)
PY
}

wait_for_service_ping() {
    local name="$1"
    local socket_path="$2"
    local timeout_sec="$3"
    local elapsed=0
    local response_file
    response_file="$(mktemp "${RUNTIME_ROOT_DIR}/ipc-ping.${name}.XXXXXX")"

    while (( elapsed < timeout_sec )); do
        local response=""
        if capture_ipc_response "${response_file}" "${socket_path}" "ping" "{}" "1"; then
            response="$(<"${response_file}")"
        fi
        if [[ -n "${response}" ]] && is_ipc_success_response "${response}"; then
            if python3 - "${response}" <<'PY'
import json
import sys
p = json.loads(sys.argv[1])
ok = bool(p.get("result", {}).get("pong"))
raise SystemExit(0 if ok else 1)
PY
            then
                rm -f "${response_file}"
                return 0
            fi
        fi
        sleep 1
        elapsed=$((elapsed + 1))
    done

    rm -f "${response_file}"
    return 1
}

wait_for_query_health() {
    local socket_path="$1"
    local timeout_sec="$2"
    local elapsed=0
    local response_file
    response_file="$(mktemp "${RUNTIME_ROOT_DIR}/ipc-query-health.XXXXXX")"

    while (( elapsed < timeout_sec )); do
        local response=""
        if capture_ipc_response "${response_file}" "${socket_path}" "getQueryHealthV3" "{}" "2"; then
            response="$(<"${response_file}")"
        fi
        if [[ -n "${response}" ]] && is_ipc_success_response "${response}"; then
            rm -f "${response_file}"
            return 0
        fi
        sleep 1
        elapsed=$((elapsed + 1))
    done
    rm -f "${response_file}"
    return 1
}

inference_roles_ready() {
    local response_json="$1"
    python3 - "${response_json}" <<'PY'
import json
import sys

payload = json.loads(sys.argv[1]).get("result", {})
if not payload.get("connected", False):
    raise SystemExit(1)
roles = payload.get("roleStatusByModel", {})
ready = (
    roles.get("bi-encoder") == "ready"
    and roles.get("cross-encoder") == "ready"
)
raise SystemExit(0 if ready else 1)
PY
}

wait_for_inference_core_roles() {
    local socket_path="$1"
    local timeout_sec="$2"
    local elapsed=0
    local response_file
    response_file="$(mktemp "${RUNTIME_ROOT_DIR}/ipc-inference-health.XXXXXX")"

    while (( elapsed < timeout_sec )); do
        local response=""
        if capture_ipc_response "${response_file}" "${socket_path}" "get_inference_health" "{}" "3"; then
            response="$(<"${response_file}")"
        fi
        if [[ -n "${response}" ]] && is_ipc_success_response "${response}" \
            && inference_roles_ready "${response}"; then
            rm -f "${response_file}"
            return 0
        fi
        sleep 1
        elapsed=$((elapsed + 1))
    done
    rm -f "${response_file}"
    return 1
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

stop_previous_session() {
    local state_file="${RUNTIME_ROOT_DIR}/current-session.sh"
    local previous_app_pid=""
    local previous_service_pids=""
    local previous_app_binary=""
    local previous_helpers_dir=""

    if [[ -f "${state_file}" ]]; then
        previous_app_pid="$(grep -E '^export APP_PID=' "${state_file}" | head -n 1 | cut -d'"' -f2 || true)"
        previous_service_pids="$(grep -E '^export SERVICE_PIDS=' "${state_file}" | head -n 1 | cut -d'"' -f2 || true)"
        previous_app_binary="$(grep -E '^export APP_BINARY=' "${state_file}" | head -n 1 | cut -d'"' -f2 || true)"
        previous_helpers_dir="$(grep -E '^export HELPERS_DIR=' "${state_file}" | head -n 1 | cut -d'"' -f2 || true)"

        kill_if_owned "${previous_app_pid}" "${previous_app_binary}" "${previous_helpers_dir}"
        for pid in ${previous_service_pids}; do
            kill_if_owned "${pid}" "${previous_app_binary}" "${previous_helpers_dir}"
        done
    fi
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
export DATA_DIR="${DATA_DIR}"
export BETTERSPOTLIGHT_DATA_DIR="${DATA_DIR}"
EOF
    chmod +x "${state_file}"
}

launch_service() {
    local name="$1"
    local binary="$2"
    local wait_timeout="$3"
    local log_file="${LOG_DIR}/${name}.log"
    local socket_path="${SOCKET_DIR}/${name}.sock"

    [[ -x "${binary}" ]] || fail "service binary is not executable: ${binary}"

    local common_env=(
        "BETTERSPOTLIGHT_INSTANCE_ID=${INSTANCE_ID}"
        "BETTERSPOTLIGHT_RUNTIME_DIR=${RUNTIME_DIR}"
        "BETTERSPOTLIGHT_SOCKET_DIR=${SOCKET_DIR}"
        "BETTERSPOTLIGHT_PID_DIR=${PID_DIR}"
        "BETTERSPOTLIGHT_MODELS_DIR=${BETTERSPOTLIGHT_MODELS_DIR}"
        "QT_PLUGIN_PATH=${QT_PLUGIN_PATH}"
        "QML_IMPORT_PATH=${QML_IMPORT_PATH}"
        "QML2_IMPORT_PATH=${QML2_IMPORT_PATH}"
    )
    if [[ -n "${DATA_DIR}" ]]; then
        common_env+=("BETTERSPOTLIGHT_DATA_DIR=${DATA_DIR}")
    fi

    nohup env "${common_env[@]}" "${binary}" >"${log_file}" 2>&1 < /dev/null &
    local pid=$!
    SERVICE_PIDS+=" ${pid}"

    if ! wait_for_path "${socket_path}" "${wait_timeout}"; then
        tail_log_on_failure "${log_file}"
        fail "service '${name}' did not create ${socket_path}"
    fi

    if ! wait_for_service_ping "${name}" "${socket_path}" "${wait_timeout}"; then
        tail_log_on_failure "${log_file}"
        if [[ -n "${LAST_IPC_ERROR}" ]]; then
            log "Last IPC probe error for ${name}: ${LAST_IPC_ERROR}"
        fi
        fail "service '${name}' did not respond to ping in ${wait_timeout}s"
    fi
}

start_indexing_if_requested() {
    [[ "${START_INDEX}" -eq 1 ]] || return 0
    [[ "${#ROOTS[@]}" -gt 0 ]] || fail "--start-index requires explicit --roots"

    local roots_json params_json response
    roots_json="$(json_array_from_args "${ROOTS[@]}")"
    params_json="$(python3 - "${roots_json}" <<'PY'
import json
import sys
roots = json.loads(sys.argv[1])
print(json.dumps({"roots": roots}, separators=(",", ":")))
PY
)"

    local response_file
    response_file="$(mktemp "${RUNTIME_ROOT_DIR}/ipc-start-index.XXXXXX")"
    response=""
    if capture_ipc_response "${response_file}" "${SOCKET_DIR}/indexer.sock" "startIndexing" \
        "${params_json}" "8"; then
        response="$(<"${response_file}")"
    fi
    rm -f "${response_file}"
    [[ -n "${response}" ]] || fail "startIndexing request failed"

    if ! is_ipc_success_response "${response}"; then
        if python3 - "${response}" <<'PY'
import json
import sys
err = json.loads(sys.argv[1]).get("error", {})
msg = (err.get("message") or "").lower()
raise SystemExit(0 if "already running" in msg else 1)
PY
        then
            log "Indexer reported already running; continuing"
            return 0
        fi
        fail "startIndexing failed: ${response}"
    fi

    log "Indexer startIndexing accepted for ${#ROOTS[@]} root(s)"
}

wait_for_index_completion_if_requested() {
    [[ "${WAIT_INDEX}" -eq 1 ]] || return 0
    local start_ts now elapsed response
    local pending processing preparing writing rebuild_running
    start_ts="$(date +%s)"

    while true; do
        local response_file
        response_file="$(mktemp "${RUNTIME_ROOT_DIR}/ipc-queue-status.XXXXXX")"
        response=""
        if capture_ipc_response "${response_file}" "${SOCKET_DIR}/indexer.sock" \
            "getQueueStatus" "{}" "4"; then
            response="$(<"${response_file}")"
        fi
        if [[ -n "${response}" ]] && is_ipc_success_response "${response}"; then
            read -r pending processing preparing writing rebuild_running < <(
                python3 - "${response}" <<'PY'
import json
import sys
res = json.loads(sys.argv[1]).get("result", {})
print(
    int(res.get("pending", 0)),
    int(res.get("processing", 0)),
    int(res.get("preparing", 0)),
    int(res.get("writing", 0)),
    int(bool(res.get("rebuildRunning", False))),
)
PY
            )
            if [[ "${pending}" -eq 0 && "${processing}" -eq 0 \
                && "${preparing}" -eq 0 && "${writing}" -eq 0 \
                && "${rebuild_running}" -eq 0 ]]; then
                rm -f "${response_file}"
                log "Indexer queue drained"
                return 0
            fi
        fi

        rm -f "${response_file}"

        now="$(date +%s)"
        elapsed=$((now - start_ts))
        if (( elapsed >= INDEX_WAIT_TIMEOUT_SEC )); then
            fail "Timed out waiting for index queue drain (${INDEX_WAIT_TIMEOUT_SEC}s)"
        fi
        sleep 1
    done
}

trigger_vector_rebuild_if_requested() {
    [[ "${REBUILD_VECTORS}" -eq 1 ]] || return 0

    local params_json response
    if [[ "${#ROOTS[@]}" -gt 0 ]]; then
        local roots_json
        roots_json="$(json_array_from_args "${ROOTS[@]}")"
        params_json="$(python3 - "${roots_json}" <<'PY'
import json
import sys
roots = json.loads(sys.argv[1])
print(json.dumps({"includePaths": roots}, separators=(",", ":")))
PY
)"
    else
        params_json="{}"
    fi

    local response_file
    response_file="$(mktemp "${RUNTIME_ROOT_DIR}/ipc-rebuild-vectors.XXXXXX")"
    response=""
    if capture_ipc_response "${response_file}" "${SOCKET_DIR}/query.sock" "rebuildVectorIndex" \
        "${params_json}" "8"; then
        response="$(<"${response_file}")"
    fi
    rm -f "${response_file}"
    [[ -n "${response}" ]] || fail "rebuildVectorIndex request failed"

    if ! is_ipc_success_response "${response}"; then
        fail "rebuildVectorIndex failed: ${response}"
    fi

    VECTOR_REBUILD_EXPECTED=1
    log "Query vector rebuild request accepted"
}

wait_for_vector_rebuild_if_requested() {
    [[ "${WAIT_EMBED}" -eq 1 ]] || return 0

    local start_ts now elapsed response
    local status progress
    start_ts="$(date +%s)"

    while true; do
        local response_file
        response_file="$(mktemp "${RUNTIME_ROOT_DIR}/ipc-embed-status.XXXXXX")"
        response=""
        if capture_ipc_response "${response_file}" "${SOCKET_DIR}/query.sock" \
            "getQueryHealthV3" "{}" "4"; then
            response="$(<"${response_file}")"
        fi
        if [[ -n "${response}" ]] && is_ipc_success_response "${response}"; then
            read -r status progress < <(
                python3 - "${response}" <<'PY'
import json
import sys
index_health = json.loads(sys.argv[1]).get("result", {}).get("indexHealth", {})
status = index_health.get("vectorRebuildStatus", "unknown")
progress = index_health.get("vectorRebuildProgressPct", 0.0)
print(status, progress)
PY
            )

            if [[ "${status}" == "succeeded" ]]; then
                rm -f "${response_file}"
                log "Vector rebuild completed successfully"
                return 0
            fi
            if [[ "${status}" == "failed" || "${status}" == "aborted" || "${status}" == "cancelled" ]]; then
                rm -f "${response_file}"
                fail "Vector rebuild ended with status '${status}'"
            fi
            if [[ "${VECTOR_REBUILD_EXPECTED}" -eq 0 && "${status}" == "idle" ]]; then
                rm -f "${response_file}"
                log "Vector rebuild idle; nothing to wait for"
                return 0
            fi
        fi

        rm -f "${response_file}"

        now="$(date +%s)"
        elapsed=$((now - start_ts))
        if (( elapsed >= EMBED_WAIT_TIMEOUT_SEC )); then
            fail "Timed out waiting for vector rebuild completion (${EMBED_WAIT_TIMEOUT_SEC}s)"
        fi
        sleep 1
    done
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
        --data-dir)
            [[ $# -ge 2 ]] || fail "--data-dir requires a path"
            DATA_DIR="$2"
            shift 2
            ;;
        --shared-user-data)
            SHARED_USER_DATA=1
            shift
            ;;
        --roots)
            [[ $# -ge 2 ]] || fail "--roots requires a value"
            append_roots_arg "$2"
            shift 2
            ;;
        --start-index)
            START_INDEX=1
            shift
            ;;
        --rebuild-vectors)
            REBUILD_VECTORS=1
            shift
            ;;
        --wait-index)
            WAIT_INDEX=1
            shift
            ;;
        --wait-embed)
            WAIT_EMBED=1
            shift
            ;;
        --allow-degraded-pdf)
            ALLOW_DEGRADED_PDF=1
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

if [[ "${SHARED_USER_DATA}" -eq 1 && -n "${DATA_DIR}" ]]; then
    fail "--shared-user-data cannot be used together with --data-dir"
fi

if [[ "${START_INDEX}" -eq 1 && "${#ROOTS[@]}" -eq 0 ]]; then
    fail "--start-index requires one or more --roots"
fi

if [[ "${START_INDEX}" -eq 0 && "${WAIT_INDEX}" -eq 1 && "${#ROOTS[@]}" -gt 0 ]]; then
    log "--wait-index set without --start-index; waiting on currently active indexer work"
fi

if [[ "${REBUILD_VECTORS}" -eq 0 && "${WAIT_EMBED}" -eq 1 ]]; then
    log "--wait-embed set without --rebuild-vectors; waiting only if rebuild is already running"
fi

[[ -x "${SETUP_SCRIPT}" ]] || fail "setup script is missing or not executable: ${SETUP_SCRIPT}"

SETUP_ARGS=(--write-env-file "${ENV_FILE}")
if [[ "${ALLOW_DEGRADED_PDF}" -eq 1 ]]; then
    SETUP_ARGS+=(--allow-degraded-pdf)
fi
"${SETUP_SCRIPT}" "${SETUP_ARGS[@]}"
# shellcheck disable=SC1090
source "${ENV_FILE}"

if [[ "${ALLOW_DEGRADED_PDF}" -eq 0 && "${BS_DEV_PDF_CAPABILITY:-degraded}" != "available" ]]; then
    fail "Supported manual-run profile requires PDF capability; rerun with --allow-degraded-pdf only for unsupported local forensics"
fi

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
if [[ "${BUILD_PIPELINE_TESTS}" -eq 1 ]]; then
    BUILD_TARGETS=(betterspotlight-default-verification)
else
    BUILD_TARGETS=(betterspotlight-dev-runtime)
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

if [[ "${SHARED_USER_DATA}" -eq 1 ]]; then
    DATA_DIR=""
else
    if [[ -z "${DATA_DIR}" ]]; then
        DATA_DIR="${RUNTIME_ROOT_DIR}/${INSTANCE_ID}/data"
    fi
    mkdir -p "${DATA_DIR}"
fi

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
launch_service "indexer" "${HELPERS_DIR}/betterspotlight-indexer" "${SERVICE_READY_TIMEOUT_SEC}"
launch_service "extractor" "${HELPERS_DIR}/betterspotlight-extractor" "${SERVICE_READY_TIMEOUT_SEC}"
launch_service "query" "${HELPERS_DIR}/betterspotlight-query" "${SERVICE_READY_TIMEOUT_SEC}"
launch_service "inference" "${HELPERS_DIR}/betterspotlight-inference" "${INFERENCE_READY_TIMEOUT_SEC}"

if ! wait_for_query_health "${SOCKET_DIR}/query.sock" "${SERVICE_READY_TIMEOUT_SEC}"; then
    tail_log_on_failure "${LOG_DIR}/query.log"
    fail "query service health endpoint did not become available"
fi
if ! wait_for_inference_core_roles "${SOCKET_DIR}/inference.sock" "${INFERENCE_READY_TIMEOUT_SEC}"; then
    tail_log_on_failure "${LOG_DIR}/inference.log"
    fail "inference service did not report required core roles ready"
fi

log "Launching BetterSpotlight app..."
common_app_env=(
    "BETTERSPOTLIGHT_INSTANCE_ID=${INSTANCE_ID}"
    "BETTERSPOTLIGHT_RUNTIME_DIR=${RUNTIME_DIR}"
    "BETTERSPOTLIGHT_SOCKET_DIR=${SOCKET_DIR}"
    "BETTERSPOTLIGHT_PID_DIR=${PID_DIR}"
    "BETTERSPOTLIGHT_MODELS_DIR=${BETTERSPOTLIGHT_MODELS_DIR}"
    "QT_PLUGIN_PATH=${QT_PLUGIN_PATH}"
    "QML_IMPORT_PATH=${QML_IMPORT_PATH}"
    "QML2_IMPORT_PATH=${QML2_IMPORT_PATH}"
)
if [[ -n "${DATA_DIR}" ]]; then
    common_app_env+=("BETTERSPOTLIGHT_DATA_DIR=${DATA_DIR}")
fi
nohup env "${common_app_env[@]}" "${APP_BINARY}" >"${APP_LOG}" 2>&1 < /dev/null &
APP_PID=$!

if ! wait_for_log_line "${APP_LOG}" "BetterSpotlight ready" "${APP_READY_TIMEOUT_SEC}"; then
    if ! kill -0 "${APP_PID}" 2>/dev/null; then
        tail_log_on_failure "${APP_LOG}"
        fail "app exited before reaching ready state"
    fi
    tail_log_on_failure "${APP_LOG}"
    fail "app did not report readiness within ${APP_READY_TIMEOUT_SEC}s"
fi

if ! kill -0 "${APP_PID}" 2>/dev/null; then
    tail_log_on_failure "${APP_LOG}"
    fail "app reached ready state but did not remain alive"
fi

if grep -Fq "QObject::startTimer: Timers cannot be started from another thread" "${APP_LOG}"; then
    tail_log_on_failure "${APP_LOG}"
    fail "app log reported cross-thread timer startup"
fi

if ! wait_for_query_health "${SOCKET_DIR}/query.sock" "${SERVICE_READY_TIMEOUT_SEC}"; then
    tail_log_on_failure "${LOG_DIR}/query.log"
    fail "query service health probe failed after app launch"
fi
if ! wait_for_inference_core_roles "${SOCKET_DIR}/inference.sock" "${INFERENCE_READY_TIMEOUT_SEC}"; then
    tail_log_on_failure "${LOG_DIR}/inference.log"
    fail "inference core roles are not ready after app launch"
fi

for pid in ${SERVICE_PIDS}; do
    if ! kill -0 "${pid}" 2>/dev/null; then
        fail "helper service pid ${pid} exited after launch"
    fi
done

start_indexing_if_requested
wait_for_index_completion_if_requested
trigger_vector_rebuild_if_requested
wait_for_vector_rebuild_if_requested

write_session_state
trap - ERR INT TERM

log "BetterSpotlight dev build is running."
printf '\n'
printf 'Build dir: %s\n' "${BUILD_DIR}"
printf 'App PID: %s\n' "${APP_PID}"
printf 'Runtime dir: %s\n' "${RUNTIME_DIR}"
if [[ -n "${DATA_DIR}" ]]; then
    printf 'Data dir: %s\n' "${DATA_DIR}"
else
    printf 'Data dir: shared user data (opt-in)\n'
fi
if [[ "${#ROOTS[@]}" -gt 0 ]]; then
    printf 'Roots:\n'
    for root in "${ROOTS[@]}"; do
        printf '  %s\n' "${root}"
    done
fi
printf 'Sockets:\n'
printf '  %s\n' "${SOCKET_DIR}/indexer.sock"
printf '  %s\n' "${SOCKET_DIR}/extractor.sock"
printf '  %s\n' "${SOCKET_DIR}/query.sock"
printf '  %s\n' "${SOCKET_DIR}/inference.sock"
printf 'Logs: %s\n' "${LOG_DIR}"
printf 'Env file: %s\n' "${ENV_FILE}"
