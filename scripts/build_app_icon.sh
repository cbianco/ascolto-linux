#!/bin/sh
# Rebuild the native macOS icon from its approved 1024px transparent master.
set -eu
project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
icon_work=$(mktemp -d "${TMPDIR:-/tmp}/ascolto-icon.XXXXXX")
trap 'rm -rf "$icon_work"' EXIT HUP INT TERM
icon_set="$icon_work/Ascolto.iconset"
mkdir -p "$icon_set"
for icon_size in 16 32 128 256 512; do
    /usr/bin/sips -z "$icon_size" "$icon_size" "$project_root/assets/ascolto-app-icon.png" --out "$icon_set/icon_${icon_size}x${icon_size}.png" >/dev/null
    retina_size=$((icon_size * 2))
    /usr/bin/sips -z "$retina_size" "$retina_size" "$project_root/assets/ascolto-app-icon.png" --out "$icon_set/icon_${icon_size}x${icon_size}@2x.png" >/dev/null
done
/usr/bin/iconutil -c icns "$icon_set" -o "$project_root/assets/Ascolto.icns"
