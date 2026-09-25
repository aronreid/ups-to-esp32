# Bill of materials

Prices are LCSC's at quantity 10, checked against a live product page for
every line. **They move** -- treat them as an order of magnitude, not a quote.

## Verified

| Ref | Part | LCSC | Qty 10 | Stock | Ext |
|---|---|---|---|---|---|
| U1 | ESP32-S3-WROOM-1-**N4** | `C2913197` | **$2.93** | 5,063 | $2.93 |
| U2 | CH340C | `C84681` | $0.50 | 44,411 | $0.50 |
| U3 | **AP2114H**-3.3 SOT-223 (450 mV dropout) | `C150716` | $0.062 | 22,365 | $0.06 |
| U4 | AP2171W**G-7** SOT-23-5 (active-high EN) | `C110466` | $0.092 | 31,475 | $0.09 |
| D1, D2 | USBLC6-2SC6 SOT-23-6 | `C7519` | $0.176 | 31,255 | $0.35 |
| J1 | TYPE-C-31-M-12, 16P | `C165948` | $0.097 | 202,380 | $0.10 |
| J2 | XKB U231-091N-4BLRA00-S USB-A (USB 3.0 9P, used as 2.0) | `C2880618` | $0.14 | 2,420 | $0.14 |
| Q1, Q2 | MMBT3904 SOT-23 | `C20526` | ~$0.01 | — | $0.02 |
| SW1, SW2 | XKB TS-1187A-B-A-B tact switch | `C318884` | Basic | — | ~$0.04 |
| D3 | KENTO KT-0603**Y** yellow | `C2287` | Basic | 91,450 | ~$0.01 |
| D4 | KENTO KT-0603R red | `C2286` | Basic | — | ~$0.01 |
| F1 | PTTC SMD1206P200TF, 2 A hold / 3.5 A trip | `C545216` | $0.038 | — | $0.04 |

### Passives

Pinned deliberately, because JLC's matcher resolved every resistor line to an
01005. Each is JLC Basic, stocked in the millions, and **imperial** 0402 —
metric 1005, which is what `R_0402_1005Metric` means and is twice the length of
the metric-0402 parts the matcher chose.

| Ref | Value | LCSC | Part | Size |
|---|---|---|---|---|
| R1, R2, R7, R8 | 1 k ±1% | `C11702` | 0402WGF1001TCE | 0402 imperial |
| R3, R4, R12 | 10 k ±1% | `C25744` | 0402WGF1002TCE | 0402 imperial |
| R5, R6 | 4.7 k ±1% | `C25900` | 0402WGF4701TCE | 0402 imperial |
| R9, R10 | 5.1 k ±1% | `C25905` | 0402WGF5101TCE | 0402 imperial |
| R11 | 100 k ±1% | `C25741` | 0402WGF1003TCE | 0402 imperial |
| C3, C6, C9, **C8** | 100 nF 16 V X7R | `C1525` | CL05B104KO5NNNC | 0402 imperial |
| C1 | 1 µF **25 V** X5R | `C52923` | CL05A105KA5NQNC | 0402 imperial |
| C4, **C5** | 22 µF **25 V** X5R | `C45783` | CL21A226MAQNNNE | 0805 |
| C2, C7 | 100 µF 6.3 V X5R | `C15008` | CL31A107MQHNNNE | 1206 |

Three lines are deliberately one part serving two specifications, which is one
fewer unique part on the order:

- **C8** is specified 100 nF 25 V but sits on VBUS_A, a 5 V rail. A 16 V X7R is
 3.2× margin, so it shares C3/C6/C9's part.
- **C5** is specified 22 µF 6.3 V on +3V3 and **C4** 22 µF 16 V on +5V. One
 25 V part in the same 0805 covers both — and holds *more* capacitance under
 DC bias than either, so the derating in `test_power.py` is now pessimistic
 rather than optimistic.
- **C1** is specified 16 V; the Basic 0402 1 µF is 25 V.

## Two parts where the suffix is the whole specification

`U3` and `U4` each have a sibling that shares the package, the pinout drawing and
most of the part number, and would destroy the board:

| Order | Never | What the wrong one does |
|---|---|---|
| **AP2114H**-3.3TRG1 `C150716` | AP2114**HA**-3.3TRG1 `C460314` | HA is pin 1 VIN, pin 2 GND — puts the input rail on ground |
| **AP217**1W `C110466` | AP216**1**W `C176957` | AP2161 has an active-LOW enable — the UPS never powers up |

Both are stocked at LCSC, so a search by package or description reaches the wrong
one. `tools/test/test_bom.py` pins both LCSC codes and fails if either changes.

## Capacitor voltage ratings

Every capacitor value now carries its rating, because leaving it off hands the
choice to JLC's matcher and on a 5 V rail the cheapest 1206 it reaches for is
6.3 V. Two separate things depend on this:

**Reliability.** A ceramic on a 5 V rail wants 16 V or better — USB VBUS is
5 V +5%, and an unplugged cable's inductance rings on top of that.

**Capacitance.** Class-II ceramics lose capacitance under DC bias, worst near
their rating. A 100 µF 6.3 V X5R is about 48 µF at 3.3 V and nearer 38 µF at
5 V. Every figure on the schematic is a nominal; `tools/test/test_power.py`
derates before judging anything.

**C7 is the one compromise.** It is the USB host port's bulk, where effective
capacitance is what matters, and JLC's Basic library has no 1206 above 6.3 V at
this value — a 25 V part in the same package is 22 µF, which after derating is
*less* capacitance than the 6.3 V part. So C7 runs at 79% of its rating, giving
about 38 µF against the 120 µF USB nominally asks of a host port.

That is accepted rather than overlooked. The inrush a device draws on insertion
is bounded by the load switch's 1.5 A limit, not by this capacitor, and nothing
is enumerated at the moment of insertion for a droop to disturb. If a UPS ever
fails to enumerate on plug-in, a second 100 µF in parallel is the first thing to
try — there is room beside C7.

## Not yet verified

The switches, LEDs and fuse used to be listed here with no part numbers. That
was worse than it looks: `RESET`, `green` and `2A hold` are not values JLC's
matcher can resolve, so those lines would have been silently dropped from the
assembly order, and the buttons sat on an E-Switch land pattern that nothing
orderable had been checked against. They now have real parts, above, and the
buttons use KiCad's own footprint for the XKB switch.

**The upload files are `hardware/bom/bom.csv` and `hardware/bom/cpl.csv`**,
regenerated by every build from the schematic's own table. The hand-exported
BOM that used to sit there went stale and still named the AP2161W, the N8
module and a 500 mA fuse when it was finally read.

| Ref | Part | Note |
|---|---|---|
| J3, J5 | 2.54 mm socket and header | Through-hole. Economic PCBA does fit THT, but by hand: $3.50 labour plus $0.0173 a joint. 11 joints, so a few dollars on the order or solder them yourself |
| 16 × passives | 0402/0805/1206 R and C | Basic, roughly $0.15 the set |

## What this costs, honestly

Board subtotal came to roughly $6.00 on the N8, against the $4.43 previously recorded;
on the N4 it is roughly **$4.70**.
Almost all of the difference is the module: **$4.23, not $2.90**. At that price
**U1 is about 70% of the bill of materials** and everything else is noise.

The old figures were inherited estimates and had never been checked against a
live page. They are now.

## The N8-to-N4 change (taken )

| Module | LCSC | Qty 10 | Stock |
|---|---|---|---|
| ESP32-S3-WROOM-1-**N8** (8 MB) | `C2913198` | $4.23 | 984 |
| ESP32-S3-WROOM-1-**N4** (4 MB) | `C2913197` | **$2.93** | 5,063 |

**$1.30 per board, about 21% of the BOM**, and five times the stock. **Taken.**
Board subtotal is now roughly **$4.70**.

Does 4 MB fit? The application binary is **766 KB**. A 4 MB part leaves roughly
3.87 MB after the bootloader, partition table, NVS, otadata and PHY data, which
takes two **1.8 MB** OTA slots comfortably — 2.3× headroom on today's firmware,
with NUT dispatch, MQTT and the remaining web endpoints still to come. Those are
small code, not megabytes.

CLAUDE.md fixes the part at N8 and justifies it as "8 MB of flash covers two OTA
app slots plus a filesystem". **There is no filesystem** — it was removed as
unused and the space left unallocated, so half that justification no longer
holds. The decision is still a decision, and it is not mine to reopen: it trades
future headroom for 21% of the BOM.

## Polyfuse sized against the switch, not against normal draw — fixed

The AP2171W's current limit is **fixed at 1 A**, confirmed from its LCSC listing.
The polyfuse was 1 A hold. Those two being equal was the wrong way round:

- A shorted UPS makes the switch limit at 1 A.
- That 1 A flows through the polyfuse, on top of the board's own ~400 mA.
- 1.4 A through a 1 A-hold fuse trips it, given time and a warm closet.

So a fault on the UPS port would have taken down the **whole bridge**, which is
exactly what the load switch exists to prevent.

**Raised to 2 A hold**, giving 43% margin over the 1.4 A fault case — 1.5 A would
have been only 7%, and hold derates with temperature. It still trips near 4 A,
well inside what a dead short on the 5 V rail draws. Basic-library part; the
change cost nothing. `tools/test/test_power.py` now asserts the relationship, so
changing the switch or the fuse without checking the other fails the harness.

## Before ordering

1. ~~Choose the USB-A receptacle~~ — done, XKB `C2880618`.
2. ~~Decide N8 or N4~~ — done, N4.
3. ~~Raise the polyfuse~~ — done, 2 A hold.
4. Confirm **Basic vs Extended** for every line. JLC charges **$3.07 per unique
 extended part per order**, which at these quantities rivals the parts
 themselves. Their site renders this only in a browser, so it could not be
 checked from here — it needs a human with the parts library open.
