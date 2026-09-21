#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Regenerate every board rendering in docs/img.
#
# These used to be made by hand from a command in an HTML comment, and it cost
# what you would expect: the back silkscreen was rewritten in e992524 and
# board-bottom.png was left showing the previous artwork, because regenerating
# it was a thing someone had to remember. A rendering that disagrees with the
# board is worse than no rendering -- it is the picture a reviewer trusts.
#
#   tools/render-board.sh
#
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$here/.."
cd "$root"

KCLI="${KICAD_CLI:-/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli}"
PCB=hardware/kicad/ups-adaptor.kicad_pcb
OUT=docs/img

[ -x "$KCLI" ] || { echo "kicad-cli not found at $KCLI (set KICAD_CLI)"; exit 1; }
mkdir -p "$OUT"

render() {   # render <name> <width> <height> <side> [extra...]
    local name=$1 w=$2 h=$3 side=$4; shift 4
    echo "== $name.png  ${w}x${h}  $side"
    "$KCLI" pcb render -o "$OUT/$name.png" --side "$side" \
        -w "$w" -h "$h" --quality high --background transparent "$@" "$PCB"
}

# The hero shot used on the one-pager and the README: perspective, tilted so
# the connectors and the module are all legible in one frame.
render board-hero   2200 1600 top    --rotate "-25,0,32" --perspective

# Isometric, orthogonal -- for reading placement rather than for looking good.
render board-iso    1800 1300 top    --rotate "-30,0,35"

# Straight-down top and bottom, portrait to match the 36 x 58 mm outline.
# The bottom carries no parts (Economic PCBA is single-sided placement) so this
# view exists to show the silkscreen and the copper, which is exactly what got
# out of date last time.
render board-top    1400 2000 top
render board-bottom 1400 2000 bottom

echo
echo "rendered from $(git log -1 --format=%h -- "$PCB") ($PCB)"
