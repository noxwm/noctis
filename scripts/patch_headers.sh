#!/usr/bin/env bash
# Patches wlroots system headers that use C99 [static N] array qualifiers,
# which are not valid C++ syntax. Outputs patched copies into $1.
#
# Usage: patch_headers.sh <output_dir>
# Called automatically by CMake during configure.

set -e

OUT="$1"
if [[ -z "$OUT" ]]; then
  echo "Usage: $0 <output_dir>" >&2
  exit 1
fi

HEADERS=(
  "types/wlr_scene.h"
  "types/wlr_matrix.h"
  "render/wlr_renderer.h"
  "render/interface.h"
)

for HDR in "${HEADERS[@]}"; do
  SRC="/usr/include/wlr/$HDR"
  DST="$OUT/wlr/$HDR"
  mkdir -p "$(dirname "$DST")"
  sed 's/\[static [0-9]*\]/[]/g' "$SRC" > "$DST"
  echo "patched: $HDR"
done
