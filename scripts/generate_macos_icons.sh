#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ASSET_DIR="${ROOT_DIR}/src/app/assets"
ICONSET_DIR="${ASSET_DIR}/AppIcon.iconset"
MASTER_PNG="${ASSET_DIR}/app_icon_master.png"
APP_ICNS="${ASSET_DIR}/BetterSpotlight.icns"

if ! command -v magick >/dev/null 2>&1; then
  echo "error: 'magick' (ImageMagick) is required" >&2
  exit 1
fi

if ! command -v iconutil >/dev/null 2>&1; then
  echo "error: 'iconutil' is required (macOS only)" >&2
  exit 1
fi

mkdir -p "${ICONSET_DIR}"

render_png() {
  local size="$1"
  local out="$2"
  magick "${MASTER_PNG}" -resize "${size}x${size}" -colorspace sRGB -alpha on -type TrueColorAlpha "${out}"
}

if [[ ! -f "${MASTER_PNG}" ]]; then
  echo "error: missing ${MASTER_PNG}" >&2
  exit 1
fi

render_png 16   "${ICONSET_DIR}/icon_16x16.png"
render_png 32   "${ICONSET_DIR}/icon_16x16@2x.png"
render_png 32   "${ICONSET_DIR}/icon_32x32.png"
render_png 64   "${ICONSET_DIR}/icon_32x32@2x.png"
render_png 128  "${ICONSET_DIR}/icon_128x128.png"
render_png 256  "${ICONSET_DIR}/icon_128x128@2x.png"
render_png 256  "${ICONSET_DIR}/icon_256x256.png"
render_png 512  "${ICONSET_DIR}/icon_256x256@2x.png"
render_png 512  "${ICONSET_DIR}/icon_512x512.png"
render_png 1024 "${ICONSET_DIR}/icon_512x512@2x.png"

iconutil -c icns "${ICONSET_DIR}" -o "${APP_ICNS}"

echo "Generated:"
echo "  ${APP_ICNS}"
