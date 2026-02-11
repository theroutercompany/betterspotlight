#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODELS_DIR="${ROOT_DIR}/data/models"
LEGACY_ONNX_OUT="${MODELS_DIR}/bge-small-en-v1.5-int8.onnx"
HQ_ONNX_OUT="${MODELS_DIR}/bge-large-en-v1.5-f32.onnx"
VOCAB_OUT="${MODELS_DIR}/vocab.txt"

LEGACY_ONNX_URL="https://huggingface.co/Xenova/bge-small-en-v1.5/resolve/main/onnx/model_int8.onnx"
HQ_ONNX_URL="https://huggingface.co/Xenova/bge-large-en-v1.5/resolve/main/onnx/model.onnx"
VOCAB_URL="https://huggingface.co/Xenova/bge-small-en-v1.5/resolve/main/vocab.txt"

FORCE=0
FETCH_MAX_QUALITY="${BETTERSPOTLIGHT_FETCH_MAX_QUALITY:-1}"
for arg in "$@"; do
    case "${arg}" in
        --force)
            FORCE=1
            ;;
        --max-quality)
            FETCH_MAX_QUALITY=1
            ;;
        --no-max-quality)
            FETCH_MAX_QUALITY=0
            ;;
    esac
done

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
download_file "${LEGACY_ONNX_URL}" "${LEGACY_ONNX_OUT}" 1000000
download_file "${VOCAB_URL}" "${VOCAB_OUT}" 10000
if [[ "${FETCH_MAX_QUALITY}" == "1" ]]; then
    download_file "${HQ_ONNX_URL}" "${HQ_ONNX_OUT}" 100000000
else
    echo "Skipping 1024d model download (--max-quality to enable or --no-max-quality to force skip)"
fi

echo
echo "Embedding assets ready:"
if [[ -f "${HQ_ONNX_OUT}" ]]; then
    ls -lh "${LEGACY_ONNX_OUT}" "${HQ_ONNX_OUT}" "${VOCAB_OUT}"
else
    ls -lh "${LEGACY_ONNX_OUT}" "${VOCAB_OUT}"
fi
