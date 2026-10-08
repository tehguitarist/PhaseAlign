#!/bin/bash
# Builds the embedded UI assets from the masters in ui/ and regenerates
# src/ui/Layout.h from ui/ui-info.csv. Re-run after changing any artwork or the CSV, then rebuild.
#
# Each image is resampled to 2.5x of its slot at the default 782x490 size, which is exactly its slot in the
# 1954x1224 design space, keeping its aspect ratio, then compressed with pngquant. Masters are never enlarged.
# Output goes to assets/images/, which CMake embeds as BinaryData. Fonts live in assets/fonts/ and
# are not processed. Requires magick (ImageMagick), pngquant and the project venv.
set -euo pipefail
cd "$(dirname "$0")/.."

PY=.venv/bin/python
[ -x "$PY" ] || { echo "missing .venv: python3 -m venv .venv && .venv/bin/pip install -r reference/requirements.txt" >&2; exit 1; }
command -v magick >/dev/null || { echo "magick not found" >&2; exit 1; }
command -v pngquant >/dev/null || { echo "pngquant not found" >&2; exit 1; }

"$PY" tools/ui_layout.py header

OUT=assets/images
rm -rf "$OUT"
mkdir -p "$OUT"

"$PY" tools/ui_layout.py assets | while IFS=$'\t' read -r name w h; do
    src="ui/$name.png"
    dst="$OUT/$name.png"
    # Fit inside the slot box (aspect kept). ">" only ever shrinks: a master at or below its slot size is kept.
    # Resampling in linear light keeps highlights and LED glows from darkening.
    magick "$src" -colorspace RGB -filter Lanczos -resize "${w}x${h}>" -colorspace sRGB -strip "$dst"
    # High quality floor: the knob and panel gradients band visibly below ~85.
    pngquant --force --skip-if-larger --quality=85-100 --speed 1 --output "$dst" "$dst" || true
    printf '%-24s %s\n' "$name" "$(magick identify -format '%wx%h' "$dst")"
done

du -sh "$OUT"
