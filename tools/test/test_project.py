#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The design rules DRC runs against must be the ones this project chose.

KiCad keeps design rules and net classes in the .kicad_pro, NOT in the board.
So "DRC passes" means nothing on its own: it means the board satisfies whatever
rules that file happens to contain, and that file is easy to lose.

It was lost. `pcbnew.SaveBoard()` in a standalone script rewrites the project
file from the board's in-memory settings, which are KiCad's defaults -- so a
script that only meant to edit component values silently reverted every design
rule and deleted the Power net class. It was caught only because KiCad's
default edge clearance (0.5mm) is STRICTER than this project's (0.3mm) and DRC
started failing. Had the default been looser, DRC would have gone on passing
against rules nobody chose, on a board with no power net class at all.

    $KICAD_PY tools/test/test_project.py     (plain python3 is fine too)
"""
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PRO = os.path.join(ROOT, "hardware", "kicad", "ups-adaptor.kicad_pro")

# Must match what tools/gen-pcb.py writes.
RULES = {
    "min_clearance": 0.2, "min_track_width": 0.2,
    "min_through_hole_diameter": 0.3, "min_via_annular_width": 0.13,
    "min_via_diameter": 0.6, "min_hole_clearance": 0.25,
    "min_hole_to_hole": 0.5, "min_copper_edge_clearance": 0.3,
}
POWER_NETS = {"+5V", "5V_RAW", "+3V3", "VBUS_A"}
POWER_WIDTH = 0.4


def main():
    if not os.path.exists(PRO):
        print("  FAIL %s is missing" % PRO)
        return 1
    pro = json.load(open(PRO))
    rules = pro.get("board", {}).get("design_settings", {}).get("rules", {})
    nets = pro.get("net_settings", {})

    fails = 0
    wrong = {k: (rules.get(k), v) for k, v in RULES.items() if rules.get(k) != v}
    if wrong:
        print("  FAIL the project's design rules are not the ones gen-pcb.py sets:")
        for k, (got, want) in sorted(wrong.items()):
            print("         %-28s is %s, should be %s" % (k, got, want))
        print("       DRC has been passing against rules nobody chose. Re-run")
        print("       tools/gen-pcb.py, or restore the .kicad_pro.")
        fails += 1
    else:
        print("  ok   all %d design rules match gen-pcb.py" % len(RULES))

    classes = {c.get("name"): c for c in nets.get("classes", [])}
    power = classes.get("Power")
    if power is None:
        print("  FAIL there is no Power net class -- the rails will route at "
              "signal width")
        fails += 1
    elif abs(power.get("track_width", 0) - POWER_WIDTH) > 1e-9:
        print("  FAIL the Power net class is %.2f mm wide, should be %.2f"
              % (power.get("track_width", 0), POWER_WIDTH))
        fails += 1
    else:
        assigned = {p.get("pattern") for p in nets.get("netclass_patterns", [])
                    if p.get("netclass") == "Power"}
        missing = POWER_NETS - assigned
        if missing:
            print("  FAIL these rails are not assigned to the Power class: %s"
                  % ", ".join(sorted(missing)))
            fails += 1
        else:
            print("  ok   Power net class is %.1f mm and carries %s"
                  % (POWER_WIDTH, ", ".join(sorted(POWER_NETS))))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
