#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODELS_DIR="${ROOT_DIR}/data/models"
ONNX_OUT="${MODELS_DIR}/bge-small-en-v1.5-int8.onnx"
VOCAB_OUT="${MODELS_DIR}/vocab.txt"

ONNX_URL="https://huggingface.co/Xenova/bge-small-en-v1.5/resolve/main/onnx/model_int8.onnx"
VOCAB_URL="https://huggingface.co/Xenova/bge-small-en-v1.5/resolve/main/vocab.txt"

FORCE=0
if [[ "${1:-}" == "--force" ]]; then
    FORCE=1
fi

download_file() {
    local url="$1"
    local out="$2"
    local min_bytes="$3"

    if [[ ${FORCE} -eq 0 && -s "${out}" ]]; then
        local size
        size="$(wc -c < "${out}")"
        if [[ "${size}" -ge "${min_bytes}" ]]; then
            echo "Using existing $(basename "${out}") (${size} bytes)"
            return
        fi
        echo "Existing $(basename "${out}") is too small (${size} bytes), re-downloading..."
    fi

    local tmp="${out}.tmp"
    rm -f "${tmp}"
    curl -fL --retry 3 --retry-delay 2 --connect-timeout 20 --max-time 1800 \
        "${url}" -o "${tmp}"

    local downloaded_size
    downloaded_size="$(wc -c < "${tmp}")"
    if [[ "${downloaded_size}" -lt "${min_bytes}" ]]; then
        rm -f "${tmp}"
        echo "Downloaded $(basename "${out}") is unexpectedly small (${downloaded_size} bytes)" >&2
        exit 1
    fi

    mv "${tmp}" "${out}"
    echo "Downloaded $(basename "${out}") (${downloaded_size} bytes)"
}

mkdir -p "${MODELS_DIR}"
download_file "${ONNX_URL}" "${ONNX_OUT}" 1000000
download_file "${VOCAB_URL}" "${VOCAB_OUT}" 10000

echo
echo "Embedding assets ready:"
ls -lh "${ONNX_OUT}" "${VOCAB_OUT}"
