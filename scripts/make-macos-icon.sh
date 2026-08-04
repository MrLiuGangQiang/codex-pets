#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 <1024x1024-png> <output.icns>" >&2
  exit 2
fi

source_png="$1"
output_icns="$2"
[[ -f "$source_png" ]] || { echo "icon source not found: $source_png" >&2; exit 1; }
mkdir -p "$(dirname "$output_icns")"
work="$(mktemp -d "${TMPDIR:-/tmp}/codexpets-icon.XXXXXX")"
trap 'rm -rf "$work"' EXIT
iconset="$work/AppIcon.iconset"
mkdir -p "$iconset"

make_icon() {
  local pixels="$1"
  local name="$2"
  /usr/bin/sips -s format png -z "$pixels" "$pixels" "$source_png" \
    --out "$iconset/$name" >/dev/null
}

make_icon 16 icon_16x16.png
make_icon 32 icon_16x16@2x.png
make_icon 32 icon_32x32.png
make_icon 64 icon_32x32@2x.png
make_icon 128 icon_128x128.png
make_icon 256 icon_128x128@2x.png
make_icon 256 icon_256x256.png
make_icon 512 icon_256x256@2x.png
make_icon 512 icon_512x512.png
make_icon 1024 icon_512x512@2x.png
/usr/bin/iconutil -c icns "$iconset" -o "$output_icns"
