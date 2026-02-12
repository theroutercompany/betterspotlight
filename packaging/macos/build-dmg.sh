#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

export BS_RELEASE_BUILD_DIR="${BS_RELEASE_BUILD_DIR:-$ROOT_DIR/build-release}"
export BS_RELEASE_OUTPUT_DIR="${BS_RELEASE_OUTPUT_DIR:-$ROOT_DIR/dist/release}"
export BS_RELEASE_BUILD_TYPE="${BS_RELEASE_BUILD_TYPE:-Release}"

export BS_ENABLE_SPARKLE="${BS_ENABLE_SPARKLE:-0}"
export BS_SIGN=0
export BS_NOTARIZE=0
export BS_CREATE_DMG=1

"$ROOT_DIR/scripts/release/build_macos_release.sh"
