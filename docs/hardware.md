# My UPS Pal Rev A hardware

Rev A is a two-layer board, 36 x 58 mm, that sits between a USB-only UPS and a
Wi-Fi network. It carries an ESP32-S3-WROOM-1-N4 module, one USB-A host port
with switched and current-limited VBUS, one USB-C port for power, programming
and console, a header for a 0.96" I2C OLED, an expansion header and two status
LEDs. The design is published under CERN-OHL-S v2.

A second board, Rev B, uses the ESP32-S3-WROOM-1-N16R8 module and has more memory; its hardware design is not published. Everything below describes Rev A.

Companion pages: [`circuit.md`](circuit.md) is the one-page overview,
[`hardware-schematic.md`](hardware-schematic.md) lists every net and pin,
[`BOM.md`](BOM.md) the parts and LCSC codes, [`jlcpcb.md`](jlcpcb.md) the
manufacturing limits and the ordering checks. The schematic is
[`schematic.pdf`](schematic.pdf).

Out of scope for Rev A: onboard battery backup, a second UPS port, Ethernet and
wide-input DC.

## Block diagram

```
 +-------------------------------------------+
 USB-C ---5V----> | polyfuse --+--> AP2114H-3.3 --> 3V3 rail |
 (power, | | | |
 prog, | +--> AP2171W ---------|-------+ |
 console) | (EN, FLAG to GPIO) | |
 | | | | |
 +--D+/D- ---> | USBLC6 --> CH340C --> UART0 (43/44) | | |
 | | | | |
 | ESP32-S3-WROOM-1-N4 | | |
 | | | | | |
 | GPIO8/9 | | GPIO19/20 | | |
 | | | | | |
 | OLED header USBLC6 VBUS | |
 | (I2C) | | | |
 +-----------------------------|------------|--+
 v v
 USB-A receptacle --> UPS
```

## Power tree

USB-C VBUS enters through a 2 A hold polyfuse (F1, 3.5 A trip) onto the 5 V
rail. That rail feeds two loads:

1. **AP2114H-3.3** (U3), producing the 3.3 V rail for the module, the CH340C,
   the OLED header and the status LEDs.
2. **AP2171W** load switch (U4), whose output becomes VBUS on the USB-A host
   receptacle.

Worst-case 5 V draw is about 500 mA: the S3 peaks near 350 mA during Wi-Fi
transmit, the CH340C draws about 20 mA, the OLED about 20 mA when fitted, the
two LEDs about 10 mA together and the UPS interface under 100 mA. Any 1 A USB
charger is enough.

The regulator dissipates about 0.2 W average and runs 40 to 50 °C. Its tab
has a copper pour.

**The regulator is chosen for dropout.** USB may deliver as little as 4.75 V
at the port; a thin metre of cable at half an amp costs another 200 mV, and
the polyfuse and trace take their share, leaving about 4.5 V at the regulator.
The AP2114H drops 450 mV, which leaves about 760 mV of headroom. An AMS1117 in
the same package and pinout drops 1200 mV and would sit at the edge of
regulation, falling out of it on every Wi-Fi transmit burst; that presents as
random resets that look like a firmware bug. Do not substitute it. The
AP2114H's recommended input maximum is 6 V, which costs nothing here because
the AP2171W on the same rail is limited to 6.5 V absolute maximum.

**The load switch limits current at 1.5 A typical** (1.1 A minimum, 1.9 A
maximum over temperature), with no ILIM pin to set it. A UPS USB interface
draws far less, so this is a fault limit, not an operating one. The switch
also blocks reverse current, so no backfeed diode is fitted: a series Schottky
would cost 0.3 to 0.4 V of regulator headroom.

**The polyfuse is sized against the switch, not against normal draw.** A
shorted UPS makes the switch limit, and that current flows through the fuse on
top of the board's own 400 mA: about 1.9 A at the switch's typical limit. A
fuse that holds below that trips on a UPS fault and takes down the whole
bridge, which is what the switch is fitted to prevent. The 2 A hold covers the
typical case with hold derating in a warm closet in mind; at the switch's
worst-case limit the margin is gone on paper, but into a dead short the
AP2171W dissipates several watts in a SOT-23-5 and thermally cycles within
milliseconds, so the average fault current sits well below the limit, and a
polyfuse needs seconds above hold to trip. The fuse still trips near 4 A, well
inside what a dead short on the 5 V rail draws.

**C7, the 100 µF on the switched VBUS, is bracketed on both sides.** Below
about 90 µF, hot-plugging a UPS whose USB interface carries the 10 µF the
specification allows drags VBUS under the 4.4 V a host must hold, by charge
sharing alone: at 22 µF it collapses to 3.4 V. Above about 180 µF, charging
the capacitor through the switch's 0.6 ms controlled rise exceeds the switch's
own current limit, so it limits on its own output capacitor and flags a fault
at every power-on. C7 is the same 100 µF 1206 part as C2, so it costs no extra
BOM line.

## Pin assignment

| Signal | GPIO | Notes |
|---|---|---|
| USB host D- | 19 | Fixed by silicon. To USB-A via USBLC6. |
| USB host D+ | 20 | Fixed by silicon. To USB-A via USBLC6. |
| UART0 TX | 43 | To CH340C RXD |
| UART0 RX | 44 | To CH340C TXD |
| BOOT | 0 | Strapping pin. Tact switch to GND, 10k pull-up. |
| RESET | EN | Tact switch to GND, 10k pull-up, 1 µF to GND |
| I2C SDA (OLED) | 8 | 4.7k pull-up |
| I2C SCL (OLED) | 9 | 4.7k pull-up |
| Status LED (yellow) | 48 | Plain LED, 1k series, active high |
| Fault LED (red) | 47 | Plain LED, 1k series, active high |
| VBUS enable | 4 | AP2171W EN, active high, **100k pull-down to GND** |
| VBUS fault | 5 | AP2171W FLG, open drain, active low, **10k pull-up to 3V3** |
| Expansion | 10, 11, 12, 13 | J5 pins 4 to 7 |

GPIO26 through GPIO32 are consumed by the module's SPI flash and are not
brought out. GPIO3, GPIO45 and GPIO46 are strapping pins and are left
unconnected, so their internal pull-downs select 3.3 V `VDD_SPI` and normal
boot. GPIO0 is pulled up with no capacitor on it. `EN` carries Espressif's
recommended 10 kΩ and 1 µF. GPIO35 through GPIO38 are free on the N4 module,
which has no PSRAM, and are unconnected.

The VBUS enable GPIO is the reliability feature of this board. Some UPS USB
interfaces stop responding and need their interface power cycled. Firmware
drops EN, waits, and re-enumerates, instead of someone visiting the closet.
The 100k pull-down keeps the switch off, and the UPS unpowered, until firmware
decides otherwise, which also covers the power-up glitches Espressif warns of
on GPIO17 to GPIO20.

The status LED is yellow (591 nm), not green: a green LED's forward voltage is
about 3.3 V, the same as the rail driving it, so a green part lights on some
boards and not others.

## Expansion header J5

Male 7-pin 2.54 mm header on the left edge, so a case can expose it without
cutting near the antenna or the connectors.

| J5 pin | Signal |
|---|---|
| 1 | 3V3 |
| 2 | 5V |
| 3 | GND |
| 4 | GPIO10 |
| 5 | GPIO11 |
| 6 | GPIO12 |
| 7 | GPIO13 |

There is no separate 5 V input header. Pins 2 and 3 of J5 are 5 V and GND, so
an enclosure can be fed from here. Do not feed J5 while USB-C is plugged in:
the two 5 V sources are in parallel with no ORing diode, and one supply is
pushed into the other.

## OLED header J3

The OLED is optional. Firmware does not require it, and the web interface
shows the same values. The screen's useful job is showing the IP address;
mDNS (`ups-esp32-XXXX.local`) solves the same problem without it.

> ### Read this before you plug a module into J3.
>
> **J3's four pins, in order from pin 1 (the square pad), are 3V3, GND, SDA,
> SCL.**
>
> Most 0.96" SSD1306 modules print their own pins as `GND, VCC, SCL, SDA`.
> J3 is a straight 1:1 socket, so a module seated by its own labels puts 3V3
> on its ground pin and ground on its supply pin. That destroys the module.
>
> Read the four labels printed on your module, in the order they are printed,
> before you seat it:
>
> - **Module reads `GND, VCC, SCL, SDA`** (the common case): do not seat it
>   directly. Wire it with four jumpers: module VCC to J3 pin 1, module GND to
>   J3 pin 2, SCL to pin 3, SDA to pin 4. Only the power pair crosses.
> - **Module reads `VCC, GND, SCL, SDA`**: its power pins line up with J3's,
>   and only SDA and SCL are swapped. It plugs straight in.
> - **Anything else**: confirm the module's pinout from its datasheet or a
>   continuity check before wiring it.
>
> In every case, run the **`reva-oled`** firmware build
> (`sdkconfig.defaults.reva-oled`), which swaps SDA and SCL for the display
> in software to match. The plain Rev A build leaves the display off, on
> purpose: it cannot know which way round the module in hand is wired.

| J3 pin | Signal |
|---|---|
| 1 | 3V3 |
| 2 | GND |
| 3 | SDA |
| 4 | SCL |

Firmware drives an **SSD1306, 128x64, I2C**, at address **0x3C** and 400 kHz.
Two further traps:

- **1.3" modules are frequently SH1106, not SSD1306.** SH1106 has a 132-column
  RAM with the 128-pixel panel offset by 2, so an SSD1306 driver renders it
  shifted and wrapped. The firmware is SSD1306-only.
- **Some modules are strapped for 0x3D**, usually by a jumper or resistor on
  the back. Firmware probes 0x3C and logs a warning if nothing answers, so a
  wrong address looks like an absent display.

J3 is a female socket (`PinSocket_1x04_P2.54mm_Vertical`), because these
modules ship with male pins; the land pattern is the same as a male header.
J5 stays male, because jumper wires have female ends. J3 is placed so a 0.96"
module sits flat over the board, wholly within the outline and clear of the
antenna end. A module plugged into J3 covers both LEDs, so firmware draws the
LED state on the screen. Take the display off where Wi-Fi range matters: a
27 mm slab of PCB a few millimetres above a 2.4 GHz antenna never helps.

Both J3 and J5 are through-hole. JLCPCB Economic PCBA fits through-hole parts
by hand at a labour fee per order plus a few cents a joint; fit them yourself
or let them, but decide before ordering.

## Manufacturing and cost

JLCPCB Economic PCBA, every part on the top side (see
[`jlcpcb.md`](jlcpcb.md)). JLC charges a setup fee, a stencil fee, a fee of
about $3 per order for each distinct Extended-library part, and a small fee
per solder joint. Basic-library parts carry no per-part fee, which is why the
passives are Basic parts and the status LEDs are plain 0603 LEDs: an
addressable LED would be another Extended line, costing about $3 per order
for a $0.06 part. Rev A has nine Extended lines (see `BOM.md`), so the fixed
costs do not amortise below about ten boards. Ten is the sensible first order.

Parts come to roughly $4.70 a board without the OLED, and the module is well
over half of that. A 0.96" SSD1306 module adds about $2.

## Layout constraints

**Antenna keep-out is the dominant constraint.** The WROOM-1's PCB antenna
overhangs the board outline, with no copper on either layer beneath it, no
ground pour, and nothing metallic within roughly 15 mm. This drives the board
outline more than anything else, and getting it wrong produces a board that
works on a desk and badly inside a closet near metal. The module sits at
the antenna end of the board and everything else is laid out around it.

**USB routing.** Full speed at 12 Mbps needs no controlled impedance, but
D+/D- run as a tight pair, matched length, on the top layer, with continuous
ground beneath and no stubs. Each USBLC6 array sits close to its connector,
on the connector side of the pair, not the MCU side.

**Thermal.** The regulator's tab has a copper pour of at least 100 mm² with
vias to the bottom layer. The pour is on the output rail.

**Bulk capacitance.** The 100 µF bulk capacitor sits close to the module's
3V3 pin. Wi-Fi transmit bursts are why ESP32 designs brown out.

**Both USB connectors are on the same edge**, away from the antenna, so one
case wall carries both openings.

## Mechanical

**The board is 36 x 58 mm with no mounting holes.** It is light and has no
moving parts, so the enclosure holds it by its edges: the board drops into a
tray and the cover snaps on and holds it down. The printable case is in
`hardware/enclosure/`.

What that asks of any other enclosure:

- The outline is the mechanical interface. A printed case wants roughly
  0.3 mm of clearance per side, not a press fit.
- Both USB connectors are on one short edge; one wall carries both openings.
- The antenna end overhangs the board. Nothing metallic within about 15 mm,
  which includes screws, threaded inserts and metal clips. Keep that end of the
  case plastic and empty.
- The expansion header is on the left long edge.

**Why the board is this size.** The width is set by the connectors: the
right-angle USB-A courtyard is 15.5 x 15.3 mm and the USB-C is 10.6 x 9.4 mm,
so both on one edge need about 30 mm before margins. The module needs only
19 mm of board for its pads; the rest of its 33.5 mm courtyard is antenna
keep-out, which is open air. The length is set by routing: a 0.25 mm track
needs its own width plus clearance on either side to pass between two parts,
and at a routable 1.2 mm courtyard spacing the parts need 58 mm.

**The USB-A receptacle** is the XKB U231-091N-4BLRA00-S, a USB 3.0 9-contact
Type-A and the smallest surface-mount USB-A footprint in the KiCad library
(15.5 x 15.3 mm courtyard). Footprint size is set by the plug cavity, which is
identical for USB 2.0 and 3.0, so the extra contacts cost nothing: pins 1 to 4
are the standard USB 2.0 contacts, wired VBUS / D- / D+ / GND, and the five
SuperSpeed pads are unconnected. The board runs full speed at 12 Mbps. A
vertical through-hole receptacle would halve the footprint area, at the cost
of the UPS cable pointing up off the board face instead of out the edge.

## Opening and editing the design

The design is a KiCad project in `hardware/kicad/`:

| File | What it is |
|---|---|
| `ups-adaptor.kicad_pro` | project: design rules, net classes |
| `ups-adaptor.kicad_sch` | schematic |
| `ups-adaptor.kicad_pcb` | board |

The board file is saved by **KiCad 10**; the schematic was last saved by
KiCad 9 and opens in 10. Open the project in KiCad 10 or later. Only the
standard KiCad symbol, footprint and 3D libraries are used, so nothing else
needs installing.

Outputs, all committed so an order can be placed without KiCad:

| Path | What it is for |
|---|---|
| `hardware/fab/ups-adaptor-gerbers.zip` | the PCB upload: copper, mask, paste and silkscreen for both sides, edge cuts, the job file and the two drill files |
| `hardware/fab/ups-adaptor-jlcpcb-order.zip` | the gerber zip, `bom.csv`, `cpl.csv` and a `README.txt` with the order settings, in one download |
| `hardware/fab/SOURCE.sha256` | SHA-256 of the `.kicad_pcb` the outputs were plotted from, so a stale output can be detected |
| `hardware/bom/bom.csv` | the assembly BOM upload, every line pinned to an LCSC code |
| `hardware/bom/cpl.csv` | the placement upload, pad centroids and checked rotations |
| `hardware/enclosure/` | the printable case: tray, wall-mount tray, cover, light pipes, three-colour cover project |

To regenerate the gerbers after a change, plot from the PCB editor (File,
Plot) with only the manufacturing layers selected: F.Cu, B.Cu, F.Mask, B.Mask,
F.Paste, B.Paste, F.Silkscreen, B.Silkscreen and Edge.Cuts, then generate the
drill files (PTH and NPTH, Excellon). The same from the command line:

```sh
kicad-cli pcb export gerbers --layers F.Cu,B.Cu,F.Mask,B.Mask,F.Paste,B.Paste,F.Silkscreen,B.Silkscreen,Edge.Cuts \
    -o gerber/ hardware/kicad/ups-adaptor.kicad_pcb
kicad-cli pcb export drill -o gerber/ hardware/kicad/ups-adaptor.kicad_pcb
```

Plotting with no layer list also emits the courtyard, fab, adhesive, margin
and user layers, which are documentation; a fab importer either ignores them
or offers to treat one as a real layer. Leave them out.

A position file from `kicad-cli pcb export pos` (or File, Fabrication Outputs,
Component Placement) needs two corrections before it becomes `cpl.csv`, and
[`jlcpcb.md`](jlcpcb.md) gives them: KiCad writes each footprint's anchor, not
its pad centroid, and three parts differ by millimetres between the two; and
JLC's assembly preview, not KiCad's angle, decides seven rotations. Check the
BOM against [`BOM.md`](BOM.md): every line carries an LCSC code, and the
footprint sizes agree with the codes.
