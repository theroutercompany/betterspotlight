#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODELS_DIR="${ROOT_DIR}/data/models"
LOCKFILE="${BS_MODEL_ASSET_LOCKFILE:-${ROOT_DIR}/tools/model_assets.lock.json}"
MIRROR_BASE_URL="${BS_MODEL_MIRROR_BASE_URL:-}"

FORCE=0
FETCH_MAX_QUALITY="${BETTERSPOTLIGHT_FETCH_MAX_QUALITY:-0}"
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
        *)
            echo "Unknown argument: ${arg}" >&2
            exit 2
            ;;
    esac
done

require_cmd() {
    local name="$1"
    if ! command -v "${name}" >/dev/null 2>&1; then
        echo "Missing required command: ${name}" >&2
        exit 1
    fi
}

require_cmd curl
require_cmd python3
require_cmd shasum

if [[ ! -f "${LOCKFILE}" ]]; then
    echo "Model lockfile not found: ${LOCKFILE}" >&2
    exit 1
fi

lookup_asset() {
    local asset_name="$1"
    python3 - "${LOCKFILE}" "${asset_name}" <<'PY'
import json
import sys

lockfile, asset_name = sys.argv[1:3]
with open(lockfile, "r", encoding="utf-8") as fh:
    payload = json.load(fh)

assets = payload.get("assets", [])
for asset in assets:
    if asset.get("name") == asset_name:
        required = ["name", "url", "sha256", "size_bytes", "version"]
        missing = [key for key in required if key not in asset]
        if missing:
            raise SystemExit(f"Asset '{asset_name}' is missing required fields: {', '.join(missing)}")
        print(
            "\t".join(
                [
                    str(asset["name"]),
                    str(asset["url"]),
                    str(asset["sha256"]).lower(),
                    str(asset["size_bytes"]),
                    str(asset["version"]),
                ]
            )
        )
        break
else:
    raise SystemExit(f"Asset '{asset_name}' not found in lockfile")
PY
}

resolve_url() {
    local name="$1"
    local lock_url="$2"
    local version="$3"
    if [[ -n "${MIRROR_BASE_URL}" ]]; then
        local clean_base="${MIRROR_BASE_URL%/}"
        printf "%s/%s/%s" "${clean_base}" "${version}" "${name}"
        return
    fi
    printf "%s" "${lock_url}"
}

verify_file() {
    local path="$1"
    local expected_sha="$2"
    local expected_size="$3"

    local size
    if size="$(stat -f%z "${path}" 2>/dev/null)"; then
        :
    else
        size="$(wc -c < "${path}" | tr -d '[:space:]')"
    fi
    if [[ "${size}" != "${expected_size}" ]]; then
        echo "Size mismatch for $(basename "${path}"): expected ${expected_size}, got ${size}" >&2
        return 1
    fi

    local actual_sha
    actual_sha="$(shasum -a 256 "${path}" | awk '{print tolower($1)}')"
    if [[ "${actual_sha}" != "${expected_sha}" ]]; then
        echo "SHA256 mismatch for $(basename "${path}")" >&2
        echo "  expected: ${expected_sha}" >&2
        echo "  actual:   ${actual_sha}" >&2
        return 1
    fi

    return 0
}

download_and_verify() {
    local asset_name="$1"
    local target_path="$2"

    local metadata
    metadata="$(lookup_asset "${asset_name}")"

    local lock_name lock_url expected_sha expected_size version
    IFS=$'\t' read -r lock_name lock_url expected_sha expected_size version <<<"${metadata}"

    local resolved_url
    resolved_url="$(resolve_url "${lock_name}" "${lock_url}" "${version}")"

    if [[ ${FORCE} -eq 0 && -s "${target_path}" ]]; then
        if verify_file "${target_path}" "${expected_sha}" "${expected_size}"; then
            echo "Using existing ${lock_name} (hash verified)"
            return
        fi
        echo "Existing ${lock_name} failed verification; re-downloading..."
    fi

    local tmp="${target_path}.tmp"
    rm -f "${tmp}"
    curl -fL --retry 3 --retry-delay 2 --connect-timeout 20 --max-time 1800 \
        "${resolved_url}" -o "${tmp}"

    verify_file "${tmp}" "${expected_sha}" "${expected_size}"
    mv "${tmp}" "${target_path}"
    echo "Downloaded ${lock_name} (${expected_size} bytes, sha256 verified)"
}

mkdir -p "${MODELS_DIR}"
download_and_verify "bge-small-en-v1.5-int8.onnx" "${MODELS_DIR}/bge-small-en-v1.5-int8.onnx"
download_and_verify "vocab.txt" "${MODELS_DIR}/vocab.txt"
if [[ "${FETCH_MAX_QUALITY}" == "1" ]]; then
    download_and_verify "bge-large-en-v1.5-f32.onnx" "${MODELS_DIR}/bge-large-en-v1.5-f32.onnx"
else
    echo "Skipping 1024d model download (--max-quality to enable or --no-max-quality to force skip)"
fi

echo
echo "Embedding assets ready:"
if [[ -f "${MODELS_DIR}/bge-large-en-v1.5-f32.onnx" ]]; then
    ls -lh "${MODELS_DIR}/bge-small-en-v1.5-int8.onnx" \
        "${MODELS_DIR}/bge-large-en-v1.5-f32.onnx" \
        "${MODELS_DIR}/vocab.txt"
else
    ls -lh "${MODELS_DIR}/bge-small-en-v1.5-int8.onnx" \
        "${MODELS_DIR}/vocab.txt"
fi
