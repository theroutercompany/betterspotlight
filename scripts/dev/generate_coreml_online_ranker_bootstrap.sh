#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
VENV_DIR_DEFAULT="${ROOT_DIR}/.build/coreml-bootstrap-venv"
OUTPUT_ROOT_DEFAULT="${ROOT_DIR}/data/models/online-ranker-v1/bootstrap"
GENERATOR_SCRIPT="${ROOT_DIR}/tools/generate_online_ranker_coreml.py"

PYTHON_BIN="${PYTHON:-python3}"
VENV_DIR="${BS_COREML_BOOTSTRAP_VENV:-${VENV_DIR_DEFAULT}}"
OUTPUT_ROOT="${BS_COREML_BOOTSTRAP_OUTPUT_ROOT:-${OUTPUT_ROOT_DEFAULT}}"
COREMLTOOLS_SPEC="${BS_COREMLTOOLS_SPEC:-coremltools==9.0}"
RECREATE_VENV=0
SKIP_INSTALL=0
KEEP_MLMODEL=0
GENERATOR_ARGS=()

usage() {
    cat <<'EOF'
Usage: scripts/dev/generate_coreml_online_ranker_bootstrap.sh [options] [-- <generator args>]

Creates or reuses a local Python virtualenv, installs the Core ML tooling
needed by tools/generate_online_ranker_coreml.py, and regenerates the
BetterSpotlight online-ranker bootstrap model.

Options:
  --output-root PATH    Bootstrap output directory
  --venv-dir PATH       Virtualenv path to create or reuse
  --python PATH         Python interpreter to use (default: python3)
  --recreate-venv       Delete and recreate the virtualenv before installing
  --skip-install        Reuse the current virtualenv without pip installing
  --keep-mlmodel        Preserve the intermediate .mlmodel artifact
  -h, --help            Show this help text

Any arguments after `--` are passed directly to
tools/generate_online_ranker_coreml.py.
EOF
}

log() {
    printf '[coreml-bootstrap] %s\n' "$*"
}

fail() {
    printf '[coreml-bootstrap] Error: %s\n' "$*" >&2
    exit 1
}

require_cmd() {
    local name="$1"
    command -v "${name}" >/dev/null 2>&1 || fail "required command not found: ${name}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --output-root)
            [[ $# -ge 2 ]] || fail "--output-root requires a path"
            OUTPUT_ROOT="$2"
            shift 2
            ;;
        --venv-dir)
            [[ $# -ge 2 ]] || fail "--venv-dir requires a path"
            VENV_DIR="$2"
            shift 2
            ;;
        --python)
            [[ $# -ge 2 ]] || fail "--python requires a path"
            PYTHON_BIN="$2"
            shift 2
            ;;
        --recreate-venv)
            RECREATE_VENV=1
            shift
            ;;
        --skip-install)
            SKIP_INSTALL=1
            shift
            ;;
        --keep-mlmodel)
            KEEP_MLMODEL=1
            shift
            ;;
        --)
            shift
            GENERATOR_ARGS+=("$@")
            break
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

[[ "$(uname -s)" == "Darwin" ]] || fail "this script currently supports macOS only"
require_cmd "${PYTHON_BIN}"
require_cmd xcrun
[[ -f "${GENERATOR_SCRIPT}" ]] || fail "generator script missing: ${GENERATOR_SCRIPT}"

if [[ "${RECREATE_VENV}" -eq 1 ]]; then
    rm -rf "${VENV_DIR}"
fi

if [[ ! -x "${VENV_DIR}/bin/python" ]]; then
    log "Creating virtualenv at ${VENV_DIR}"
    "${PYTHON_BIN}" -m venv "${VENV_DIR}"
fi

VENV_PYTHON="${VENV_DIR}/bin/python"
VENV_PIP="${VENV_DIR}/bin/pip"
[[ -x "${VENV_PYTHON}" ]] || fail "virtualenv python missing: ${VENV_PYTHON}"
[[ -x "${VENV_PIP}" ]] || fail "virtualenv pip missing: ${VENV_PIP}"

if [[ "${SKIP_INSTALL}" -eq 0 ]]; then
    log "Installing bootstrap toolchain into ${VENV_DIR}"
    "${VENV_PIP}" install --upgrade pip >/dev/null
    "${VENV_PIP}" install --upgrade "${COREMLTOOLS_SPEC}" >/dev/null
fi

OUTPUT_ROOT="$(cd "$(dirname "${OUTPUT_ROOT}")" && pwd)/$(basename "${OUTPUT_ROOT}")"
mkdir -p "${OUTPUT_ROOT}"

log "Generating CoreML online-ranker bootstrap into ${OUTPUT_ROOT}"
CMD=(
    "${VENV_PYTHON}"
    "${GENERATOR_SCRIPT}"
    --output-root "${OUTPUT_ROOT}"
)

if [[ "${KEEP_MLMODEL}" -eq 1 ]]; then
    CMD+=(--keep-mlmodel)
fi

if [[ "${#GENERATOR_ARGS[@]}" -gt 0 ]]; then
    CMD+=("${GENERATOR_ARGS[@]}")
fi

"${CMD[@]}"

MODEL_DIR="${OUTPUT_ROOT}/online_ranker_v1.mlmodelc"
METADATA_PATH="${OUTPUT_ROOT}/metadata.json"
[[ -d "${MODEL_DIR}" ]] || fail "compiled CoreML model missing after generation: ${MODEL_DIR}"
[[ -s "${METADATA_PATH}" ]] || fail "metadata.json missing after generation: ${METADATA_PATH}"

log "Generated bootstrap model:"
log "  model: ${MODEL_DIR}"
log "  metadata: ${METADATA_PATH}"
