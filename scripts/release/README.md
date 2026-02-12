# macOS Release Pipeline

## Primary script

`scripts/release/build_macos_release.sh`

This script builds/stages a production-style macOS artifact with:

- App bundle staging at `dist/release/BetterSpotlight.app`
- Helper services in `Contents/Helpers/`
- Bootstrap model assets only in `Contents/Resources/models/`
- Optional `macdeployqt` framework deployment (main app + helper executables)
- Post-deploy Mach-O sanitization (`install_name_tool`) to remove non-portable refs
- Hard portability gate (fails build if non-system absolute deps/rpaths remain)
- Optional code signing
- Default ad-hoc signing for unsigned local builds (prevents invalid-page crashes
  after Mach-O install-name rewrites)
- Optional notarization + stapling
- Optional DMG creation

## Common usage

### Local unsigned release smoke

```bash
BS_SIGN=0 BS_NOTARIZE=0 BS_CREATE_DMG=1 \
scripts/release/build_macos_release.sh
```

### Signed (non-notarized) validation

```bash
DEVELOPER_ID="Developer ID Application: Your Org (TEAMID)" \
BS_SIGN=1 BS_NOTARIZE=0 BS_CREATE_DMG=1 \
scripts/release/build_macos_release.sh
```

### Full notarized release

```bash
DEVELOPER_ID="Developer ID Application: Your Org (TEAMID)" \
APPLE_ID="you@example.com" \
APPLE_APP_SPECIFIC_PASSWORD="xxxx-xxxx-xxxx-xxxx" \
TEAM_ID="TEAMID" \
BS_SIGN=1 BS_NOTARIZE=1 BS_CREATE_DMG=1 \
scripts/release/build_macos_release.sh
```

## Important env vars

- `BS_RELEASE_BUILD_DIR` (default: `build-release`)
- `BS_RELEASE_OUTPUT_DIR` (default: `dist/release`)
- `BS_RELEASE_BUILD_TYPE` (default: `Release`)
- `BS_ENABLE_SPARKLE` (`0|1`, default `0`)
- `BS_RUN_MACDEPLOYQT` (`0|1`, default `1`)
- `BS_MACDEPLOYQT_TIMEOUT_SEC` (default: `180`)
- `BS_SKIP_BUILD` (`0|1`, default `0`)
- `BS_SIGN` (`0|1`, default `0`)
- `BS_ADHOC_SIGN_WHEN_UNSIGNED` (`0|1`, default `1`)
- `BS_NOTARIZE` (`0|1`, default `0`)
- `BS_CREATE_DMG` (`0|1`, default `1`)
- `BS_ENTITLEMENTS_PATH` (default: `packaging/macos/entitlements.plist`)

## Backward-compatible CI entrypoint

`scripts/release/notarize_with_sparkle.sh` now wraps the primary script and keeps existing workflow behavior.
