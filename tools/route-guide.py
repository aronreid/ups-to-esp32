#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Draw a colour-coded routing guide: what is still unconnected, and where.

Reads the live DRC report and the board, and emits an SVG then a PDF showing
each missing connection as a coloured line between its two endpoints -- one
colour per net, so you match red to red, blue to blue.

It is generated from the DRC report rather than a hand-kept list, so re-running
it after routing something shows the shorter list. When everything is done it
says so.

    $KICAD_PY tools/route-guide.py        # writes docs/route-guide.pdf
"""
import os
import re
import subprocess
import sys

import pcbnew

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "test"))
import test_ground

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PCB = os.path.join(ROOT, "hardware", "kicad", "ups-adaptor.kicad_pcb")
OUT_SVG = os.path.join(ROOT, "docs", "route-guide.svg")
OUT_PDF = os.path.join(ROOT, "docs", "route-guide.pdf")
KCLI = os.environ.get("KICAD_CLI",
                      "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli")
RSVG = os.environ.get("RSVG", "rsvg-convert")

SCALE = 4.0          # mm on the page per mm of board
MARGIN = 10.0        # mm around the board
LEGEND_H = 52.0      # mm reserved under the board

# One colour per net, in order of appearance.
GND_COL = "#b5179e"     # floating grounds, distinct from every net colour

PALETTE = [("#d92b2b", "RED"), ("#1f6fd0", "BLUE"), ("#1f9e4a", "GREEN"),
           ("#c46a00", "ORANGE"), ("#8b3fbf", "PURPLE"), ("#0f9aa8", "TEAL")]


def mm(v):
    return pcbnew.ToMM(v)


def drc_unconnected_pads():
    """Pads DRC reports as unconnected: [(ref, padnum, net), ...].

    DRC pairs an unconnected item with whatever is NEAREST, which is often on a
    different net -- it reported SW2's IO0 pad against an EN track. Pairing is
    therefore useless for drawing "connect this to that"; only the pad itself is
    reliable. The other end is worked out from the board, below."""
    rpt = "/tmp/route-guide-drc.rpt"
    subprocess.run([KCLI, "pcb", "drc", "--severity-error", "-o", rpt, PCB],
                   capture_output=True, text=True)
    if not os.path.exists(rpt):
        return None
    out = []
    for block in re.split(r"\n(?=\[)", open(rpt).read()):
        if not block.startswith("[unconnected_items]"):
            continue
        for m in re.finditer(r"Pad (\S+) \[([^\]]+)\] of (\w+)", block):
            entry = (m.group(3), m.group(1), m.group(2))
            if entry not in out:
                out.append(entry)
    return out


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def main():
    board = pcbnew.LoadBoard(PCB)
    bb = board.GetBoardEdgesBoundingBox()
    BL, BT = mm(bb.GetLeft()), mm(bb.GetTop())
    BW, BH = mm(bb.GetWidth()), mm(bb.GetHeight())

    orphans = drc_unconnected_pads()
    if orphans is None:
        print("DRC did not run")
        return 1

    # For each orphan pad, find the nearest thing on its OWN net to connect to.
    conns = []
    drawn = set()
    for ref, num, net in orphans:
        fp = board.FindFootprintByReference(ref)
        if fp is None:
            continue
        pad = next((p for p in fp.Pads() if p.GetNumber() == num), None)
        if pad is None:
            continue
        ppos = pad.GetPosition()
        best = None
        for ofp in board.GetFootprints():
            for op in ofp.Pads():
                if op.GetNetname() != net or op is pad:
                    continue
                if ofp.GetReference() == ref and op.GetNumber() == num:
                    continue      # the other half of the same bonded pin pair
                q = op.GetPosition()
                d = (mm(q.x - ppos.x) ** 2 + mm(q.y - ppos.y) ** 2) ** 0.5
                if best is None or d < best[0]:
                    best = (d, mm(q.x), mm(q.y),
                            "Pad %s of %s on %s" % (op.GetNumber(), ofp.GetReference(),
                                                    "B.Cu" if ofp.IsFlipped() else "F.Cu"))
        if best is None:
            continue
        # When BOTH ends of a connection are reported as orphans -- which is the
        # normal case for a net with nothing routed at all -- each produces the
        # same pair, mirrored. Key on the unordered pair so it is drawn once.
        a = (round(mm(ppos.x), 2), round(mm(ppos.y), 2))
        bpt = (round(best[1], 2), round(best[2], 2))
        key = (net, tuple(sorted([a, bpt])))
        if key in drawn:
            continue
        drawn.add(key)
        conns.append((net, [
            (mm(ppos.x), mm(ppos.y),
             "Pad %s of %s on %s" % (num, ref, "B.Cu" if fp.IsFlipped() else "F.Cu")),
            (best[1], best[2], best[3])]))

    nets = []
    for net, _ in conns:
        if net not in nets:
            nets.append(net)
    colour = {n: PALETTE[i % len(PALETTE)] for i, n in enumerate(nets)}

    W = BW * SCALE + 2 * MARGIN
    H = BH * SCALE + 2 * MARGIN + LEGEND_H

    def px(x):
        return MARGIN + (x - BL) * SCALE

    def py(y):
        return MARGIN + (y - BT) * SCALE

    s = []
    s.append('<?xml version="1.0" encoding="UTF-8"?>')
    s.append('<svg xmlns="http://www.w3.org/2000/svg" width="%gmm" height="%gmm" '
             'viewBox="0 0 %g %g">' % (W, H, W, H))
    s.append('<rect width="%g" height="%g" fill="#ffffff"/>' % (W, H))
    s.append('<g font-family="Helvetica, Arial, sans-serif">')

    # board outline
    s.append('<rect x="%g" y="%g" width="%g" height="%g" rx="1" fill="#f7f9f7" '
             'stroke="#333" stroke-width="0.5"/>'
             % (px(BL), py(BT), BW * SCALE, BH * SCALE))

    # footprints: courtyard box plus reference
    for fp in board.GetFootprints():
        ref = fp.GetReference()
        if ref == "U1":
            # The module's courtyard IS the 48 x 41mm antenna keepout, which
            # drawn as a box swamps the board and hides everything under it.
            # Use the physical can instead.
            p0 = fp.GetPosition()
            x0, y0 = mm(p0.x) - 9.0, mm(p0.y) - 12.75
            w, h = 18.0, 25.5
        else:
            cy = fp.GetCourtyard(pcbnew.B_CrtYd if fp.IsFlipped() else pcbnew.F_CrtYd)
            if cy.IsEmpty():
                continue
            c = cy.BBox()
            x0, y0 = mm(c.GetLeft()), mm(c.GetTop())
            w, h = mm(c.GetWidth()), mm(c.GetHeight())
        back = fp.IsFlipped()
        s.append('<rect x="%g" y="%g" width="%g" height="%g" fill="%s" stroke="%s" '
                 'stroke-width="0.25" %s/>'
                 % (px(x0), py(y0), w * SCALE, h * SCALE,
                    "#eceff1" if back else "#dfe6e9",
                    "#9aa5ab" if back else "#7f8c8d",
                    'stroke-dasharray="1.2 0.8"' if back else ""))
        if w * SCALE > 5 and h * SCALE > 3.5:
            s.append('<text x="%g" y="%g" font-size="2.4" fill="#5a6a72" '
                     'text-anchor="middle">%s</text>'
                     % (px(x0 + w / 2), py(y0 + h / 2) + 0.9, esc(ref)))

    # the connections
    placed_labels = []

    def label_pos(x, y):
        """Nudge a label until it is not sitting on one already placed."""
        for dy in (-2.2, 2.8, -5.2, 5.8, -8.2, 8.8):
            cand = (x + 3.4, y + dy)
            if all(abs(cand[0] - a) > 18 or abs(cand[1] - b) > 3.4
                   for a, b in placed_labels):
                placed_labels.append(cand)
                return cand
        placed_labels.append((x + 3.4, y - 2.2))
        return (x + 3.4, y - 2.2)

    for net, items in conns:
        col, _name = colour[net]
        (x1, y1, t1), (x2, y2, t2) = items
        s.append('<line x1="%g" y1="%g" x2="%g" y2="%g" stroke="%s" '
                 'stroke-width="1.1" stroke-dasharray="3 2" opacity="0.9"/>'
                 % (px(x1), py(y1), px(x2), py(y2), col))
        for (x, y, t) in items:
            back = " on B.Cu" in t
            s.append('<circle cx="%g" cy="%g" r="2.6" fill="%s" stroke="#fff" '
                     'stroke-width="0.7" opacity="%s"/>'
                     % (px(x), py(y), col, "0.45" if back else "1"))
            s.append('<circle cx="%g" cy="%g" r="2.6" fill="none" stroke="%s" '
                     'stroke-width="0.6"/>' % (px(x), py(y), col))
            label = t.split(" of ")
            if len(label) > 1 and t.startswith("Pad"):
                ref_ = label[1].split(" on ")[0]
                short = "%s pin %s" % (ref_, t.split()[1])
            else:
                short = t.split()[0]
            lx, ly_ = label_pos(px(x), py(y))
            s.append('<text x="%g" y="%g" font-size="2.6" font-weight="bold" '
                     'fill="%s">%s%s</text>'
                     % (lx, ly_, col, esc(short), " (back)" if back else ""))

    # floating grounds: a separate failure from an unrouted net, and one DRC
    # does not report at all, so it gets its own mark and its own legend entry.
    res = test_ground.analyse(board)
    floating = res[0] if res else []
    for ref, num, pos, back in sorted(floating, key=lambda t: (t[0], t[1])):
        fx, fy = px(mm(pos.x)), py(mm(pos.y))
        s.append('<rect x="%g" y="%g" width="6" height="6" fill="none" '
                 'stroke="%s" stroke-width="1.3"/>' % (fx - 3, fy - 3, GND_COL))
        s.append('<line x1="%g" y1="%g" x2="%g" y2="%g" stroke="%s" '
                 'stroke-width="1.1"/>' % (fx - 2, fy - 2, fx + 2, fy + 2, GND_COL))
        s.append('<line x1="%g" y1="%g" x2="%g" y2="%g" stroke="%s" '
                 'stroke-width="1.1"/>' % (fx + 2, fy - 2, fx - 2, fy + 2, GND_COL))
        lx, ly_ = label_pos(fx, fy)
        s.append('<text x="%g" y="%g" font-size="2.6" font-weight="bold" '
                 'fill="%s">%s pin %s &#8212; GND%s</text>'
                 % (lx, ly_, GND_COL, esc(ref), esc(num), " (back)" if back else ""))

    # outline again on top, so it reads as the board edge rather than being
    # hidden under footprint boxes
    s.append('<rect x="%g" y="%g" width="%g" height="%g" rx="1" fill="none" '
             'stroke="#222" stroke-width="0.7"/>'
             % (px(BL), py(BT), BW * SCALE, BH * SCALE))

    # legend
    ly = MARGIN + BH * SCALE + 7
    s.append('<text x="%g" y="%g" font-size="4" font-weight="bold" fill="#111">'
             'To fix: %d connection%s to route, %d floating ground%s</text>'
             % (MARGIN, ly, len(conns), "" if len(conns) == 1 else "s",
                len(floating), "" if len(floating) == 1 else "s"))
    ly += 6
    if floating:
        s.append('<rect x="%g" y="%g" width="4.4" height="4.4" fill="none" '
                 'stroke="%s" stroke-width="1.1"/>' % (MARGIN - 0.2, ly - 3.4, GND_COL))
        s.append('<text x="%g" y="%g" font-size="3.1" fill="#222">'
                 '<tspan font-weight="bold">CROSSED BOX</tspan>  &#8212;  GND pad not '
                 'connected to the ground plane (DRC does not report these)</text>'
                 % (MARGIN + 6.5, ly))
        ly += 5.2
    for net in nets:
        col, name = colour[net]
        s.append('<circle cx="%g" cy="%g" r="2.2" fill="%s"/>' % (MARGIN + 2, ly - 1.2, col))
        s.append('<text x="%g" y="%g" font-size="3.1" fill="#222">'
                 '<tspan font-weight="bold">%s</tspan>  &#8212;  net %s</text>'
                 % (MARGIN + 6.5, ly, name, esc(net)))
        ly += 5.2
    for i, line in enumerate([
            "Solid marker = pad on the FRONT (F.Cu). Faded marker and dashed outline "
            "= pad on the BACK (B.Cu).",
            "The dashed line is the connection to MAKE, not a route to follow -- "
            "take whatever path the board allows.",
            "A floating ground needs a via beside the pad, landing in the plane on "
            "the other layer, plus a short track to it.",
            "Board %.1f x %.1f mm, drawn at %gx. Ground pour islands are not shown."
            % (BW, BH, SCALE)]):
        s.append('<text x="%g" y="%g" font-size="2.7" fill="#667">%s</text>'
                 % (MARGIN, ly + 1.5 + i * 4.0, line))

    s.append("</g></svg>")

    os.makedirs(os.path.dirname(OUT_SVG), exist_ok=True)
    open(OUT_SVG, "w").write("\n".join(s))

    r = subprocess.run([RSVG, "-f", "pdf", "-o", OUT_PDF, OUT_SVG],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print("svg written, pdf conversion failed:", r.stderr.strip()[:200])
        return 1

    if floating:
        print("%d floating GND pad%s: %s"
              % (len(floating), "" if len(floating) == 1 else "s",
                 ", ".join("%s pin %s" % (r, n) for r, n, _p, _b in sorted(floating))))
    if not conns and not floating:
        print("nothing left to fix -- the guide is empty")
    elif not conns:
        print("all nets routed; only floating grounds remain")
    else:
        print("%d connection%s across %d net%s: %s"
              % (len(conns), "" if len(conns) == 1 else "s",
                 len(nets), "" if len(nets) == 1 else "s",
                 ", ".join("%s=%s" % (colour[n][1], n) for n in nets)))
    print("wrote %s" % os.path.relpath(OUT_PDF, ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
