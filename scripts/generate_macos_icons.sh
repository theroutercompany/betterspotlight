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

generate_master_png() {
  magick -size 1024x1024 xc:none \
    -fill "#1976D2" -stroke "#0C4E93" -strokewidth 8 \
    -draw "roundrectangle 64,64 960,960 214,214" \
    -fill "rgba(255,255,255,0.16)" -stroke none \
    -draw "roundrectangle 128,128 896,320 120,120" \
    -fill none -stroke white -strokewidth 86 \
    -draw "ellipse 456,456 190,190 0,360" \
    -draw "line 582,582 748,748" \
    -fill white -stroke none \
    -draw "polygon 714,222 734,274 786,294 734,314 714,366 694,314 642,294 694,274" \
    -fill "rgba(255,255,255,0.2)" \
    -draw "ellipse 258,764 44,44 0,360" \
    -colorspace sRGB -alpha on -type TrueColorAlpha \
    "${MASTER_PNG}"
}

render_png() {
  local size="$1"
  local out="$2"
  magick "${MASTER_PNG}" -resize "${size}x${size}" -colorspace sRGB -alpha on -type TrueColorAlpha "${out}"
}

generate_master_png

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

draw_menubar_icon() {
  local color="$1"
  local sparkle="$2"
  local out="$3"

  magick -size 64x64 xc:none \
    -fill "${color}" -stroke "rgba(0,0,0,0.25)" -strokewidth 2 \
    -draw "roundrectangle 8,8 56,56 13,13" \
    -fill none -stroke white -strokewidth 4 \
    -draw "circle 30,30 30,19" \
    -draw "line 39,39 48,48" \
    -fill white -stroke none \
    -draw "${sparkle}" \
    -colorspace sRGB -alpha on -type TrueColorAlpha \
    "${out}"
}

draw_menubar_error_icon() {
  local out="$1"
  magick -size 64x64 xc:none \
    -fill "#C62828" -stroke "rgba(0,0,0,0.25)" -strokewidth 2 \
    -draw "roundrectangle 8,8 56,56 13,13" \
    -fill none -stroke white -strokewidth 4 \
    -draw "circle 30,30 30,19" \
    -draw "line 39,39 48,48" \
    -fill white -stroke none \
    -draw "roundrectangle 29,16 35,34 2,2" \
    -draw "ellipse 32,43 3,3 0,360" \
    -colorspace sRGB -alpha on -type TrueColorAlpha \
    "${out}"
}

draw_menubar_icon "#1976D2" "polygon 45,15 47,21 53,23 47,25 45,31 43,25 37,23 43,21" "${ASSET_DIR}/menubar_idle.png"
draw_menubar_icon "#FB8C00" "polygon 45,14 47,20 53,22 47,24 45,30 43,24 37,22 43,20" "${ASSET_DIR}/menubar_indexing_a.png"
draw_menubar_icon "#F57C00" "polygon 47,16 49,22 55,24 49,26 47,32 45,26 39,24 45,22" "${ASSET_DIR}/menubar_indexing_b.png"
draw_menubar_error_icon "${ASSET_DIR}/menubar_error.png"

iconutil -c icns "${ICONSET_DIR}" -o "${APP_ICNS}"

echo "Generated:"
echo "  ${APP_ICNS}"
echo "  ${ASSET_DIR}/menubar_idle.png"
echo "  ${ASSET_DIR}/menubar_indexing_a.png"
echo "  ${ASSET_DIR}/menubar_indexing_b.png"
echo "  ${ASSET_DIR}/menubar_error.png"
