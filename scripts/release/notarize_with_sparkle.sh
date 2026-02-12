#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

# Backward-compatible wrapper for existing CI/workflows.
# Routes to the unified release pipeline with Sparkle + signing defaults.

export BS_RELEASE_BUILD_DIR="${BS_RELEASE_BUILD_DIR:-$ROOT_DIR/build-release}"
export BS_RELEASE_OUTPUT_DIR="${BS_RELEASE_OUTPUT_DIR:-$ROOT_DIR/dist/notarization}"
export BS_RELEASE_BUILD_TYPE="${BS_RELEASE_BUILD_TYPE:-Release}"
export BS_ENTITLEMENTS_PATH="${BS_ENTITLEMENTS_PATH:-$ROOT_DIR/packaging/macos/entitlements.plist}"

export BS_ENABLE_SPARKLE="${BS_REQUIRE_SPARKLE:-1}"
export BS_CREATE_DMG="${BS_CREATE_DMG:-1}"
export BS_NOTARIZE="${BS_NOTARIZE:-1}"
export BS_SIGN=1

"$ROOT_DIR/scripts/release/build_macos_release.sh"
