#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the 3D-printed case: a tray and a cover that snap together.

    python3 -m pip install manifold3d numpy
    python3 tools/gen-case.py            # writes hardware/enclosure/*.stl

No screws and nothing metallic. The board drops into the tray, sits on ledges
3 mm above the floor (J3 and J5 pins stick out 1.5 mm underneath), and the
cover snaps over it. Four posts inside the cover press the board down onto the
ledges, so the board is held once the case is closed.

The two parts split at the top face of the board. Both USB openings are notches
that start at the bottom edge of the cover's wall, so the cover drops straight
down over the connectors. A lip on the tray's two long sides and antenna end
slides inside a rebate in the cover. Ridges on the lip click into grooves in
the rebate. To open the case, squeeze the long sides and lift.

Every position comes from hardware/kicad/ups-adaptor.kicad_pcb, board frame:
u = X - 100, v = Y - 60 (KiCad Y, pointing toward the connectors). The board
covers u 0..36 and v 0..58. The connectors stick 0.45 mm past v = 58 and the
module's antenna overhangs to v = -6.3. Part heights are from KiCad's 3D models,
or from the datasheets where KiCad has no model: USB-A XKB U231-091N is 7.0 mm
tall, USB-C HRO TYPE-C-31-M-12 is 3.26 mm, and the headers are 8.6 mm.

Light pipes: two 3 mm rods over D3 and D4 (ups-adaptor-lightpipes.stl), printed
in clear PETG. Each one drops in from the top and rests on a shoulder at the
bottom of its guide tube, which leaves it flush with the top. Ribs near the top
end give it a light friction fit. The rods are separate so the red LED's light
does not reach the yellow pipe. A cut 3 mm acrylic rod works too, cut to
LIGHTPIPE_LEN (printed when this runs). RESET and BOOT are behind pinholes.

The OLED cover (ups-adaptor-cover-oled.stl) fits the same tray. It is taller,
has a window over the display, and drops the light pipes, because the module
sits directly over both LEDs.

The wall-mount tray (ups-adaptor-tray-wall.stl) is the same tray with two
countersunk holes through the floor for #6 / 3.5 mm flat-head wood screws.
Screw the EMPTY tray to the wall, drop the board in, snap the cover on: the
screws are hidden, and the board is never near the screwdriver. Each hole sits
in a raised pad, because the 1.6 mm floor is too thin for a countersunk head,
and the pad stops 1.2 mm under the board. Either cover fits it.

Print both parts without supports. The tray prints as generated. The cover
prints upside down, top face on the bed, which is how it is written out.
"""
import os
import struct
import sys

import numpy as np
from manifold3d import CrossSection, JoinType, Manifold, OpType, set_circular_segments

set_circular_segments(64)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "hardware", "enclosure")

# ---- board, from the PCB ------------------------------------------------------
BOARD_W, BOARD_L, BOARD_T = 36.0, 58.0, 1.6
ANTENNA_V = -6.3            # module overhang past the v = 0 edge
CONN_FRONT_V = 58.45        # J1/J2 fronts, per their F.Fab outlines
TALLEST = 8.63              # J3/J5 above the board top
LEDS = [(15.95, 29.95), (19.95, 29.95)]     # D3 yellow, D4 red
LED_H = 1.2
BUTTONS = [(4.55, 5.45), (31.05, 5.45)]     # SW1 RESET, SW2 BOOT
BUTTON_H = 1.5
USBA_U, USBA_W, USBA_H = 10.99, 12.8, 7.2   # J2 shell; 7.2 over its bumps
USBC_U, USBC_W, USBC_H = 27.34, 8.94, 3.26  # J1 shell

# ---- case ---------------------------------------------------------------------
SLOP = 0.3                  # per side, board to wall (docs/hardware.md)
WALL = 2.4                  # sides and antenna end
END_WALL = 1.4              # connector end, thin so the plugs seat fully
FLOOR = 1.6
ROOF = 1.6
UNDER = 3.0                 # floor to board underside
HEADROOM = 0.9              # tallest part to ceiling
R_OUT = 4.0                 # vertical corners
FILLET = 1.5                # top and bottom edges
R_IN = 1.0                  # cavity corners; any larger clips the board's corners

LIP_T, LIP_H = 1.0, 3.0     # tray lip, above the split
FIT = 0.15                  # lip to rebate, per side
RIDGE = 0.4                 # snap ridge protrusion
RIDGE_LEN = 10.0

LIGHTPIPE_D = 3.0
LIGHTPIPE_HOLE = LIGHTPIPE_D + 0.2
LIGHTPIPE_BOSS = 4.6
LIGHTPIPE_SHOULDER = 0.6    # ledge at the bottom of the guide the rod rests on
LIGHTPIPE_APERTURE = 2.2    # hole through that ledge, for the light
LIGHTPIPE_RIB = 0.05        # interference per side on the friction ribs
LIGHTPIPE_RIB_LEN = 3.0     # ribs only near the top, so only the last push is tight
PINHOLE = 1.6

# Optional 0.96" SSD1306 module on J3. It plugs in with its header along v and
# its body over the board toward -u, 27.3 x 27.8 mm. The generic modules vary by
# a millimetre or so, and the window is sized for that. It is the panel's
# 23.7 x 12.9 viewing area plus a margin. Check it against the module you have
# and move OLED_WIN if it is off.
OLED_J3_U = 31.35
OLED_J3_V = (23.95 + 31.57) / 2             # centre of J3's four pins
OLED_TOP = 8.6 + 2.5 + 1.2 + 1.5            # socket, pin spacer, module PCB, glass
OLED_GAP = 0.3                              # glass to the underside of the roof
OLED_WIN = (20.2, OLED_J3_V, 14.5, 25.0)    # centre u, centre v, size u, size v
OLED_PINS_UP = 2.5                          # module pins standing above its PCB

# Wall-mount tray: screw holes on the centreline, on bare board underside --
# clear of the mid posts, J5, J3, and the USB-A and USB-C shield legs, which
# are the only things that protrude beneath the board (the module's ground
# vias under the first hole are flush). check_wall_mount() enforces it.
WALL_SCREWS = [(18.0, 15.0), (18.0, 38.0)]
SCREW_CLEAR = 3.8           # #6 / M3.5 wood screw shank
SCREW_HEAD = 7.6            # flat-head diameter, 90-degree countersink
SCREW_PAD = 11.0
PAD_BELOW_BOARD = 1.2       # pad top, and so the screw head, to board underside
# Everything that sticks out under the board, (u, v, radius), from the PCB's
# through-hole pads: a pad must keep clear of these, and of the mid posts.
UNDERSIDE = ([(2.45, 18.45 + 2.54 * i, 1.0) for i in range(7)]            # J5
             + [(31.35, 23.95 + 2.54 * i, 1.0) for i in range(4)]         # J3
             + [(4.59, 46.10, 1.2), (17.39, 46.10, 1.2)]                  # J2 shield
             + [(24.45, 52.2, 0.6), (30.23, 52.2, 0.6), (23.02, 51.67, 1.0),
                (23.02, 55.85, 1.0), (31.66, 51.67, 1.0), (31.66, 55.85, 1.0)])  # J1
MID_POSTS = [(18.0, 30.0), (28.0, 40.0)]

# ---- derived ------------------------------------------------------------------
U0, U1 = -SLOP, BOARD_W + SLOP
V0 = ANTENNA_V - 0.7                        # cavity, antenna end
V1 = CONN_FRONT_V + 0.15                    # cavity, connector end
Z_BOT = FLOOR + UNDER                       # board underside
Z_TOP = Z_BOT + BOARD_T                     # board top = split plane
Z_CEIL = Z_TOP + TALLEST + HEADROOM
H = Z_CEIL + ROOF
Z_CEIL_OLED = Z_TOP + OLED_TOP + OLED_GAP
H_OLED = Z_CEIL_OLED + ROOF
OU0, OU1 = U0 - WALL, U1 + WALL
OV0, OV1 = V0 - WALL, V1 + END_WALL
LIP_END_V = 50.0            # lip stops short of the connectors
BOSS_BOT = Z_TOP + LED_H + 1.0              # guide bottom, clear of U3 at 1.8 mm
LIGHTPIPE_LEN = H - (BOSS_BOT + LIGHTPIPE_SHOULDER)
BIG = 200.0


def rrect(u0, v0, u1, v1, r):
    """Rounded rectangle as a 2D cross-section."""
    return (CrossSection.square((u1 - u0 - 2 * r, v1 - v0 - 2 * r))
            .translate((u0 + r, v0 + r))
            .offset(r, JoinType.Round))


def box(u0, v0, z0, u1, v1, z1):
    return Manifold.cube((u1 - u0, v1 - v0, z1 - z0)).translate((u0, v0, z0))


def cyl(u, v, z0, z1, d):
    return Manifold.cylinder(z1 - z0, d / 2).translate((u, v, z0))


def rounded_box(u0, v0, u1, v1, h, r, f):
    """Box with vertical corner radius r and every top and bottom edge
    filleted by f: the hull of four puck shapes."""
    prof = CrossSection.batch_hull([
        CrossSection.square((r - f, h)),
        CrossSection.circle(f).translate((r - f, f)),
        CrossSection.circle(f).translate((r - f, h - f)),
    ])
    puck = prof.revolve()
    return Manifold.batch_hull([puck.translate((x, y, 0))
                                for x in (u0 + r, u1 - r)
                                for y in (v0 + r, v1 - r)])


def stadium(u, z, w, h, v0, v1):
    """Slot running along v, cross-section w x h with round ends, centred at (u, z)."""
    r = h / 2
    cs = CrossSection.batch_hull([CrossSection.circle(r).translate((u - w / 2 + r, z)),
                                  CrossSection.circle(r).translate((u + w / 2 - r, z))])
    return _along_v(cs, v0, v1)


def _along_v(cs, v0, v1):
    # cs is in the (u, z) plane. Extruding gives (u, z, t), and rotating x by
    # +90 maps (u, z, t) to (u, -t, z), so shift it to span v0..v1.
    return cs.extrude(v1 - v0).rotate((90, 0, 0)).translate((0, v1, 0))


def ridge(u_face, outward, v_mid, z, r, protrude, length):
    """Horizontal half-round ridge along v on a wall face at u_face, standing
    `protrude` proud of the face on the `outward` side (+1 or -1)."""
    c = Manifold.cylinder(length, r).rotate((90, 0, 0)).translate((0, v_mid + length / 2, z))
    return c.translate((u_face + outward * (protrude - r), 0, 0))


def wall_holes(tray):
    """Raised pads and countersunk screw holes through the tray floor."""
    top = Z_BOT - PAD_BELOW_BOARD
    sink = (SCREW_HEAD - SCREW_CLEAR) / 2       # 90 degrees: depth = radius step
    for u, v in WALL_SCREWS:
        tray += cyl(u, v, FLOOR - 0.01, top, SCREW_PAD)
    for u, v in WALL_SCREWS:
        tray -= cyl(u, v, -1, top + 1, SCREW_CLEAR)
        tray -= Manifold.cylinder(sink + 0.01, SCREW_CLEAR / 2, SCREW_HEAD / 2 + 0.01) \
            .translate((u, v, top - sink))
    return tray


def check_wall_mount():
    """The pads must clear the board, what sticks out under it, and the posts,
    and leave the countersink room to be a countersink."""
    top = Z_BOT - PAD_BELOW_BOARD
    sink = (SCREW_HEAD - SCREW_CLEAR) / 2
    # The screw passes through floor and pad together; under the countersink
    # there must still be solid material for the head to bear on.
    if top - sink < 1.2:
        sys.exit(f"wall tray: {top:.1f} mm of floor and pad leaves {top - sink:.1f} mm "
                 f"under a {sink:.1f} mm countersink")
    for u, v in WALL_SCREWS:
        if not (U0 + SCREW_PAD / 2 < u < U1 - SCREW_PAD / 2):
            sys.exit(f"wall tray: screw at u={u} leaves the cavity")
        for pu, pv, r in UNDERSIDE:
            gap = ((u - pu) ** 2 + (v - pv) ** 2) ** 0.5 - SCREW_PAD / 2 - r
            if gap < 0.5:
                sys.exit(f"wall tray: pad at ({u}, {v}) is {gap:.1f} mm from a pin at ({pu}, {pv})")
        for pu, pv in MID_POSTS:
            gap = ((u - pu) ** 2 + (v - pv) ** 2) ** 0.5 - SCREW_PAD / 2 - 1.5
            if gap < 0.5:
                sys.exit(f"wall tray: pad at ({u}, {v}) runs into the post at ({pu}, {pv})")


def build(oled=False):
    z_ceil, h = (Z_CEIL_OLED, H_OLED) if oled else (Z_CEIL, H)
    outer = rounded_box(OU0, OV0, OU1, OV1, h, R_OUT, FILLET)
    cavity2d = rrect(U0, V0, U1, V1, R_IN)
    shell = outer - cavity2d.extrude(z_ceil - FLOOR).translate((0, 0, FLOOR))

    # Connector openings: they go through the end wall and start below the
    # split, so they come out as notches in both parts.
    thru = (V1 - 1, OV1 + 1)
    usba = CrossSection.square((USBA_W + 0.8, USBA_H + 0.4 + 0.4 + 1.0)) \
        .translate((USBA_U - (USBA_W + 0.8) / 2, Z_TOP - 0.4 - 1.0))
    usba = usba.offset(-1.0, JoinType.Round).offset(1.0, JoinType.Round)  # round the corners
    usba = _along_v(usba, *thru) ^ box(-BIG, -BIG, Z_TOP - 0.4, BIG, BIG, BIG)
    usbc_z = Z_TOP + USBC_H / 2
    usbc = stadium(USBC_U, usbc_z, USBC_W + 0.6, USBC_H + 0.6, *thru)
    # the moulding around a USB-C plug is bigger than the hole; let it sink in
    usbc_relief = stadium(USBC_U, usbc_z, 12.8, 7.0, OV1 - 1.0, OV1 + 1)
    openings = usba + usbc + usbc_relief

    # ---- tray ----
    below = box(-BIG, -BIG, -1, BIG, BIG, Z_TOP)
    tray = shell ^ below

    lip2d = (rrect(U0 - LIP_T, V0 - LIP_T, U1 + LIP_T, V1 + LIP_T, R_IN + LIP_T)
             - cavity2d)
    lip2d = lip2d ^ CrossSection.square((BIG, BIG)).translate((-BIG / 2, LIP_END_V - BIG))
    tray += lip2d.extrude(LIP_H).translate((0, 0, Z_TOP))

    ridge_z = Z_TOP + LIP_H / 2
    ridge_vs = (10.0, 38.0)
    for v in ridge_vs:
        tray += ridge(U0 - LIP_T, -1, v, ridge_z, 0.6, RIDGE, RIDGE_LEN)
        tray += ridge(U1 + LIP_T, +1, v, ridge_z, 0.6, RIDGE, RIDGE_LEN)

    # ledges under the long edges, clear of J5's pins at u >= 1.8
    tray += box(U0, 0, FLOOR - 0.01, 1.2, BOARD_L, Z_BOT)
    tray += box(BOARD_W - 1.2, 0, FLOOR - 0.01, U1, BOARD_L, Z_BOT)
    # two posts mid-board where the underside is bare
    for u, v in ((18.0, 30.0), (28.0, 40.0)):
        tray += cyl(u, v, FLOOR - 0.01, Z_BOT, 3.0)
    # stops at the antenna-end corners, outside the module's width and no
    # higher than the board, so the module overhang passes over them
    for u0, u1 in ((U0, 7.5), (28.5, U1)):
        tray += box(u0, -1.2, FLOOR - 0.01, u1, 0.0 - 0.15, Z_TOP - 0.1)

    tray -= openings
    tray_wall = wall_holes(tray)

    # ---- cover ----
    cover = shell - below
    rebate2d = rrect(U0 - LIP_T - FIT, V0 - LIP_T - FIT, U1 + LIP_T + FIT, V1 + LIP_T + FIT,
                     R_IN + LIP_T + FIT)
    rebate2d = rebate2d ^ CrossSection.square((BIG, BIG)).translate((-BIG / 2, LIP_END_V + FIT - BIG))
    cover -= rebate2d.extrude(LIP_H + FIT).translate((0, 0, Z_TOP - 0.01))
    for v in ridge_vs:
        cover -= ridge(U0 - LIP_T - FIT, -1, v, ridge_z, 0.7, RIDGE + 0.1, RIDGE_LEN + 1)
        cover -= ridge(U1 + LIP_T + FIT, +1, v, ridge_z, 0.7, RIDGE + 0.1, RIDGE_LEN + 1)

    # posts that hold the board down on the ledges, on bare board
    for u, v in ((1.4, 10.5), (1.4, 50.0), (34.6, 16.0), (34.6, 40.0)):
        cover += cyl(u, v, Z_TOP, z_ceil + 0.01, 2.2)

    if oled:
        # The module sits over D3 and D4, so there are no light pipes: the
        # display shows the state instead.
        wu, wv, su, sv = OLED_WIN
        win = rrect(wu - su / 2, wv - sv / 2, wu + su / 2, wv + sv / 2, 1.0)
        cover -= win.extrude(h + 2).translate((0, 0, z_ceil - 1))
        # 45-degree bevel around the window on the outside
        bevel = Manifold.hull(win.offset(ROOF - 0.6, JoinType.Round).extrude(0.01)
                              .translate((0, 0, h - 0.01))
                              + win.extrude(0.01).translate((0, 0, z_ceil + 0.6)))
        cover -= bevel
        # pocket in the roof for the module's header pins
        cover -= box(OLED_J3_U - 1.6, OLED_J3_V - 6.0, z_ceil - 0.01,
                     OLED_J3_U + 1.6, OLED_J3_V + 6.0,
                     Z_TOP + OLED_TOP - 1.5 + OLED_PINS_UP + 0.3)
    else:
        # light pipe guides, each with a shoulder the rod rests on
        for u, v in LEDS:
            cover += cyl(u, v, BOSS_BOT, z_ceil + 0.01, LIGHTPIPE_BOSS)
        for u, v in LEDS:
            cover -= cyl(u, v, BOSS_BOT + LIGHTPIPE_SHOULDER, h + 1, LIGHTPIPE_HOLE)
            cover -= cyl(u, v, BOSS_BOT - 1, h + 1, LIGHTPIPE_APERTURE)
            cover -= Manifold.cylinder(0.6, LIGHTPIPE_HOLE / 2, LIGHTPIPE_HOLE / 2 + 0.6) \
                .translate((u, v, h - 0.6 + 0.001))                          # chamfer

    # RESET / BOOT pinholes, with a tube that leads a paperclip onto the switch
    for u, v in BUTTONS:
        cover += cyl(u, v, Z_TOP + BUTTON_H + 1.0, z_ceil + 0.01, PINHOLE + 2.0)
        cover -= cyl(u, v, Z_TOP, h + 1, PINHOLE)

    cover -= openings
    return tray, cover, tray_wall


def lightpipe():
    """One rod, standing with its visible end on the bed, where the face comes
    out flattest. Three ribs near that end give the friction fit. The far end
    is chamfered so it finds the hole."""
    r = LIGHTPIPE_D / 2
    c = 0.3
    rod = (Manifold.cylinder(LIGHTPIPE_LEN - c, r)
           + Manifold.cylinder(c, r, r - c).translate((0, 0, LIGHTPIPE_LEN - c)))
    rib_r = LIGHTPIPE_HOLE / 2 + LIGHTPIPE_RIB
    for a in (0, 120, 240):
        rib = Manifold.cube((rib_r - r + 0.2, 0.5, LIGHTPIPE_RIB_LEN)) \
            .translate((r - 0.2, -0.25, 0)).rotate((0, 0, a))
        rod += rib
    return rod


def board_frame_to_model(m):
    """Build frame (u, v, z) has v pointing the way KiCad's Y does, which is a
    mirror image. Flip v so the printed part is the right way round."""
    return m.mirror((0, 1, 0))


def print_upside_down(m, h):
    """Turn a cover over so its top face is on the bed.

    ROTATE, never mirror. Mirroring in z also flips the part's handedness. The
    first printed cover was made that way: USB-A and USB-C were on opposite
    sides from the tray's, and it showed only when the two parts were put
    together. check_assembly() below catches it."""
    return m.rotate((180, 0, 0)).translate((0, 0, h))


def check_assembly(name, printed, design, h):
    """Turn the printed cover back over the way a person does, by rotating it,
    and require that it lands exactly on the design. A mirrored cover passes
    every other check here and still has its ports on the wrong sides."""
    placed = printed.translate((0, 0, -h)).rotate((180, 0, 0))
    stray = (placed - design).volume() + (design - placed).volume()
    if stray > 1.0:     # mm3; a correct part differs only by float noise
        sys.exit(f"{name}: turned over, it does not match the design "
                 f"({stray:.0f} mm3 differ). Was it mirrored instead of rotated?")


def write_stl(m, path):
    mesh = m.to_mesh()
    v = np.asarray(mesh.vert_properties)[:, :3].astype(np.float32)
    t = np.asarray(mesh.tri_verts)
    tri = v[t]
    n = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
    n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-12)
    rec = np.zeros(len(t), dtype=[("n", "<3f4"), ("v", "<9f4"), ("a", "<u2")])
    rec["n"] = n
    rec["v"] = tri.reshape(-1, 9)
    with open(path, "wb") as f:
        f.write(b"ups-adaptor case".ljust(80, b" "))
        f.write(struct.pack("<I", len(t)))
        f.write(rec.tobytes())


def main():
    check_wall_mount()
    tray, cover, tray_wall = build()
    cover_oled = build(oled=True)[1]
    parts = (("tray", tray, None), ("tray-wall", tray_wall, None),
             ("cover", cover, H), ("cover-oled", cover_oled, H_OLED))
    os.makedirs(OUT, exist_ok=True)
    for name, m, h in parts:
        if m.status().name != "NoError" or m.genus() < 0:
            sys.exit(f"{name}: not a valid solid ({m.status()})")
        m = board_frame_to_model(m)
        if h is not None:
            design = m
            m = print_upside_down(m, h)
            check_assembly(name, m, design, h)
        p = os.path.join(OUT, f"ups-adaptor-{name}.stl")
        write_stl(m, p)
        (x0, y0, z0), (x1, y1, z1) = np.array(m.bounding_box()).reshape(2, 3)
        print(f"{os.path.relpath(p, ROOT)}  {x1 - x0:.1f} x {y1 - y0:.1f} x {z1 - z0:.1f} mm  "
              f"{m.volume() / 1000:.1f} cm3")
    pipes = Manifold.batch_boolean([lightpipe().translate((x, 0, 0)) for x in (0, 8)],
                                   OpType.Add)
    p = os.path.join(OUT, "ups-adaptor-lightpipes.stl")
    write_stl(pipes, p)
    print(f"{os.path.relpath(p, ROOT)}  two rods, {LIGHTPIPE_D:.0f} mm x {LIGHTPIPE_LEN:.1f} mm")
    print(f"closed case {OU1 - OU0:.1f} x {OV1 - OV0:.1f} x {H:.1f} mm, "
          f"{H_OLED:.1f} mm tall with the OLED cover")


if __name__ == "__main__":
    main()
