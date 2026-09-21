# Datasheet audit

Checked part by part against primary datasheets. Pinouts were
verified earlier; **this pass looked at the electrical tables instead**, because
that is where the AMS1117 problem was hiding. Its pinout was right. Its dropout
voltage was not, and no amount of pinout checking would ever have found it.

## What this found

| Part | Checked | Result |
|---|---|---|
| **U3** regulator | dropout vs worst-case supply | **Defect.** AMS1117 needed 4.50 V, had 4.51 V. Replaced with AP2114H-3.3 |
| **D3** status LED | Vf vs the rail driving it | **Defect.** Green Vf 3.3 V typ on a 3.3 V rail. Replaced with a 591 nm yellow |
| **U1** ESP32-S3 module | supply range, EN RC, strapping | OK |
| **U2** CH340C | 3.3 V operation, V3 handling | OK |
| **U4** AP2171W | EN threshold, current limit, FLG | OK |
| **D1, D2** USBLC6-2SC6 | standoff voltage on a 5 V rail | OK |
| **Q1, Q2** MMBT3904 | base drive, ratings | OK |
| **F1** polyfuse | rating, hold vs fault current | OK, 5% margin — see `BOM.md` |
| capacitors | voltage rating, DC-bias derating | **Defect.** No ratings specified at all. Fixed |

Two defects, both invisible to DRC, to the netlist, to ERC and to every check
that existed before. Both would have produced hardware that looked correct and
behaved badly — random resets blamed on firmware, and an indicator that lit on
some boards and not others.

## The detail

**U1, ESP32-S3-WROOM-1-N4.** Needs 3.0–3.6 V and a supply good for 500 mA; it
gets 3.25–3.35 V from a 1 A regulator. `EN` has Espressif's own recommended
10 kΩ + 1 µF. `GPIO0` is pulled up with no capacitor on it, which the checklist
asks for specifically. `GPIO3`, `GPIO45` and `GPIO46` are left floating, so
their internal pull-downs select 3.3 V `VDD_SPI` and normal boot.

The checklist warns of power-up glitches on GPIO17–20, which are our USB host
lines. That costs nothing here: `VBUS_EN` is pulled *down*, so the switch is off
and the UPS is unpowered until firmware decides otherwise. The safe default
chosen for other reasons covers this too.

**U2, CH340C at 3.3 V.** The datasheet is explicit: at 3.3 V, `V3` is tied to
`VCC`, which is what pins 4 and 16 do. Draws 4–12 mA against the 20 mA budgeted.
At 5 V it would instead need a 100 nF on `V3` — wiring it as we have at 5 V
would be the mistake, and it is not what we have.

**U4, AP2171W.** `EN` needs 2.0 V to read high and gets 3.3 V. `FLG` is open
drain, active low, with a 10 kΩ pull-up — matching `board_vbus_fault`. Input
range 2.7–5.5 V.

**D1/D2, USBLC6-2SC6.** Reverse standoff 5.25 V, breakdown 6.1 V. 5.25 V is
exactly USB's maximum, which is the point of the part. Leakage at that voltage
is tens of nanoamps.

**Q1/Q2, MMBT3904.** The CH340C drives each base through 1 kΩ, about 2.6 mA,
into a transistor needing well under that to saturate at the ~0.3 mA these
collectors carry.

## Espressif recommendations NOT followed

Both are recommendations rather than requirements, and both are deliberate:

**No 499 Ω series resistor on U0TXD.** The checklist asks for one to suppress
harmonics. Ours is a 115200-baud line to a USB-serial bridge on the same board,
a few centimetres long, and the radio is inside a pre-certified module. If this
ever goes for formal emissions testing, this is the first thing to add.

**No series resistors or ground capacitors reserved on the USB lines.** The
checklist asks for 22/33 Ω footprints, unpopulated, close to the chip. That
guidance is aimed at high-speed USB; this board is full speed only, 12 Mbps,
where the edges are slow enough not to need tuning. Adding four unpopulated
0402s to a board at 74% utilisation costs placement and routing for a problem
that is unlikely to appear. Worth reconsidering if a Rev B has room.

## What a datasheet cannot tell you

Everything above is paper. The failure modes that remain need a built board:
the antenna's real performance next to a metal UPS, whether the UPS enumerates
on hot-plug, and whether anything oscillates. Those are Rev A's job.
