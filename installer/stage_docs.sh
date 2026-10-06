#!/usr/bin/env bash
# Writes the documents every installer ships into <dest>: Readme.txt (the README's quick start, as plain text) and
# licenses/ (the third-party licences listed in THIRD_PARTY_NOTICES.md, the notices themselves, and the project's own
# LICENSE once there is one). Called by the macOS, Windows and Linux packaging steps.
#
# Usage: installer/stage_docs.sh <dest> [version]

set -euo pipefail

DEST="${1:?Usage: stage_docs.sh <dest> [version]}"
VERSION="${2:-}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

mkdir -p "$DEST/licenses"

# The quick start, from its heading up to the next one, with Markdown emphasis and links flattened.
{
    echo "Phase Align${VERSION:+ $VERSION}"
    echo
    awk '/^## Quick start/{on=1; next} on && /^## /{exit} on' "$ROOT/README.md" |
        perl -0pe 's/\*\*(.+?)\*\*/$1/gs; s/\*(.+?)\*/$1/gs; s/\[([^]]+)\]\([^)]+\)/$1/g'
    echo
    echo "Phase Align is free software under the GNU AGPLv3 (licenses/PhaseAlign-LICENSE.txt). Its source code:"
    echo "https://github.com/tehguitarist/PhaseAlign"
    echo
    echo "Third-party software and fonts: see the licenses folder next to this file."
} > "$DEST/Readme.txt"

cp "$ROOT/THIRD_PARTY_NOTICES.md" "$DEST/licenses/THIRD_PARTY_NOTICES.txt"
cp "$ROOT/libs/pffft/LICENSE.txt" "$DEST/licenses/PFFFT-LICENSE.txt"
cp "$ROOT/assets/fonts/licenses/"*.txt "$DEST/licenses/"
[ -f "$ROOT/LICENSE" ] && cp "$ROOT/LICENSE" "$DEST/licenses/PhaseAlign-LICENSE.txt"
echo "Staged docs in $DEST"
