# UPS to ESP32 Module Rev A hardware specification

## Scope

Rev A is a single two-layer board, roughly 36 x 58mm, that does exactly one job: sit
between a USB-only UPS and a Wi-Fi network. It carries an ESP32-S3 module, one USB-A host
port with switched and current-limited VBUS, a single USB-C for power and programming, a
header for a 0.96" I2C OLED, and a single RGB status LED.

Deliberately out of scope for Rev A: onboard battery backup, a second UPS port, Ethernet,
wide-input DC, and an enclosure. Each was considered and deferred.

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
 | ESP32-S3-WROOM-1-N8 | | |
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

**The polyfuse is 2A hold, and the number is set by the load switch, not by
normal draw.** Normal worst case on the 5V rail is only about 500mA. What sizes
the fuse is the fault case: the AP2171W limits at a fixed 1A, and that 1A passes
through the fuse on top of the board's own 400mA, so a shorted UPS puts 1.4A
through it.

A fuse that holds below 1.4A trips on a UPS fault and takes down the whole
bridge — precisely what the load switch was fitted to prevent. 1.5A would give
only 7% margin, and hold derates with temperature in a warm closet. 2A gives 43%,
and still trips near 4A, well inside what a dead short on the 5V rail draws.

`tools/test/test_power.py` asserts that relationship directly, so changing either
the switch or the fuse without checking the other fails the harness.

USB-C VBUS enters through a 2A-hold polyfuse onto the 5V rail. That rail feeds two loads:

1. **AP2114H-3.3** producing the 3.3V logic rail for the S3 module, CH340C, OLED and
 plain status LEDs.
2. **AP2171W** load switch, whose output becomes VBUS on the USB-A host receptacle. Its
 output carries **100µF** (C7), which is bracketed on both sides and not a
 round number picked for comfort. Below about 90µF, hot-plugging a UPS whose
 USB interface has the 10µF the spec allows drags VBUS under the 4.4V a host
 must hold, purely by charge sharing — at the 22µF this board used to carry it
 collapses to 3.4V. Above about 180µF, charging that cap through the AP2171's
 0.6ms controlled rise exceeds the switch's own 1.5A limit, so it current-limits
 on its own output capacitor and flags a fault at every power-on. C7 is the
 same 100µF 1206 part as C2, so it costs no extra BOM line.
 `tools/test/test_power.py` asserts both bounds.

Worst-case 5V draw is roughly 500mA: the S3 peaks near 350mA during Wi-Fi transmit, the
CH340C draws about 20mA, the OLED about 20mA when fitted, the two status LEDs about 10mA
together, and the UPS interface under 100mA. Any 1A USB charger is ample.

The regulator dissipates about 0.2W average and will run 40 to 50°C. Acceptable for a
closet device. Give it a copper pour.

**Why not the AMS1117.** It was the AMS1117-3.3 until. Same package, same
pinout, but 1200mV of dropout against the AP2114H's 450mV — and that is the difference
between working and not. USB may legally deliver 4.75V at the port; a thin metre of cable
at half an amp costs another 200mV; the polyfuse and trace take their share. That leaves
about 4.51V at the regulator, against the 4.50V an AMS1117 needs to hold 3.3V. Ten
millivolts. The ESP32-S3 draws 350mA bursts every time it transmits, and an LDO falling
out of regulation on those bursts is the most reported failure of AMS1117-powered ESP32
boards — it presents as random resets that look like a firmware bug. The AP2114H leaves
760mV on the same supply, is explicitly stable with ceramic output capacitors, and costs
$0.06 against $0.22. `tools/test/test_power.py` now checks this headroom.

Its one trade is input range: 6.0V recommended maximum against the AMS1117's 15V. That
costs nothing here, because the AP2171W load switch on the same 5V rail is already
limited to 6.5V.

The AP2171W's current limit is fixed at 1A — there is no ILIM resistor to set. A UPS USB
interface draws far less, so this is a fault limit rather than an operating one.

## Pin assignment

| Signal | GPIO | Notes |
|---|---|---|
| USB host D- | 19 | Fixed by silicon. To USB-A via USBLC6. |
| USB host D+ | 20 | Fixed by silicon. To USB-A via USBLC6. |
| UART0 TX | 43 | To CH340C RXD |
| UART0 RX | 44 | To CH340C TXD |
| BOOT | 0 | Strapping pin. Tact switch to GND, 10k pull-up. |
| RESET | EN | Tact switch to GND, 10k pull-up, 1µF to GND |
| I2C SDA (OLED) | 8 | 4.7k pull-up |
| I2C SCL (OLED) | 9 | 4.7k pull-up |
| Status LED (green) | 48 | Plain LED, 1k series, active high |
| Fault LED (red) | 47 | Plain LED, 1k series, active high |
| VBUS enable | 4 | AP2171W EN, active high, **100k pull-down to GND** |
| VBUS fault | 5 | AP2171W FLG, open drain, **10k pull-up to 3V3** |

Avoid GPIO26 through GPIO32, which the module's SPI flash consumes. GPIO3, GPIO45 and
GPIO46 are strapping pins and are left unconnected. GPIO33 through GPIO37 are free on the
N8 part because there is no octal PSRAM, and are brought out to a small expansion pad
field for future use.

The VBUS enable GPIO is the reliability feature of this board. CyberPower USB interfaces
occasionally stop responding and need their interface power cycled. Firmware should drop
EN for 2 seconds and re-enumerate rather than requiring someone to visit the closet.

## Bill of materials

Per board. LCSC codes marked verified were checked against JLCPCB's live library.

| Part | Package | LCSC | Qty | Unit | Ext | Verified |
|---|---|---|---|---|---|---|
| ESP32-S3-WROOM-1-N4 | module | C2913197 | 1 | $2.93 | $2.93 | **checked ** |
| CH340C USB-UART | SOP-16 | C84681 | 1 | $0.50 | $0.50 | **checked ** |
| AP2171WG-7 load switch (active-high EN) | SOT-23-5 | C110466 | 1 | $0.09 | $0.09 | **checked ** |
| USBLC6-2SC6 ESD array | SOT-23-6 | C7519 | 2 | $0.18 | $0.35 | **checked ** |
| AP2114H-3.3 LDO (450mV dropout) | SOT-223 | C150716 | 1 | $0.06 | $0.06 | **checked ** |
| USB-C receptacle 16P TYPE-C-31-M-12 | SMD | C165948 | 1 | $0.10 | $0.10 | **checked ** |
| USB-A receptacle, XKB U231-091N-4BLRA00-S | SMD | C2880618 | 1 | $0.14 | $0.14 | **checked ** |
| LED green + red | 0603 | basic lib | 2 | $0.01 | $0.02 | n/a |
| MMBT3904 NPN (auto-reset) | SOT-23 | C20526 | 2 | $0.01 | $0.02 | confirm |
| Tact switch, BOOT and RESET | SMD 3x4 | basic lib | 2 | $0.03 | $0.06 | n/a |
| Polyfuse 2A hold | 1206 | basic lib | 1 | $0.05 | $0.05 | n/a |
| Pin headers: 4-pin OLED, 7-pin expansion | 2.54 THT | basic lib | 2 | $0.03 | $0.06 | n/a |
| Caps: 100µF x2, 22µF x2, 1µF, 100nF x9 | mixed | basic lib | 15 | | $0.12 | n/a |
| Resistors: 5.1k x2, 10k x3, 4.7k x2, 1k x4, 100k | 0402 | basic lib | 12 | | $0.02 | n/a |
| **Board subtotal** | | | | | **~$6.00** | |
| 0.96" SSD1306 OLED module (**optional**) | | third party | 1 | $2.00 | $2.00 | |
| **Total parts per board, with OLED** | | | | | **$6.43** | |

**The OLED is an option, not a fitted part.** At $2.00 it is 31% of the BOM and
the most expensive single line item, more than the ESP32 module, so it is not
ordered by default and firmware does not require it.

What Rev A *does* carry is the $0.03 4-pin header and the footprint, always
populated -- **but it cannot be used as intended.** J3 reads 3V3, GND, SDA, SCL
from pin 1, and standard modules read GND, VCC, SCL, SDA, so a module seated in
it is powered backwards. The first Rev A board did exactly that and killed the
module. Release firmware therefore drives no display on Rev A. The one safe way
is a module labelled VCC, GND, SCL, SDA with the bench-only firmware option
`UPSA_REVA_OLED_VCC_FIRST` -- see the warning in the README. The header order is
fixed for Rev B, whose target will enable it. Boards shipped without the module cost
**$4.43 in parts** and lose nothing a user sees, because the web interface shows
the same values.

Note on assembly: the 2.54mm header is through-hole. JLCPCB Economic PCBA does
assemble through-hole parts, but by hand, at $3.50 labour plus $0.0173 a joint —
so four joints costs cents on top of a fixed fee you pay once per order. Fit it
yourself or let them, but decide before ordering.

The screen's genuinely useful job is **showing the IP address**, which is how you
reach the web UI without hunting through a DHCP table. mDNS (`ups-esp32-XXXX.local`)
solves the same problem in firmware for free, so treat the two as alternatives
rather than both being necessary.

Do not hardcode LCSC codes for passives. Let the KiCad BOM export select current JLC
Basic library parts at order time. Basic parts are free to place; extended parts are not.

The two 5.1k resistors are the USB-C CC1 and CC2 pull-downs that make the connector a
sink. They are not optional.

**Prices above were inherited estimates and were wrong.** See
[`BOM.md`](BOM.md) for figures checked against live LCSC pages, the N8-to-N4
cost lever. The polyfuse/load-switch interaction it flagged is fixed below.

## Manufacturing and cost

JLCPCB Economic PCBA. Current fee structure: $8.18 setup, $1.53 stencil, $3.07 per unique
**extended** component charged once per order, and $0.0016 per solder joint.

This design has five extended parts: the ESP32 module, CH340C, AP2171W and both USB
connectors. That is $15.35, so fixed costs land near $25 per order regardless of quantity.
Joint count is approximately 165 per board.

The status LED is a plain 0603 pair rather than a WS2812B for exactly this reason. At
$3.07 per unique extended part per order, a $0.06 addressable LED really costs $3.13 on a
small run, while Basic-library LEDs are free to place. Two LEDs also drop the RMT driver
from the firmware, and the OLED and web UI already carry any status detail worth reading.

| | Qty 5 | Qty 10 | Qty 20 |
|---|---|---|---|
| PCB, 55x40mm two-layer | $2 | $5 | $10 |
| Assembly fixed costs | $25 | $25 | $25 |
| Components | $22 | $45 | $85 |
| Joints | $1 | $3 | $5 |
| Shipping to Canada (DHL) | $22 | $22 | $25 |
| **Total** | **$72** | **$100** | **$150** |
| **Per board, no OLED** | **~$14** | **~$10** | **~$7.50** |
| **Per board, plus a $2 OLED each** | **~$16** | **~$12** | **~$9.50** |

GST and possible brokerage apply on import to Canada.

The fixed costs do not amortise below about ten boards. At qty 5 this board is not cheaper
than five dev boards with soldered pigtails, it is simply better. Qty 10 is the sensible
first order.

## Layout constraints

**Antenna keepout is the dominant constraint.** The WROOM-1's PCB antenna must overhang
the board outline, with no copper on either layer beneath it and no ground pour, and
nothing metallic within roughly 15mm. This drives the board outline more than anything
else, and it is the mistake that produces a board working fine on the bench and badly
inside a closet near metal. Place the module at a board edge first, then lay out
everything else around it.

**USB routing.** Full speed at 12 Mbps does not require controlled impedance, but route
D+/D- as a tight pair, matched length, on the top layer, with continuous ground beneath
and no stubs. Place the USBLC6 arrays close to their respective connectors, on the
connector side of the pair, not the MCU side.

**Thermal.** Give the regulator's tab a copper pour of at least 100mm² with a few vias to
the bottom layer.

**Bulk capacitance.** Put the 100µF bulk cap physically close to the module's 3V3 pin.
The S3's Wi-Fi transmit bursts are the reason ESP32 designs brown out.

**Mechanical.** Four M3 mounting holes. Position the OLED header so a standard 0.96"
module sits flat over the board rather than hanging off an edge. Dimension the outline to
suit a 3D printed enclosure.

## OLED module compatibility

Firmware drives an **SSD1306, 128x64, I2C**, at address **0x3C** and 400 kHz
(`components/display/display.c`). J3 brings the bus out in this order:

| J3 pin | Signal |
|---|---|
| 1 | 3V3 |
| 2 | GND |
| 3 | SDA |
| 4 | SCL |

**Check your module's pin order before plugging it in.** Cheap 0.96" modules
ship in at least two arrangements — `GND, VCC, SCL, SDA` and
`VCC, GND, SCL, SDA` are both common — and neither matches this header exactly.
Swapped SDA and SCL simply will not work; **reversed power usually destroys the
module**. Every J3 pin is now named on the silkscreen so the order is readable
on the board rather than only in this file.

Two further traps:

- **1.3" modules are frequently SH1106, not SSD1306.** The controllers are
 similar but not identical: SH1106 has a 132-column RAM with the 128-pixel
 panel offset by 2, so an SSD1306 driver renders it shifted and wrapped. The
 firmware here is SSD1306-only.
- **Some modules are strapped for 0x3D**, usually by a jumper or a resistor on
 the back. `display.c` probes 0x3C and logs a warning rather than hanging if
 nothing answers, so a wrong address looks like an absent display.

If a particular module is chosen, J3's order can be changed to match it so the
module plugs straight in — the header is a generated part of the schematic and
costs nothing to re-order before fab.

### J3 is a female socket, and sits low on purpose

These modules ship with **male pins**, pre-soldered or loose in the bag, so the
board carries a **socket**: `PinSocket_1x04_P2.54mm_Vertical`. The land pattern
is identical to a male header, so this cost nothing. J5 stays a male header,
because jumper wires have female ends.

The module is nearly as large as the board — roughly 27 × 28 mm against 36 × 46 —
and extends perpendicular to the socket, centred on it. **With J3 at its original
y=11.8 the module overhung the top edge by 2.1 mm**, putting a copper-backed PCB
inside the antenna's exclusion zone. J3 is therefore anchored at y=24, where the
module lands wholly on the board and stops about 10 mm short of that edge.

Even so: take the display off before the board goes where Wi-Fi range matters. A
27 mm slab of PCB 8.5 mm above a 2.4 GHz antenna is never an improvement.

## No separate 5V input header

An earlier revision carried a 2-pin header in parallel with USB-C VBUS, so the
board could be fed from a screw terminal inside an enclosure. It has been
removed.

The capability was never lost: **J5 pins 2 and 3 are 5V and GND**, so an
enclosure can be wired from the expansion header instead. What the extra part
added was 22 mm² on a board whose size is already set by its connectors, and a
way to damage a charger — two 5V sources in parallel with no ORing diode means
powering the header while USB-C is plugged in pushes one supply into the other.

## Mechanical

**Board is 36 x 58 mm, retained by the enclosure, with no mounting holes.**

Two M3 holes need roughly 6 mm each once annulus and clearance are counted, and
on a board whose width is already set by the connectors that is several
millimetres bought for nothing. This board is light and has no moving parts, so
the enclosure holds it by its edges instead -- a slot or clip on each long side,
or a lid that presses it onto short standoffs.

What that asks of the enclosure design:

- **The outline is the mechanical interface.** Keep the board rectangular and
 hold the fab tolerance in mind; a printed case wants roughly 0.3 mm of slop per
 side, not a press fit.
- **Both USB connectors are on the same edge**, so one wall carries both
 openings. That was deliberate: it keeps them away from the antenna and means
 only one face needs cutouts.
- **The antenna end overhangs the board.** Nothing metallic within ~15 mm of it,
 which includes screws, threaded inserts and any metal clip. Keep that end of
 the case plastic and empty.
- **The expansion header sits on the left edge** so a case can expose it without
 cutting near the antenna or the connectors.

If clips prove unreliable in practice, two M3 holes cost about 4 mm of width.
That is the fallback, not the starting point.

### Why the board is this size

The width is set by the connectors, not the module. The right-angle USB-A
courtyard is 15.5 mm wide by 15.3 mm deep and USB-C is 10.6 by 9.4 mm, so both on
one edge need about 30 mm before margins. The module needs only 19 mm of board for its pads -- the rest of
its 33.5 mm courtyard is antenna keepout, which is open air.

**The length is set by routing, not by parts.** An earlier 36 x 46 version fitted
every component and could not be routed: placement used 0.3 mm of clearance
between courtyards, which is enough for parts not to touch and nothing else. A
0.25 mm track needs its own width plus clearance either side to pass between two
parts, so there were no channels at all and the autorouter was threading around
a solid block. At a routing-friendly 1.2 mm the same parts do not fit on 46 mm --
the placer fails on U3 at both 46 and 50 mm. 58 mm places everything at 61%
top-side utilisation, and the nets that would not route before now route.

At 2088 mm² this is still marginally smaller than the 55 x 40 = 2200 mm² of the
original specification. The dramatic shrink was never available; it was an
artifact of packing the parts too tightly to wire them together.

**The USB-A is the smallest surface-mount one available, and that is settled.**
The XKB part (`C2880618`) is a USB **3.0** 9-contact Type-A. That is deliberate,
or at least it is now: pins 1-4 are the standard USB 2.0 contacts in the standard
positions, wired here as VBUS / D- / D+ / GND, and the five SuperSpeed pads are
left unconnected. A USB 3.0 A receptacle is backward compatible by design and
this board runs full speed at 12 Mbps.

Every USB-A footprint in the KiCad library was measured by courtyard:

| Footprint | Courtyard | Area | Mount |
|---|---|---|---|
| Molex 105057 **vertical** | 16.04 x 7.69 | **123 mm2** | THT |
| Wuerth 614004134726 | 20.68 x 8.60 | 178 mm2 | THT |
| **XKB U231-091N (fitted)** | 15.50 x 15.30 | **237 mm2** | **SMD** |
| GCT USB1046 | 15.50 x 16.48 | 255 mm2 | SMD |
| TE 292303-7 | 17.16 x 16.46 | 282 mm2 | SMD |
| Connfly DS1098 | 22.20 x 14.10 | 313 mm2 | SMD |
| Molex 48037-2200 | 22.36 x 14.00 | 313 mm2 | SMD |
| CNCTech 1001-011-01101 | 22.80 x 14.30 | 326 mm2 | SMD |

**The XKB is the smallest SMD option.** Every USB 2.0 surface-mount alternative
is 8% to 38% larger. An earlier note here reasoned that the 15.3 mm depth was a
USB 3.0 penalty and that a 2.0 part would shrink the board. That was wrong:
footprint size is set by the USB-A plug cavity, which is identical for 2.0 and
3.0 -- the extra contacts sit deeper inside the same shell. Nine contacts cost
nothing here.

On price, Jing Extension `903-242B2023S10210` (`C2763376`) is USB 2.0, SMD,
right-angle, 1,760 in stock at **$0.115** against the XKB's $0.14. It saves
**$0.025 a board**, 0.5% of the BOM, and has no KiCad footprint -- so taking it
means hand-drawing the land pattern for the one part where a footprint error is
a dead board. Not worth it. Molex 48037-2200 has a library footprint and stock
but costs $0.749, 6.5x, which fails the low-cost rule outright.

**The only real size lever left is going vertical** -- Molex 105057 at 123 mm2,
nearly half the area. It is through-hole, and vertical means the UPS cable points
up off the board face instead of out the edge, which is an enclosure decision
rather than a layout one. Not taken.

Economic PCBA does fit through-hole parts, by hand, at $3.50 labour plus $0.0173
a joint -- so a through-hole receptacle would be assembled at extra cost, not
refused. (An earlier note here said Economic PCBA was surface-mount only. That
was wrong: JLC's assembly capabilities page lists THT under both Economic and
Standard.)

## Open questions for Rev A

All resolved in [`hardware-schematic.md`](hardware-schematic.md) except one,
which is a genuine design conflict and needs a decision:

**Resolved: the load switch is an AP2171W, and it keeps `FLAG`, not `ILIM`.**
The SY6280AAC this project started with had no KiCad symbol and an unverified
pinout, which is not something to invent for a part that switches power. The
AP2171W has a verified symbol, a fixed 1 A limit instead of an adjustable one,
and an open-drain `FLG`. So `ILIM` is what was lost, not `FLAG` — an earlier
version of this note said the opposite and was wrong.

GPIO5 therefore carries `VBUS_FAULT`, not an expansion pad, and firmware builds
with `BOARD_HAS_VBUS_FAULT 1`. `board_vbus_fault` reads it active low, which
matches the datasheet: the flag is an open drain pulled low on over-current or
over-temperature, with a 7 ms deglitch, and R12 pulls it up to 3V3.

**The part is AP21*7*1W, not AP21*6*1W, and the digit is the whole feature.**
Both are the same package with the same pinout; the only difference is enable
polarity. AP2161 is active LOW. With R11 pulling `EN` to ground, an AP2161 would
sit switched ON from power-up, and `board_vbus_set(true)` driving the pin high
to turn VBUS on would turn it OFF — the UPS would never enumerate, and the
power-cycle recovery feature would invert. AP2171 is active HIGH, which is what
R11's pull-down and the firmware both already assume. `tools/test/test_power.py`
now asserts this rather than leaving it to a BOM line.

Resolved:

| Question | Decision |
|---|---|
| USB-A vertical or right-angle | Right-angle, for a flat wall-mount enclosure |
| OLED pull-ups with no module | Safe; one board build covers both cases |
| 2-pin 5V input header | **Dropped.** J5 pins 2 and 3 already carry 5V and GND, and a bare header paralleling USB-C VBUS with no ORing diode lets one 5V supply be pushed into another |
| Backfeed Schottky | Omitted; costs 0.3–0.4V of LDO headroom for no clear gain |
| USB-C / USB-A LCSC codes | Candidates listed; must be confirmed against live JLC stock at order time |
| ~~SY6280 ILIM value~~ | Moot: the AP2171W's limit is fixed at 1A |

## Deferred to a possible Rev B

- Onboard LiPo, charger and boost, so the bridge does not consume a UPS outlet.
- A second USB-A host port behind a CH334 hub IC, for two UPSes on one bridge.
- Ethernet via an RMII PHY, for people who do not want another Wi-Fi client.
- **Move D3 and D4 out from under the OLED.** On Rev A, a display plugged into J3
 sits directly over both LEDs, so a board with the screen fitted has no
 visible LEDs, even through the case's light pipes. The firmware covers this
 by drawing the LED state on the screen (`docs/firmware.md`, *Status
 indication*), but the LEDs keep working when the display fails, comes loose or
 wedges its I2C bus, and then nothing on the board shows the state.
 Move them to the strip along the right-hand long edge, roughly u 33.5-35.5 mm
 and v 20-45 mm in board coordinates (X 133.5-135.5, Y 80-105 in the Rev A
 layout). No part's courtyard reaches past X 134.7 there, and the module's
 edge stops at about X 132.7. That puts the light pipes beside the display
 instead of under it, so one cover serves boards with and without a screen.
 Cost: rerouting the GPIO47/48 LED nets, and moving the two hold-down posts
 `tools/gen-case.py` puts on that edge.
