#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "Usage: $0 <artifact-path> [owner/repo]" >&2
    exit 2
fi

ARTIFACT_PATH="$1"
REPO="${2:-${GITHUB_REPOSITORY:-}}"

if [[ -z "${REPO}" ]]; then
    REMOTE_URL="$(git config --get remote.origin.url 2>/dev/null || true)"
    if [[ -n "${REMOTE_URL}" ]]; then
        REPO="$(echo "${REMOTE_URL}" | sed -E 's#^git@github.com:##; s#^https://github.com/##; s#\.git$##')"
    fi
fi

if [[ -z "${REPO}" ]]; then
    echo "Unable to determine GitHub repository. Pass [owner/repo] explicitly." >&2
    exit 1
fi

if ! command -v gh >/dev/null 2>&1; then
    echo "GitHub CLI (gh) is required to verify attestations." >&2
    exit 1
fi

gh attestation verify "${ARTIFACT_PATH}" --repo "${REPO}"
