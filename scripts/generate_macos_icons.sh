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
  local marker="$1"
  local marker_y="$2"
  local out="$3"

  magick -size 64x64 xc:none \
    -fill none -stroke "rgba(0,0,0,0.97)" -strokewidth 9 \
    -draw "circle 24,24 24,8" \
    -draw "line 35,35 56,56" \
    -fill "rgba(0,0,0,0.97)" -stroke none \
    -draw "${marker}" \
    -draw "ellipse 53,${marker_y} 4.6,4.6 0,360" \
    -colorspace sRGB -alpha on -type TrueColorAlpha \
    "${out}"
}

draw_menubar_error_icon() {
  local out="$1"
  magick -size 64x64 xc:none \
    -fill none -stroke "rgba(0,0,0,0.97)" -strokewidth 9 \
    -draw "circle 24,24 24,8" \
    -draw "line 35,35 56,56" \
    -fill "rgba(0,0,0,0.97)" -stroke none \
    -draw "roundrectangle 50,7 57,31 2,2" \
    -draw "ellipse 53,40 4,4 0,360" \
    -colorspace sRGB -alpha on -type TrueColorAlpha \
    "${out}"
}

draw_menubar_icon "polygon 53,7 55,14 61,17 55,20 53,27 51,20 45,17 51,14" "13" "${ASSET_DIR}/menubar_idle.png"
draw_menubar_icon "polygon 53,7 55,14 61,17 55,20 53,27 51,20 45,17 51,14" "19" "${ASSET_DIR}/menubar_indexing_a.png"
draw_menubar_icon "polygon 53,7 55,14 61,17 55,20 53,27 51,20 45,17 51,14" "25" "${ASSET_DIR}/menubar_indexing_b.png"
draw_menubar_error_icon "${ASSET_DIR}/menubar_error.png"

iconutil -c icns "${ICONSET_DIR}" -o "${APP_ICNS}"

echo "Generated:"
echo "  ${APP_ICNS}"
echo "  ${ASSET_DIR}/menubar_idle.png"
echo "  ${ASSET_DIR}/menubar_indexing_a.png"
echo "  ${ASSET_DIR}/menubar_indexing_b.png"
echo "  ${ASSET_DIR}/menubar_error.png"
