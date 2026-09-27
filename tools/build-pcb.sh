#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Regenerate the board end to end: outline and nets, placement, routing, pours.
# Reproducible -- the .kicad_pcb is an output, not a hand-edited source. If you
# edit it in the GUI, either stop running this or fold the change back into the
# scripts.
#
# Freerouting is NOT deterministic. The same inputs give a different board every
# run, and the difference matters: consecutive runs have landed on anything from
# zero defects to two floating ground pads and two unconnected items. So the
# build routes several times and KEEPS THE BEST ATTEMPT rather than whatever the
# last dice roll produced. A board whose quality depends on luck is not a build.
#
#   tools/build-pcb.sh [routing passes, default 3] [attempts, default 3]
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$here/.."
passes="${1:-3}"
attempts="${2:-3}"
PCB="$root/hardware/kicad/ups-adaptor.kicad_pcb"
BEST="$(mktemp -t ups-best).kicad_pcb"
trap 'rm -f "$BEST"' EXIT

KPY="${KICAD_PY:-/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3}"
KCLI="${KICAD_CLI:-/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli}"
quiet() { grep -viE 'assert .*traits|wxApp|Adding duplicate image handler|Debug:' || true; }

best_score=999999
for attempt in $(seq 1 "$attempts"); do
echo "########## attempt $attempt of $attempts"
echo "== 1/9 outline, footprints, nets, zones"
python3 "$here/gen-pcb.py"

echo "== 2/9 placement (pcbnew: correct flipping and rotation)"
"$KPY" "$here/place-pcb.py" 2>&1 | quiet

echo "== 3/9 reserve ground escape vias (before the router takes the space)"
"$KPY" "$here/preseed-gnd.py" 2>&1 | quiet

echo "== 4/9 routing, $passes passes"
for i in $(seq 1 "$passes"); do
    "$KPY" "$here/route-pcb.py" 2>&1 | quiet | grep -E 'unrouted|track/via' || true
done

echo "== 5/9 join duplicate pads"
"$KPY" "$here/join-pads.py" 2>&1 | quiet

"$KPY" "$here/widen-power.py" 2>&1 | quiet

echo "== 6/9 ground stitching vias"
"$KPY" "$here/stitch-pcb.py" 2>&1 | quiet

echo "== 7/9 ground pours"
"$KPY" "$here/pour-pcb.py" 2>&1 | quiet

# Filling the pour re-cuts it, so a pad that was in the plane when stitching ran
# can be orphaned by the fill that follows. That is why the floating-pad list
# used to differ on every run. Rescue and re-fill until it stops changing.
for i in 1 2 3; do
    if "$KPY" "$here/test/test_ground.py" 2>&1 | quiet | grep -q "every GND pad reaches"; then
        break
    fi
    echo "   floating pads after the fill -- rescue pass $i"
    "$KPY" "$here/stitch-pcb.py" 2>&1 | quiet | grep -E 'rescued|tied' || true
    "$KPY" "$here/pour-pcb.py" 2>&1 | quiet | grep -v 'filled pour' || true
done
"$KPY" "$here/test/test_ground.py" 2>&1 | quiet | grep -E 'ground network|ok |FAIL|WARN' || true

# Score this attempt. A floating ground is far worse than an unrouted cosmetic
# pin, so weight it accordingly.
# `|| true` is load-bearing: test_ground.py exits non-zero when it finds a
# floating pad, and under `set -e` with pipefail a command substitution that
# inherits that status aborts the whole build -- which killed the retry loop in
# exactly the case it exists to handle. A failing attempt must be SCORED, not
# fatal.
floating=$( { "$KPY" "$here/test/test_ground.py" 2>&1 || true; } | quiet \
    | sed -n 's/.*FAIL \([0-9]*\) GND pads.*/\1/p' )
floating="${floating:-0}"
"$KCLI" pcb drc --severity-error -o /tmp/ups-drc-attempt.rpt "$PCB" \
    2>&1 | grep -v Fontconfig > /dev/null || true
unconn=$(grep -c '^\[unconnected_items\]' /tmp/ups-drc-attempt.rpt || true)
# items_not_allowed is in this list because it was NOT, and an attempt scoring
# 11 turned out to be carrying three of them -- copper inside a keepout, which
# on this board means the antenna exclusion zone. A defect class the scorer
# cannot see is a defect class the best-of-N loop will happily select for.
other=$(grep -cE '^\[(clearance|shorting_items|tracks_crossing|hole_clearance|items_not_allowed|silk_over_copper_error)\]' /tmp/ups-drc-attempt.rpt || true)
score=$(( floating * 100 + unconn * 10 + other ))
echo "   attempt $attempt: $floating floating ground pad(s), $unconn unconnected, score $score"

if [ "$score" -lt "$best_score" ]; then
    best_score=$score
    cp "$PCB" "$BEST"
    echo "   kept as best so far"
fi
if [ "$best_score" -eq 0 ]; then
    echo "   clean board, no need to re-roll"
    break
fi
done

cp "$BEST" "$PCB"
echo "########## using the best of $attempts attempt(s), score $best_score"

echo "== 8/9 silkscreen"
"$KPY" "$here/silk-pcb.py" 2>&1 | quiet
"$KPY" "$here/silk-jlc.py" 2>&1 | quiet

echo "== 9/9 fab outputs"
mkdir -p "$root/hardware/fab"
"$KCLI" pcb export gerbers -o "$root/hardware/fab/" "$root/hardware/kicad/ups-adaptor.kicad_pcb" 2>&1 | grep -v Fontconfig | tail -1
"$KCLI" pcb export drill -o "$root/hardware/fab/" --excellon-separate-th "$root/hardware/kicad/ups-adaptor.kicad_pcb" 2>&1 | grep -v Fontconfig | tail -1
"$KCLI" pcb export pos -o "$root/hardware/fab/ups-adaptor-cpl.csv" --format csv --units mm --side both "$root/hardware/kicad/ups-adaptor.kicad_pcb" 2>&1 | grep -v Fontconfig | tail -1

# The files that actually get uploaded to JLC. Derived from the schematic's own
# table every build, because a hand-exported BOM sat stale here for days and
# still named the AP2161W when it was finally read.
python3 "$here/gen-bom.py"

echo "== DRC"
"$KCLI" pcb drc --severity-error -o /tmp/ups-drc.rpt \
    "$root/hardware/kicad/ups-adaptor.kicad_pcb" 2>&1 | grep -v Fontconfig | tail -1
python3 - <<'PY'
import re
t = open('/tmp/ups-drc.rpt').read()
d = {}
for b in re.split(r'\n(?=\[)', t):
    m = re.match(r'\[([a-z_]+)\]', b)
    if m:
        d[m.group(1)] = d.get(m.group(1), 0) + 1
for k, v in sorted(d.items(), key=lambda x: -x[1]):
    print("   %-24s %d" % (k, v))
PY
