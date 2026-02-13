#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORKFLOW_DIR="${ROOT_DIR}/.github/workflows"

if [[ ! -d "${WORKFLOW_DIR}" ]]; then
    echo "Workflow directory not found: ${WORKFLOW_DIR}" >&2
    exit 1
fi

violations=0

while IFS= read -r workflow; do
    line_no=0
    while IFS= read -r line; do
        line_no=$((line_no + 1))
        if [[ ! "${line}" =~ ^[[:space:]]*uses:[[:space:]]*([^[:space:]#]+) ]]; then
            continue
        fi

        ref="${BASH_REMATCH[1]}"

        # Local composite actions and docker images are allowed.
        if [[ "${ref}" == ./* || "${ref}" == docker://* ]]; then
            continue
        fi

        if [[ "${ref}" != *"@"* ]]; then
            continue
        fi

        version_ref="${ref##*@}"
        if [[ "${version_ref}" =~ ^[0-9a-fA-F]{40}$ ]]; then
            continue
        fi

        echo "Unpinned action reference: ${workflow}:${line_no} -> ${ref}" >&2
        violations=$((violations + 1))
    done < "${workflow}"
done < <(find "${WORKFLOW_DIR}" -type f \( -name '*.yml' -o -name '*.yaml' \) | sort)

if [[ "${violations}" -gt 0 ]]; then
    echo "Found ${violations} unpinned action reference(s). Pin every uses: to a full commit SHA." >&2
    exit 1
fi

echo "All workflow actions are SHA pinned."
