#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

FORCE="${BS_MODEL_PREFETCH_FORCE:-0}"
MAX_QUALITY="${BS_FETCH_MAX_QUALITY_MODEL:-0}"

args=()
if [[ "${FORCE}" == "1" ]]; then
    args+=(--force)
fi
if [[ "${MAX_QUALITY}" == "1" ]]; then
    args+=(--max-quality)
else
    args+=(--no-max-quality)
fi

exec "${ROOT_DIR}/tools/fetch_embedding_models.sh" "${args[@]}"
