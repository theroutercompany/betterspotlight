#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BS_RELEASE_BUILD_DIR:-$ROOT_DIR/build-release}"
OUTPUT_DIR="${BS_RELEASE_OUTPUT_DIR:-$ROOT_DIR/dist/notarization}"
ENTITLEMENTS_PATH="${BS_ENTITLEMENTS_PATH:-$ROOT_DIR/packaging/macos/entitlements.plist}"
BUILD_TYPE="${BS_RELEASE_BUILD_TYPE:-Release}"
NOTARIZE="${BS_NOTARIZE:-1}"
REQUIRE_SPARKLE="${BS_REQUIRE_SPARKLE:-1}"
CREATE_DMG="${BS_CREATE_DMG:-1}"
SKIP_BUILD="${BS_SKIP_BUILD:-0}"

APP_PATH="$BUILD_DIR/src/app/betterspotlight.app"
APP_BINARY="$APP_PATH/Contents/MacOS/betterspotlight"
ZIP_PATH="$OUTPUT_DIR/betterspotlight.app.zip"
APP_NOTARY_JSON="$OUTPUT_DIR/notary_app.json"
DMG_NOTARY_JSON="$OUTPUT_DIR/notary_dmg.json"
SUMMARY_JSON="$OUTPUT_DIR/release_summary.json"

function bool_is_true() {
    local v
    v="${1:-0}"
    v="${v,,}"
    [[ "$v" == "1" || "$v" == "true" || "$v" == "yes" || "$v" == "on" ]]
}

function require_cmd() {
    local name="$1"
    if ! command -v "$name" >/dev/null 2>&1; then
        echo "Error: required command not found: $name" >&2
        exit 1
    fi
}

function require_env() {
    local key="$1"
    if [[ -z "${!key:-}" ]]; then
        echo "Error: environment variable $key is required" >&2
        exit 1
    fi
}

function run_notary_submit() {
    local artifact_path="$1"
    local output_json="$2"

    xcrun notarytool submit "$artifact_path" \
        --apple-id "$APPLE_ID" \
        --password "$APPLE_APP_SPECIFIC_PASSWORD" \
        --team-id "$TEAM_ID" \
        --wait \
        --output-format json > "$output_json"

    local status
    status="$(python3 - "$output_json" <<'PY'
import json
import sys

with open(sys.argv[1], 'r', encoding='utf-8') as f:
    payload = json.load(f)
print(payload.get('status', 'unknown'))
PY
)"

    if [[ "$status" != "Accepted" ]]; then
        echo "Error: notarization rejected for $artifact_path (status=$status)" >&2
        echo "See JSON log: $output_json" >&2
        return 1
    fi

    echo "Notarization accepted for $artifact_path"
}

require_cmd cmake
require_cmd codesign
require_cmd ditto
require_cmd xcrun
require_cmd otool
require_cmd hdiutil
require_cmd python3

mkdir -p "$OUTPUT_DIR"

if ! bool_is_true "$SKIP_BUILD"; then
    CMAKE_ARGS=(
        -S "$ROOT_DIR"
        -B "$BUILD_DIR"
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
        -DBETTERSPOTLIGHT_ENABLE_SPARKLE=ON
        -DBETTERSPOTLIGHT_FETCH_MODELS=OFF
    )

    if command -v brew >/dev/null 2>&1; then
        QT_PREFIX="$(brew --prefix qt@6 2>/dev/null || true)"
        if [[ -n "$QT_PREFIX" ]]; then
            CMAKE_ARGS+=("-DCMAKE_PREFIX_PATH=$QT_PREFIX")
        fi
    fi

    cmake "${CMAKE_ARGS[@]}"
    cmake --build "$BUILD_DIR" -j"$(sysctl -n hw.ncpu)"
fi

if [[ ! -d "$APP_PATH" || ! -x "$APP_BINARY" ]]; then
    echo "Error: built app bundle not found at $APP_PATH" >&2
    exit 1
fi

if bool_is_true "$REQUIRE_SPARKLE"; then
    if ! otool -L "$APP_BINARY" | grep -q "Sparkle"; then
        echo "Error: Sparkle is not linked in app binary. Expected Sparkle-enabled artifact." >&2
        exit 1
    fi
fi

require_env DEVELOPER_ID

if [[ ! -f "$ENTITLEMENTS_PATH" ]]; then
    echo "Error: entitlements file not found: $ENTITLEMENTS_PATH" >&2
    exit 1
fi

codesign --deep --force --options runtime \
    --entitlements "$ENTITLEMENTS_PATH" \
    --sign "$DEVELOPER_ID" \
    "$APP_PATH"

codesign --verify --deep --strict --verbose=2 "$APP_PATH"

APP_STAGING="$OUTPUT_DIR/betterspotlight.app"
rm -rf "$APP_STAGING" "$ZIP_PATH"
cp -R "$APP_PATH" "$APP_STAGING"

ditto -c -k --sequesterRsrc --keepParent "$APP_STAGING" "$ZIP_PATH"

APP_NOTARIZED=0
DMG_NOTARIZED=0

if bool_is_true "$NOTARIZE"; then
    require_env APPLE_ID
    require_env APPLE_APP_SPECIFIC_PASSWORD
    require_env TEAM_ID

    run_notary_submit "$ZIP_PATH" "$APP_NOTARY_JSON"
    xcrun stapler staple "$APP_STAGING"
    xcrun stapler validate "$APP_STAGING"
    APP_NOTARIZED=1
fi

DMG_PATH=""
if bool_is_true "$CREATE_DMG"; then
    DMG_PATH="$OUTPUT_DIR/BetterSpotlight-${BUILD_TYPE}.dmg"
    STAGING_DIR="$(mktemp -d)"
    trap 'rm -rf "$STAGING_DIR"' EXIT

    cp -R "$APP_STAGING" "$STAGING_DIR/betterspotlight.app"
    ln -s /Applications "$STAGING_DIR/Applications"

    rm -f "$DMG_PATH"
    hdiutil create \
        -volname "BetterSpotlight" \
        -srcfolder "$STAGING_DIR" \
        -ov \
        -format UDZO \
        "$DMG_PATH"

    if bool_is_true "$NOTARIZE"; then
        run_notary_submit "$DMG_PATH" "$DMG_NOTARY_JSON"
        xcrun stapler staple "$DMG_PATH"
        xcrun stapler validate "$DMG_PATH"
        DMG_NOTARIZED=1
    fi

    rm -rf "$STAGING_DIR"
    trap - EXIT
fi

python3 - "$SUMMARY_JSON" "$APP_STAGING" "$ZIP_PATH" "$DMG_PATH" \
    "$APP_NOTARIZED" "$DMG_NOTARIZED" "$BUILD_TYPE" <<'PY'
import json
import os
import sys

summary_path = sys.argv[1]
app_path = sys.argv[2]
zip_path = sys.argv[3]
dmg_path = sys.argv[4]
app_notarized = int(sys.argv[5])
dmg_notarized = int(sys.argv[6])
build_type = sys.argv[7]

summary = {
    "buildType": build_type,
    "appPath": app_path,
    "zipPath": zip_path,
    "dmgPath": dmg_path if dmg_path else "",
    "appNotarized": bool(app_notarized),
    "dmgNotarized": bool(dmg_notarized),
    "appExists": os.path.isdir(app_path),
    "zipExists": os.path.isfile(zip_path),
    "dmgExists": bool(dmg_path and os.path.isfile(dmg_path)),
}

with open(summary_path, "w", encoding="utf-8") as f:
    json.dump(summary, f, indent=2)

print(json.dumps(summary, indent=2))
PY

echo "Notarization pipeline completed. Outputs in: $OUTPUT_DIR"
