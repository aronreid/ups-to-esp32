#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the Rev A board outline and floorplan as a .kicad_pcb.

This is a FLOORPLAN, not finished layout. It fixes the decisions that are
expensive to get wrong and tedious to redo -- board size, where the module sits
relative to its antenna keepout, and where the connectors land -- and assigns
every pad its net so the ratsnest is correct the moment you open it.

Routing, final passive placement and pours are yours. Passives are parked in a
grid on the top layer; select them and press F in the GUI to flip them to the
bottom, which KiCad does correctly and which is safer than me rewriting layer
names by hand.

THE ANTENNA. The module's pads span 18.7mm but its courtyard is 33.5mm; the
difference is the keepout. The board's top edge is placed just above the topmost
module pad, so the antenna end of the module hangs in free air. The outline then
IS the keepout -- no copper can sit under the antenna because no board does.
Nothing metallic should come within ~15mm of it in the enclosure either.

Usage: tools/gen-pcb.py
"""
import json, os, re, subprocess, sys, tempfile, uuid

# Locate KiCad without hardcoding one machine's install path.
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ""))
import kicad_paths

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FP = kicad_paths.footprints()
KC = kicad_paths.cli()
SCH = os.path.join(ROOT, "hardware", "kicad", "ups-adaptor.kicad_sch")
OUT = os.path.join(ROOT, "hardware", "kicad", "ups-adaptor.kicad_pcb")

# 36 x 58. Width is set by the connectors, not the module: the right-angle
# USB-A courtyard is 20.7mm and USB-C is 10.6mm, so both on one edge need ~34mm
# before margins. The module needs only 19mm. Swapping to a compact right-angle
# USB-A is the single biggest size lever left.
#
# No mounting holes. Two M3 need ~6mm each with annulus and clearance, which
# would push the width up for no functional gain on a board this light. The
# enclosure retains it by the edges -- see docs/hardware.md, Mechanical.
BOARD_W, BOARD_H = 36.0, 58.0
OX, OY = 100.0, 60.0          # board origin on the page

# Deliberate placements, in board-local mm. Everything else is parked.
#   ref: (x, y, rotation)
# Only the placements that are MECHANICALLY constrained are fixed here. Where a
# part sits inside the board is a routing decision, and routing decisions are
# better made in the GUI with the ratsnest visible than guessed from a table.
# Fixing them here would produce a floorplan that looks finished and is not.
PLACED = {
    # Module centred, its topmost pad 0.8mm inside the board edge, antenna end
    # hanging in free air so the outline itself is the keepout.
    "U1": (18.0, 6.5, 0),
    # Both USB connectors on the bottom edge: as far from the antenna as the
    # board allows, and on the same face so one enclosure wall carries both.
    "J2": (15.5, 41.0, 0),     # USB-A host port to the UPS -- the wide one
    "J1": (30.0, 41.0, 0),     # USB-C power, programming, console
    # Expansion header along the left edge, where a case can expose it.
    "J5": (2.5, 18.5, 90 * 0), # 3V3, 5V, GND, IO10-13
}

PARK_ORIGIN = (32.0, 2.0)     # off-board grid for everything not placed
PARK_PITCH = (3.0, 2.5)
PARK_COLS = 6


def uid():
    return str(uuid.uuid4())


def netlist():
    tmp = tempfile.mktemp(suffix=".net")
    subprocess.run([KC, "sch", "export", "netlist", "--format", "kicadsexpr",
                    "-o", tmp, SCH], capture_output=True, text=True)
    text = open(tmp).read()
    pad_net, order = {}, []
    for b in re.split(r"\n\t\t\(net\n", text)[1:]:
        nm = re.search(r'\(name "([^"]+)"\)', b)
        if not nm:
            continue
        name = nm.group(1)
        if name not in order:
            order.append(name)
        for ref, pin in re.findall(r'\(ref "([^"]+)"\)\s*\n\s*\(pin "([^"]+)"\)', b):
            pad_net[(ref, pin)] = name
    comps = {}
    for ref, body in re.findall(r'\(comp\s*\(ref "([^"]+)"\)(.*?)(?=\n\t\t\(comp|\n\t\)\n)',
                                text, re.S):
        if ref.startswith("#"):
            continue
        f = re.search(r'\(footprint "([^"]*)"\)', body)
        v = re.search(r'\(value "([^"]*)"\)', body)
        if f and f.group(1):
            comps[ref] = (f.group(1), v.group(1) if v else "")
    return pad_net, order, comps


def load_mod(libname):
    lib, name = libname.split(":", 1)
    return open(os.path.join(FP, lib + ".pretty", name + ".kicad_mod")).read()


def inject_nets(body, ref, pad_net, netnum):
    """Add (net N "name") to each pad that has one."""
    out, i = [], 0
    while True:
        m = re.compile(r'\n(\t*)\(pad "([^"]*)"').search(body, i)
        if not m:
            out.append(body[i:])
            break
        out.append(body[i:m.start()])
        depth, j = 0, m.start() + 1
        while j < len(body):
            if body[j] == "(":
                depth += 1
            elif body[j] == ")":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        block = body[m.start():j + 1]
        net = pad_net.get((ref, m.group(2)))
        if net:
            block = block[:-1].rstrip() + '\n%s\t(net %d "%s")\n%s)' % (
                m.group(1), netnum[net], net, m.group(1))
        out.append(block)
        i = j + 1
    return "".join(out)


def courtyard(libname):
    lib, name = libname.split(":", 1)
    t = open(os.path.join(FP, lib + ".pretty", name + ".kicad_mod")).read()
    xs, ys = [], []
    for m in re.finditer(r"\(fp_(?:line|rect|poly|circle)\b(.*?)\n\t\)", t, re.S):
        if '"F.CrtYd"' not in m.group(1):
            continue
        for a, b in re.findall(r"\((?:start|end|center|xy) ([-\d.]+) ([-\d.]+)\)", m.group(1)):
            xs.append(float(a)); ys.append(float(b))
    if not xs:
        return None
    return min(xs), min(ys), max(xs), max(ys)


def rotate_box(b, r):
    """Rotate a bounding box about the footprint origin, KiCad's sense."""
    x0, y0, x1, y1 = b
    r %= 360
    if r == 0:
        return b
    if r == 90:
        return (-y1, x0, -y0, x1)
    if r == 180:
        return (-x1, -y0, -x0, -y1) if False else (-x1, -y1, -x0, -y0)
    return (y0, -x1, y1, -x0)


def check_placement(comps):
    """A floorplan whose parts overlap is worse than no floorplan: it looks
    finished. U1 is compared by module BODY, because its courtyard is the 15mm
    antenna keepout and parts beside the module are an RF question, not a
    collision."""
    boxes, problems = {}, []
    for ref, (x, y, rot) in PLACED.items():
        if ref not in comps:
            continue
        c = courtyard(comps[ref][0])
        if not c:
            continue
        if ref == "U1":
            c = (-9.0, -12.75, 9.0, 12.75)      # module body, not keepout
        c = rotate_box(c, rot)
        boxes[ref] = (x + c[0], y + c[1], x + c[2], y + c[3])

    for ref, b in sorted(boxes.items()):
        if b[0] < 0 or b[1] < 0 or b[2] > BOARD_W or b[3] > BOARD_H:
            # The module is meant to overhang the top edge; nothing else is.
            if not (ref == "U1" and b[1] < 0 and b[0] >= 0 and b[2] <= BOARD_W):
                problems.append("%s extends outside the board: x %.1f..%.1f y %.1f..%.1f"
                                % (ref, b[0], b[2], b[1], b[3]))
    refs = sorted(boxes)
    for i, a in enumerate(refs):
        for bb in refs[i + 1:]:
            A, B = boxes[a], boxes[bb]
            ox = min(A[2], B[2]) - max(A[0], B[0])
            oy = min(A[3], B[3]) - max(A[1], B[1])
            if ox > 0 and oy > 0:
                problems.append("%s and %s overlap by %.1f x %.1f mm" % (a, bb, ox, oy))
    return problems



# ---------------------------------------------------------------- auto-placer
CLEARANCE = 0.3          # mm between courtyards
GRID = 0.5               # candidate step
EDGE_MARGIN = 0.6        # keep copper off the board edge
ANTENNA_STRIP = 2.5      # top strip left clear of the antenna's near field

# Everything stays on the top layer. Flipping parts to the bottom would buy area,
# but a hand-written mirrored footprint is a class of error that survives DRC and
# reaches fab, and this board fits without it.


def module_body_box():
    """U1's physical extent, plus margin. Its courtyard is the antenna keepout,
    which is a copper question, not a placement one."""
    x, y, _ = PLACED["U1"]
    return (x - 9.0 - CLEARANCE, y - 12.75, x + 9.0 + CLEARANCE, y + 12.75 + CLEARANCE)


def overlaps(a, b):
    return (min(a[2], b[2]) - max(a[0], b[0]) > 0 and
            min(a[3], b[3]) - max(a[1], b[1]) > 0)


def auto_place(comps, pad_net):
    """Greedy placement: biggest parts first, each put where it fits closest to
    the parts it shares nets with. Not a router-aware placer, but it keeps
    decoupling near what it decouples instead of scattering it."""
    boxes = {}
    for ref, (x, y, rot) in PLACED.items():
        if ref not in comps:
            continue
        c = courtyard(comps[ref][0])
        if not c:
            continue
        if ref == "U1":
            c = (-9.0, -12.75, 9.0, 12.75)
        c = rotate_box(c, rot)
        boxes[ref] = (x + c[0], y + c[1], x + c[2], y + c[3])

    blocked = [module_body_box()]

    nets_of = {}
    for (ref, _pin), net in pad_net.items():
        nets_of.setdefault(ref, set()).add(net)

    todo = []
    for ref in comps:
        if ref in PLACED:
            continue
        c = courtyard(comps[ref][0])
        if not c:
            continue
        todo.append((( c[2] - c[0]) * (c[3] - c[1]), ref, c))
    todo.sort(reverse=True)

    placed = dict(PLACED)
    failed = []

    for _area, ref, c in todo:
        mine = nets_of.get(ref, set()) - {"GND", "+3V3", "+5V"}
        targets = []
        for other, ob in boxes.items():
            if mine & (nets_of.get(other, set()) - {"GND", "+3V3", "+5V"}):
                targets.append(((ob[0] + ob[2]) / 2.0, (ob[1] + ob[3]) / 2.0))
        if targets:
            tx = sum(t[0] for t in targets) / len(targets)
            ty = sum(t[1] for t in targets) / len(targets)
        else:
            tx, ty = BOARD_W / 2.0, BOARD_H / 2.0

        best = None
        for rot in (0, 90):
            rc = rotate_box(c, rot)
            w, h = rc[2] - rc[0], rc[3] - rc[1]
            x = EDGE_MARGIN - rc[0]
            while x + rc[2] <= BOARD_W - EDGE_MARGIN + 1e-9:
                y = max(EDGE_MARGIN, ANTENNA_STRIP) - rc[1]
                while y + rc[3] <= BOARD_H - EDGE_MARGIN + 1e-9:
                    box = (x + rc[0] - CLEARANCE / 2, y + rc[1] - CLEARANCE / 2,
                           x + rc[2] + CLEARANCE / 2, y + rc[3] + CLEARANCE / 2)
                    clash = any(overlaps(box, b) for b in boxes.values()) or \
                            any(overlaps(box, b) for b in blocked)
                    if not clash:
                        cost = ((x - tx) ** 2 + (y - ty) ** 2) ** 0.5
                        if best is None or cost < best[0]:
                            best = (cost, x, y, rot,
                                    (x + rc[0], y + rc[1], x + rc[2], y + rc[3]))
                    y += GRID
                x += GRID
        if best is None:
            failed.append(ref)
            continue
        _cost, x, y, rot, box = best
        placed[ref] = (x, y, rot)
        boxes[ref] = box

    used = sum((b[2] - b[0]) * (b[3] - b[1]) for b in boxes.values())
    return placed, failed, used


def main():
    pad_net, order, comps = netlist()
    layout, failed, used = auto_place(comps, pad_net)
    if failed:
        print("could not place %d parts on a %g x %g board: %s"
              % (len(failed), BOARD_W, BOARD_H, ", ".join(sorted(failed))))
        return 1
    netnum = {n: i + 1 for i, n in enumerate(order)}

    o = []
    o.append("(kicad_pcb")
    o.append("\t(version 20241229)")
    o.append('\t(generator "ups-adaptor gen-pcb.py")')
    o.append('\t(generator_version "9.0")')
    o.append("\t(general\n\t\t(thickness 1.6)\n\t\t(legacy_teardrops no)\n\t)")
    o.append('\t(paper "A4")')
    o.append("\t(layers")
    for n, nm, ty in [(0, "F.Cu", "signal"), (31, "B.Cu", "signal"),
                      (32, "B.Adhes", "user"), (33, "F.Adhes", "user"),
                      (34, "B.Paste", "user"), (35, "F.Paste", "user"),
                      (36, "B.SilkS", "user"), (37, "F.SilkS", "user"),
                      (38, "B.Mask", "user"), (39, "F.Mask", "user"),
                      (40, "Dwgs.User", "user"), (41, "Cmts.User", "user"),
                      (42, "Eco1.User", "user"), (43, "Eco2.User", "user"),
                      (44, "Edge.Cuts", "user"), (45, "Margin", "user"),
                      (46, "B.CrtYd", "user"), (47, "F.CrtYd", "user"),
                      (48, "B.Fab", "user"), (49, "F.Fab", "user")]:
        o.append('\t\t(%d "%s" %s)' % (n, nm, ty))
    o.append("\t)")
    # Board setup stays minimal and valid. Design rules live in the .kicad_pro,
    # which is where KiCad 7+ reads them from; putting them in the board file
    # produces a file KiCad refuses to load.
    o.append("\t(setup\n\t\t(pad_to_mask_clearance 0.05)\n\t)")

    o.append('\t(net 0 "")')
    for n in order:
        o.append('\t(net %d "%s")' % (netnum[n], n))

    # Board outline. The top edge stops short of the antenna on purpose.
    edge = [(0, 0), (BOARD_W, 0), (BOARD_W, BOARD_H), (0, BOARD_H), (0, 0)]
    for (x1, y1), (x2, y2) in zip(edge, edge[1:]):
        o.append('\t(gr_line\n\t\t(start %g %g)\n\t\t(end %g %g)'
                 '\n\t\t(stroke (width 0.1) (type default))'
                 '\n\t\t(layer "Edge.Cuts")\n\t\t(uuid "%s")\n\t)'
                 % (OX + x1, OY + y1, OX + x2, OY + y2, uid()))

    # A note on the layer that documents intent for whoever routes this.
    for txt, x, y in [
        ("ANTENNA OVERHANGS THIS EDGE - no copper, keep 15mm metal-free", 13, -3),
        ("ups-adaptor Rev A  26 x 42 mm  floorplan, not routed", 13, BOARD_H + 3),
    ]:
        o.append('\t(gr_text "%s"\n\t\t(at %g %g)\n\t\t(layer "Cmts.User")'
                 '\n\t\t(uuid "%s")\n\t\t(effects (font (size 1 1) (thickness 0.15)))\n\t)'
                 % (txt, OX + x, OY + y, uid()))

    # Ground pours on both layers, defined here as file content rather than
    # built through the Python API: constructing a ZONE and handing it a
    # Python-owned SHAPE_POLY_SET leaves pcbnew holding a dangling pointer and
    # segfaults on fill. Filling is done separately by tools/pour-pcb.py.
    #
    # The bottom pour is the one that matters: a 12 Mbps USB pair wants
    # continuous ground directly beneath it, which is why the passives were
    # flipped to the bottom and left it mostly empty.
    # Routing keepouts around the perimeter. Freerouting works to the board
    # outline and knows nothing about the edge clearance rule, so it will route
    # a track a fifth of a millimetre from the edge.
    #
    # The TOP strip is deliberately wider than the rest. That edge is where the
    # antenna projects, and a track running along it sits in the antenna's near
    # field -- an RF problem the edge-clearance rule would never mention.
    #
    # copperpour stays ALLOWED: the ground pour should still reach the edge, it
    # is tracks and vias that must not.
    EDGE_KO, TOP_KO = 0.45, 2.0
    strips = [
        ("top",    0, 0, BOARD_W, TOP_KO),
        ("bottom", 0, BOARD_H - EDGE_KO, BOARD_W, EDGE_KO),
        ("left",   0, 0, EDGE_KO, BOARD_H),
        ("right",  BOARD_W - EDGE_KO, 0, EDGE_KO, BOARD_H),
    ]
    for name, kx, ky, kw, kh in strips:
        o.append("\t(zone")
        o.append("\t\t(net 0)")
        o.append('\t\t(net_name "")')
        o.append('\t\t(layers "F.Cu" "B.Cu")')
        o.append('\t\t(uuid "%s")' % uid())
        o.append('\t\t(name "edge keepout %s")' % name)
        o.append("\t\t(hatch edge 0.5)")
        o.append("\t\t(connect_pads\n\t\t\t(clearance 0)\n\t\t)")
        o.append("\t\t(min_thickness 0.25)")
        o.append("\t\t(keepout")
        o.append("\t\t\t(tracks not_allowed)")
        o.append("\t\t\t(vias not_allowed)")
        o.append("\t\t\t(pads allowed)")
        o.append("\t\t\t(copperpour allowed)")
        o.append("\t\t\t(footprints allowed)")
        o.append("\t\t)")
        o.append("\t\t(polygon\n\t\t\t(pts")
        for px, py in ((kx, ky), (kx + kw, ky), (kx + kw, ky + kh), (kx, ky + kh)):
            o.append("\t\t\t\t(xy %g %g)" % (OX + px, OY + py))
        o.append("\t\t\t)\n\t\t)")
        o.append("\t)")

    gnd = netnum.get("GND")
    if gnd:
        inset = 0.3
        pts = [(inset, inset), (BOARD_W - inset, inset),
               (BOARD_W - inset, BOARD_H - inset), (inset, BOARD_H - inset)]
        for layer in ("F.Cu", "B.Cu"):
            o.append("\t(zone")
            o.append("\t\t(net %d)" % gnd)
            o.append('\t\t(net_name "GND")')
            o.append('\t\t(layer "%s")' % layer)
            o.append('\t\t(uuid "%s")' % uid())
            o.append("\t\t(hatch edge 0.5)")
            # Solid, not thermal relief. The board is reflow assembled, so the
            # hand-soldering argument for thermal relief does not apply, and an
            # 0402 ground pad cannot resolve the two spokes the rule wants --
            # which is what every starved_thermal violation was.
            o.append("\t\t(connect_pads yes\n\t\t\t(clearance 0.25)\n\t\t)")
            o.append("\t\t(min_thickness 0.2)")
            o.append("\t\t(filled_areas_thickness no)")
            # island_removal_mode 1 discards pour islands that reach nothing.
            # Keeping them would leave isolated copper that DRC rightly calls an
            # unconnected GND item, and that does no work on the board.
            # island_removal_mode 2 with a minimum area drops every fragment
            # below the threshold, connected or not. The pour fragments into
            # dozens of slivers around routing -- 37 on the front, 25 on the
            # back -- and a two-square-millimetre crumb of copper does no work
            # whether or not a via reaches it. Removing them leaves the handful
            # of real regions, which stitching can then tie to the plane.
            o.append("\t\t(fill yes\n\t\t\t(thermal_gap 0.4)"
                     "\n\t\t\t(thermal_bridge_width 0.4)"
                     "\n\t\t\t(island_removal_mode 2)"
                     "\n\t\t\t(island_area_min 2.0)\n\t\t)")
            o.append("\t\t(polygon\n\t\t\t(pts")
            for px, py in pts:
                o.append("\t\t\t\t(xy %g %g)" % (OX + px, OY + py))
            o.append("\t\t\t)\n\t\t)")
            o.append("\t)")

    parked = 0
    bumped = {}
    for ref in sorted(comps):
        libname, value = comps[ref]
        try:
            mod = load_mod(libname)
        except FileNotFoundError:
            print("  skip %s: footprint missing %s" % (ref, libname))
            continue

        if ref in layout:
            x, y, rot = layout[ref]
        else:
            c, r = parked % PARK_COLS, parked // PARK_COLS
            x = PARK_ORIGIN[0] + c * PARK_PITCH[0]
            y = PARK_ORIGIN[1] + r * PARK_PITCH[1]
            rot = 0
            parked += 1

        body = mod

        # JLCPCB's standard 2-layer minimum drill is 0.3mm; 0.2mm is an
        # advanced-process option that costs more. The stock ESP32-S3-WROOM-1
        # footprint stitches its thermal pad with twelve 0.2mm vias. Their pads
        # are 0.6mm, so 0.3mm still leaves a 0.15mm annulus -- inside the
        # 0.13mm rule. Bump them rather than pay for finer drilling on a via
        # whose only job is heat.
        def _bump(m):
            d = float(m.group(1))
            return "(drill %g)" % (0.3 if d < 0.3 else d)
        before = body
        body = re.sub(r"\(drill ([0-9.]+)\)", _bump, body)
        if body != before:
            bumped[ref] = body.count("(drill 0.3)") - before.count("(drill 0.3)")

        body = re.sub(r'^\(footprint "([^"]+)"',
                      '(footprint "%s"' % libname, body, count=1)
        body = inject_nets(body, ref, pad_net, netnum)
        # Position only -- NEVER a rotation. In a .kicad_pcb each pad's angle
        # must already include the footprint's rotation, while in a .kicad_mod
        # it does not: pcbnew rotates pad POSITIONS with the footprint but not
        # their SHAPES. Writing a rotation here silently produces pads that
        # overlap their neighbours. All rotation is applied by place-pcb.py
        # through pcbnew, which gets it right.
        head = '(footprint "%s"\n\t\t(layer "F.Cu")\n\t\t(uuid "%s")\n\t\t(at %g %g)' % (
            libname, uid(), OX + x, OY + y)
        body = re.sub(r'^\(footprint "[^"]+"', head, body, count=1)
        body = body.replace('(property "Reference" "REF**"',
                            '(property "Reference" "%s"' % ref, 1)
        body = re.sub(r'\(property "Value" "[^"]*"',
                      '(property "Value" "%s"' % value.replace('"', ""), body, count=1)
        o.append("\n".join("\t" + l for l in body.split("\n")))

    o.append(")")
    open(OUT, "w").write("\n".join(o) + "\n")
    # KiCad 7+ keeps design rules in the project file. Writing them makes DRC
    # meaningful rather than complaining about unset defaults.
    pro = OUT.replace(".kicad_pcb", ".kicad_pro")
    existing = {}
    if os.path.exists(pro):
        try:
            existing = json.load(open(pro))
        except Exception:
            existing = {}
    existing.setdefault("meta", {"filename": os.path.basename(pro), "version": 1})
    ds = existing.setdefault("board", {}).setdefault("design_settings", {})
    ds["rules"] = {
        "min_clearance": 0.2, "min_track_width": 0.2,
        "min_through_hole_diameter": 0.3, "min_via_annular_width": 0.13,
        "min_via_diameter": 0.6, "min_hole_clearance": 0.25,
        "min_hole_to_hole": 0.5, "min_copper_edge_clearance": 0.3,
        "min_silk_clearance": 0.0, "min_text_height": 0.8,
        "min_text_thickness": 0.08, "min_connection": 0.0,
        "min_resolved_spokes": 2,
    }
    # Power nets get their own class. Everything used to route at the Default
    # 0.2 mm, including +5V (78 mm of it) and +3V3 (127 mm): a signal-width
    # track is 2.4 mOhm per mm, so the run from the regulator to the module was
    # a tenth of an ohm in series with a supply that steps 300 mA on every Wi-Fi
    # burst, fed from an LDO already short of headroom on a sagging USB 5 V. At
    # 0.4 mm the drop halves and 1 A -- the UPS port in a fault -- stays inside a
    # 10 C rise. Wider would be better electrically but will not leave a 0.6 mm
    # USB-C pad without fouling its neighbour. GND is a pour and needs no class.
    POWER_NETS = ["+5V", "5V_RAW", "+3V3", "VBUS_A"]
    ns = existing.setdefault("net_settings", {})
    classes = [c for c in ns.get("classes", []) if c.get("name") != "Power"]
    if not classes:
        classes = [{"name": "Default", "clearance": 0.2, "track_width": 0.2,
                    "via_diameter": 0.6, "via_drill": 0.3,
                    "priority": 2147483647}]
    power = dict(classes[0])
    power.update({"name": "Power", "track_width": 0.4, "priority": 0})
    classes.append(power)
    ns["classes"] = classes
    ns["netclass_patterns"] = [{"netclass": "Power", "pattern": n}
                               for n in POWER_NETS]
    ns.setdefault("meta", {"version": 5})
    json.dump(existing, open(pro, "w"), indent=2)

    if bumped:
        for ref, n in sorted(bumped.items()):
            print("bumped %d sub-0.3mm drills to 0.3mm in %s" % (n, ref))

    print("wrote %s" % OUT)
    print("board %g x %g mm = %.0f mm2" % (BOARD_W, BOARD_H, BOARD_W * BOARD_H))
    print("%d parts placed, %d parked, %d nets, %.0f%% courtyard utilisation"
          % (len(layout), parked, len(order), 100.0 * used / (BOARD_W * BOARD_H)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
