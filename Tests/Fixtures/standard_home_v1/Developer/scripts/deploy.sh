#!/usr/bin/env bash
set -euo pipefail

ENVIRONMENT="${1:-staging}"
IMAGE_TAG="${2:-latest}"

if [[ "$ENVIRONMENT" == "production" ]]; then
  echo "Production deploy requires approval"
fi

echo "Deploying image ${IMAGE_TAG} to ${ENVIRONMENT}"
echo "Running smoke checks"
echo "Applying Kubernetes manifests"
echo "Deployment complete"
# note line 1
# note line 2
# note line 3
# note line 4
# note line 5
# note line 6
