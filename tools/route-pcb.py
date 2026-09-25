#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Autoroute the board via Freerouting, round-tripping through Specctra.

    ups-adaptor.kicad_pcb
        |  pcbnew.ExportSpecctraDSN()     existing tracks come across as fixed
        v
    ups-adaptor.dsn
        |  java -jar freerouting.jar -de ... -do ...
        v
    ups-adaptor.ses
        |  pcbnew.ImportSpecctraSES()     merges tracks back, keeps footprints
        v
    ups-adaptor.kicad_pcb

Re-runnable: each pass re-exports from the current board, so manual touch-ups
survive as fixed routes.

An autorouter is a starting point, not an authority. It has no idea that the USB
pairs want to stay tight and matched over unbroken ground, or that the antenna
end wants nothing near it. Check those by hand afterwards; the harness checks
what it can.

Pattern borrowed from the christmas-tree-sensor repo's rev7 routing script.

    FREEROUTING_JAR=/path/to/freerouting.jar $KICAD_PY tools/route-pcb.py
"""
import os
import subprocess
import sys

import pcbnew

HW = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                  "hardware", "kicad")
PCB = os.path.join(HW, "ups-adaptor.kicad_pcb")
DSN = os.path.join(HW, "ups-adaptor.dsn")
SES = os.path.join(HW, "ups-adaptor.ses")

def _find_jar():
    """freerouting.jar is not in this repository -- it is a 30MB binary.

    It was borrowed from the christmas-tree-sensor repo on the machine this was
    written on. Anywhere else that path does not exist, so look in a few sane
    places and then say plainly what to do rather than failing on one person's
    home directory."""
    env = os.environ.get("FREEROUTING_JAR")
    if env:
        return env
    here = os.path.dirname(os.path.abspath(__file__))
    for c in (os.path.join(here, "freerouting.jar"),
              os.path.join(here, "..", "hardware", "tools", "freerouting.jar"),
              os.path.expanduser("~/Downloads/GitHub/christmas-tree-sensor/"
                                 "hardware/tools/freerouting.jar"),
              os.path.expanduser("~/freerouting.jar")):
        if os.path.exists(c):
            return os.path.abspath(c)
    return os.path.join(here, "freerouting.jar")


JAR = _find_jar()


def main():
    if not os.path.exists(JAR):
        print("freerouting.jar not found (looked at %s)" % JAR)
        print("It is a ~30MB binary and is deliberately not committed here.")
        print("Download it from https://github.com/freerouting/freerouting/releases")
        print("and either drop it in tools/ or set FREEROUTING_JAR to its path.")
        return 2

    for f in (DSN, SES):
        if os.path.exists(f):
            os.remove(f)

    board = pcbnew.LoadBoard(PCB)
    if not pcbnew.ExportSpecctraDSN(board, DSN):
        print("DSN export failed")
        return 1
    print("exported %s (%.0f KB)" % (os.path.basename(DSN),
                                     os.path.getsize(DSN) / 1024.0))

    # -mt 1 forces single-threaded optimisation. Freerouting prints this on every
    # run: "Multi-threaded route optimization is broken and it is known to
    # generate clearance violations." It is not idle advice -- running it
    # multi-threaded produced five shorting_items and two clearance violations
    # between LED_STAT and RTS, in sub-millimetre track fragments. Slower and
    # correct beats faster and wrong.
    cmd = ["java", "-jar", JAR, "-de", DSN, "-do", SES, "-mp", "10", "-mt", "1"]
    print("routing: %s" % " ".join(cmd[:3] + ["..."]))
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=1800)
    tail = [l for l in (r.stdout + r.stderr).splitlines() if l.strip()][-6:]
    for l in tail:
        print("   " + l[:150])

    if not os.path.exists(SES):
        print("freerouting produced no .ses")
        return 1
    print("produced %s (%.0f KB)" % (os.path.basename(SES),
                                     os.path.getsize(SES) / 1024.0))

    board = pcbnew.LoadBoard(PCB)
    if not pcbnew.ImportSpecctraSES(board, SES):
        print("SES import failed")
        return 1
    pcbnew.SaveBoard(PCB, board)

    tracks = len(board.GetTracks())
    print("imported: board now has %d track/via objects" % tracks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
