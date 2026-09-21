#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build the three things JLCPCB's order form actually takes:
#
#   hardware/fab/ups-adaptor-gerbers.zip   PCB tab: gerbers + drill
#   hardware/bom/bom.csv                   Assembly tab: BOM
#   hardware/bom/cpl.csv                   Assembly tab: placement
#
# Only manufacturing layers go in the zip. `kicad-cli pcb export gerbers` with
# no --layers emits Courtyard, Fab, Adhesive, Margin and four User_* layers as
# well -- documentation, which a fab's importer either ignores or, worse, offers
# to treat as a real layer. Being explicit is the point of this script.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$here/.."
PCB="$root/hardware/kicad/ups-adaptor.kicad_pcb"
OUT="$root/hardware/fab"
ZIP="$OUT/ups-adaptor-gerbers.zip"

KCLI="${KICAD_CLI:-/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli}"
quiet() { grep -v Fontconfig || true; }

# F_Paste/B_Paste are for the stencil. Harmless on a bare-board order and
# required if the same zip is used for assembly, so they stay in.
LAYERS="F.Cu,B.Cu,F.Mask,B.Mask,F.Silkscreen,B.Silkscreen,F.Paste,B.Paste,Edge.Cuts"

rm -rf "$OUT/gerber" "$ZIP"
mkdir -p "$OUT/gerber"

echo "== gerbers ($LAYERS)"
"$KCLI" pcb export gerbers --layers "$LAYERS" --no-protel-ext \
    -o "$OUT/gerber/" "$PCB" 2>&1 | quiet | tail -1

echo "== drill"
"$KCLI" pcb export drill --format excellon --drill-origin absolute \
    --excellon-units mm --excellon-separate-th \
    -o "$OUT/gerber/" "$PCB" 2>&1 | quiet | tail -1

# gen-bom.py reads KiCad's raw position export and rewrites it into JLC's
# column names WITH the reel-angle corrections applied. Export it here rather
# than depending on build-pcb.sh having run: a missing one used to make this
# script skip the CPL entirely and still print "upload these three files".
# NOT `kicad-cli pcb export pos`: that writes each footprint's ANCHOR, and a
# machine places the part's CENTROID at the coordinate it is given. On this
# board the two differ by 3.77 mm on U1, 2.49 on J2 and 1.46 on J1 -- which is
# what JLC's preview showed as the module and both USB connectors sitting off
# their pads. gen-cpl-pos.py writes the same columns with centroids.
echo "== placement"
"${KICAD_PY:-/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3}" \
    "$here/gen-cpl-pos.py" 2>&1 | grep -viE 'wxApp|memory leak|Debug:|assert ' | quiet

echo "== order files"
python3 "$here/gen-bom.py"

( cd "$OUT/gerber" && zip -q -X "../$(basename "$ZIP")" ./* )

# The zip and the loose gerbers are COMMITTED, so they can be uploaded from any
# machine without KiCad. That reintroduces the stale-artifact risk this project
# has been bitten by before: a zip that every gate passes because no gate looks
# at it. Record what it was built from. tools/test/test_fab.py recomputes this
# and fails if the board has moved on -- git does not preserve mtimes, so a
# timestamp comparison would be meaningless after a clone.
shasum -a 256 "$PCB" | awk '{print $1}' > "$OUT/SOURCE.sha256"
echo
echo "wrote $(realpath --relative-to="$root" "$ZIP" 2>/dev/null || echo "${ZIP#$root/}")"
unzip -l "$ZIP" | awk 'NR>3 && NF>3 && $4!="" {printf "   %8d  %s\n",$1,$4}' | head -20
# One archive with everything an order needs, for moving between machines or
# handing to someone else. The gerber zip stays a zip inside it, because that
# is what JLC's PCB tab takes.
ORDER="$OUT/ups-adaptor-jlcpcb-order.zip"
rm -f "$ORDER"
STAGE="$OUT/.order"
rm -rf "$STAGE"; mkdir -p "$STAGE"
cp "$ZIP" "$STAGE/"
cp "$root/hardware/bom/bom.csv" "$root/hardware/bom/cpl.csv" "$STAGE/"
cat > "$STAGE/README.txt" <<'ORDERNOTE'
ups-adaptor Rev A -- JLCPCB order package
=========================================

Upload these three, in this order:

  PCB tab         ups-adaptor-gerbers.zip     (upload the ZIP as-is)
  Assembly, BOM   bom.csv
  Assembly, CPL   cpl.csv

Board settings: JLCPCB's defaults are correct for this board -- 2 layer,
1.6 mm FR-4, 1 oz copper, HASL, green with white silkscreen, tented vias.
Tented matters: U1's thermal vias have no back mask opening and the back
silkscreen artwork prints over them.

Change from the defaults:
  * Quantity 10, not 5.
  * Turn on PCB Assembly, ASSEMBLY SIDE: TOP ONLY. This board cannot be
    double-sided -- Economic PCBA is single-sided placement, and Standard
    PCBA's minimum board is 70x70 mm against this one's 36x58.
  * J3 and J5 are through-hole and are NOT in the CPL. Either fit them
    yourself or re-run tools/pack-fab.sh --with-tht.
  * JLC prints their order number somewhere of their choosing. The back of
    this board is artwork; either pay to remove the number or accept it.

BEFORE PAYING, open Component Placements and check these against the board.
The CPL carries seven rotations that are NOT KiCad's own angle, each one
decided by looking at this preview, and they are the failure nobody can see
after assembly:

  U2 SOIC-16   0     body runs EAST-WEST, 8 pads north and 8 south
  U3 SOT-223   180   3 leads WEST, tab EAST
  U4 SOT-23-5  270   3 leads WEST, 2 EAST
  D1 SOT-23-6  270   3 leads WEST, 3 EAST
  D2 SOT-23-6  270   3 leads WEST, 3 EAST
  Q1 SOT-23    180   2 leads WEST, 1 EAST
  Q2 SOT-23    270   2 leads SOUTH, 1 NORTH

U4, D1 and D2 were inferred from an earlier preview rather than confirmed,
so they are the ones worth the extra ten seconds.

J1 (USB-C) and U1 take KiCad's own angle. Both were tested: the community
rotation table has a rule for J1's exact part number and it drew the
connector UPSIDE DOWN, which is why this project no longer applies that
table at all.

Positions are pad CENTROIDS, not KiCad anchors. U1, J1 and J2 differ by
3.77, 1.46 and 2.49 mm between the two, and at 3.77 mm every one of the
module's 40 castellated pads misses.

Polarity on silk: a dot marks pin 1. D3 and D4 instead carry a BAR across
the CATHODE end -- on a diode a dot would read as the capacitor convention
and mean the opposite.
ORDERNOTE
( cd "$STAGE" && zip -q -X "$ORDER" ./* )
rm -rf "$STAGE"
echo
echo "wrote ${ORDER#$root/}"
unzip -l "$ORDER" | awk 'NR>3 && NF>3 && $4!="" {printf "   %8d  %s\n",$1,$4}'

echo
echo "Upload to JLCPCB:"
echo "   PCB          hardware/fab/ups-adaptor-gerbers.zip"
echo "   BOM          hardware/bom/bom.csv"
echo "   Placement    hardware/bom/cpl.csv"
echo
cat <<'NOTE'

Seven rotations in the CPL are NOT KiCad's own angle, each decided by
looking at JLC's placement preview: U2 0, U3 180, U4 270, D1 270, D2 270,
Q1 180, Q2 270. Check them there before paying -- U4, D1 and D2 were
inferred from an earlier preview rather than confirmed.

J1 and U1 take KiCad's angle deliberately. The community rotation table
has a rule for J1's exact part number and it drew the connector upside
down, which is why none of that table is applied any more.

Positions are pad centroids, not anchors: U1, J1 and J2 differ by 3.77,
1.46 and 2.49 mm between the two.
NOTE
