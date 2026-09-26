# Rev A schematic specification

Everything needed to draw the Rev A schematic in KiCad without making design
decisions at the keyboard. Nets are named; connections are given by **pin
function name**, not module pin number, because that is how KiCad symbols are
wired and because transcribing pin numbers from memory is how boards get built
wrong. Physical pin numbers are verified once, against datasheets, using the
checklist at the end.

Companion to `hardware.md`, which holds the BOM, costing and layout constraints.

---

## 1. Load switch: AP2171W, with FLAG

**Superseded decision, recorded because the reasoning matters at order time.**

A SOT-23-5 load switch has room for `IN`, `OUT`, `GND`, `EN` and **exactly one**
of `ILIM` or `FLAG`. That was first worked out as an argument about the
SY6280AAC; it is confirmed by the AP2171W, whose verified pinout is
1=`OUT`, 2=`GND`, 3=`FLG`, 4=`EN`, 5=`IN` — FLAG, no ILIM.

Rev A now uses the **AP2171W**, for a blunt reason: it has a KiCad symbol with a
verified pinout, and the SY6280AAC does not. Inventing a pinout for a part that
goes to fabrication is not a risk worth taking to save a few cents. Fault
reporting comes back on GPIO5 as a result, and firmware carries
`BOARD_HAS_VBUS_FAULT 1` again.

**Checked against the datasheet.** The AP2171W limits at **1.5 A typical** (the datasheet's
figure; a distributor listing saying 1.0 A is the *recommended continuous load*,
not the limit). The polyfuse was raised from 500 mA to **2 A hold** so the switch
limits first and the bridge survives a shorted UPS — the whole point of having a
switch. That leaves only 5% margin over the 1.9 A fault case, which is tolerable
rather than comfortable: into a dead short the AP2171W dissipates ~7 W in a
SOT-23-5 and thermally duty-cycles within milliseconds, so the *average* fault
current sits far below the limit, and a polyfuse needs seconds above hold to
trip. `tools/test/test_power.py` asserts the relationship.

### The original argument, for the record

`hardware.md` assigns both an `ILIM` resistor and a `FLAG` output to an
SY6280AAC in SOT-23-5. A 5-pin package cannot carry six functions: `IN`, `OUT`,
`GND`, `EN`, `ILIM` and `FLAG`. One of them does not exist on the part. This
holds regardless of the exact pinout, so it needs settling before the schematic
is drawn.

Three ways out:

| Option | Keeps | Loses | Cost |
|---|---|---|---|
| **A. Keep SY6280AAC, drop FLAG** | adjustable current limit | fault reporting; GPIO5 freed | no change |
| **B. Keep SY6280AAC, drop ILIM** | fault reporting | trip point fixed by the part | no change |
| **C. Move to a SOT-23-6 switch** | both | — | +$0.02, new LCSC part |

**Recommendation: A.** Firmware already detects a wedged UPS by counting failed
polls and power-cycles VBUS on that basis — `ups_hid.c` does not need `FLAG` to
do its job. An overcurrent event is self-protecting because the switch limits
current whether or not anything is listening. Losing the fault line costs a
diagnostic nicety; losing a correct trip point risks nuisance tripping on the
UPS's inrush.

The firmware change was one line, `BOARD_HAS_VBUS_FAULT 0` for Rev A.
`board_vbus_fault` already returned false on boards without the line, so the
web UI and status LEDs needed no change.

The accepted cost: a genuine overcurrent is indistinguishable from a wedged UPS
interface. Both look like failed polls and both get a VBUS power-cycle.

**Option A was taken.** GPIO5 is left unconnected and brought to the expansion
pads, and firmware already carries `BOARD_HAS_VBUS_FAULT 0` for Rev A.

---

## 2. Decisions resolved

Closing out `hardware.md`'s open questions:

| # | Question | Decision |
|---|---|---|
| 1 | USB-C / USB-A LCSC codes | **Chosen and stock-checked :** `C165948` and `C2880618`. Re-check stock at order time. |
| 2 | USB-A vertical or right-angle | **Right-angle.** A UPS is floor- or wall-adjacent; this wants to sit flat. |
| 3 | ~~SY6280 ILIM value~~ | **Moot.** The AP2171W has no ILIM; its limit is fixed at 1.5 A, and the polyfuse was raised to 2 A to suit. |
| 4 | OLED pull-ups with no module fitted | **Safe.** Two 4.7k to 3V3 idle high, drawing ~1.4mA total when a module is absent. One board build covers both cases. |
| 5 | 2-pin 5V input header | **Dropped.** J5 pins 2 and 3 already carry 5 V and GND, and a bare header paralleling USB-C VBUS with no ORing diode lets one 5 V supply be pushed into another. |
| 6 | Reverse-polarity / backfeed Schottky | **Omit.** A series Schottky costs 0.3–0.4 V off the 5 V rail. The AP2171W has **reverse current blocking** built in, which settles the backfeed question the original note deferred. |

---

## 3. Net list

31 nets, of which these carry the design. `GND` is a plane on both layers.

| Net | Members |
|---|---|
| `5V_RAW` | USB-C `VBUS` (all VBUS pins tied); 2-pin header pin 1; polyfuse pin A |
| `+5V` | polyfuse pin B; AP2114H `VIN`; AP2171W `IN`; C4 22µF |
| `+3V3` | AP2114H `VOUT` + tab; module `3V3`; CH340C `VCC` **and `V3`** (see §5); OLED header `VCC`; R3, R4, R5, R6, R12 pull-ups; C2 100µF, C5 22µF, C3/C6/C9 100nF |
| `VBUS_A` | AP2171W `OUT`; USB-A `VBUS`; USBLC6_A `VBUS`; C7 100µF + C8 100nF |
| `USB_DM` | module `IO19`; USBLC6_A `I/O1`; USB-A `D-` |
| `USB_DP` | module `IO20`; USBLC6_A `I/O2`; USB-A `D+` |
| `UART_TX` | module `TXD0` (IO43); CH340C `RXD` |
| `UART_RX` | module `RXD0` (IO44); CH340C `TXD` |
| `PROG_DM` | USB-C `D-` (both pairs tied); USBLC6_C `I/O1`; CH340C `UD-` |
| `PROG_DP` | USB-C `D+` (both pairs tied); USBLC6_C `I/O2`; CH340C `UD+` |
| `EN` | module `EN`; R_EN_PU to 3V3; C_EN 1µF to GND; SW_RESET to GND; Q1 collector |
| `IO0` | module `IO0`; R_IO0_PU to 3V3; SW_BOOT to GND; Q2 collector |
| `DTR` | CH340C `DTR#`; R_Q1B to Q1 base; Q2 emitter |
| `RTS` | CH340C `RTS#`; R_Q2B to Q2 base; Q1 emitter |
| `VBUS_EN` | module `IO4`; AP2171W `EN`; R11 100k **pull-down** — the switch must be off before firmware runs |
| `VBUS_FAULT` | module `IO5`; AP2171W `FLG` (open drain, active low); R12 10k pull-up to `+3V3` |
| `SDA` | module `IO8`; OLED header `SDA`; R_SDA_PU to 3V3 |
| `SCL` | module `IO9`; OLED header `SCL`; R_SCL_PU to 3V3 |
| `LED_STAT` | module `IO48`; R7 1k to D3 **yellow** anode (see §5 for why not green) |
| `LED_FAULT` | module `IO47`; R8 1k to D4 red anode |
| `EXP_IO10`–`EXP_IO13` | module `IO10`–`IO13`; J5 expansion header pins 4–7 |

`CC1` and `CC2` each get their own 5.1k to GND and go nowhere else. They are
what make the connector a sink; without them no charger supplies 5V.

---

## 4. Power tree

```
USB-C VBUS ── polyfuse 2A hold ──┬── AP2114H-3.3 ── 3V3 ── module, CH340C,
 │ OLED, LEDs
 └── AP2171W ── VBUS_A ── USB-A ── UPS
 EN ← IO4 (active high, pulled down)
 FLG → IO5 (open drain, pulled up)
```

The 2-pin 5 V input header was **dropped**: J5 pins 2 and 3 already carry 5 V and
GND, and a bare header paralleling USB-C VBUS with no ORing diode lets one 5 V
supply be pushed into another.

Worst case on `5V` is ~500mA: S3 ~350mA transmit peak, CH340C ~20mA, OLED ~20mA
when fitted, LEDs ~10mA, UPS interface <100mA.

---

## 5. Per-part connections

### ESP32-S3-WROOM-1-N4

Pin numbers verified against KiCad's `RF_Module:ESP32-S3-WROOM-1` symbol (41
pins), which is the symbol the schematic uses.

| Pin | Symbol name | Net | Notes |
|---|---|---|---|
| 1, 40, 41 | `GND` | `GND` | all three, plus the footprint's thermal pad, multiple vias |
| 2 | `3V3` | `3V3` | 100µF bulk **physically adjacent**, plus 100nF |
| 3 | `EN` | `EN` | 10k to 3V3, 1µF to GND, tact switch to GND |
| 4 | `IO4` | `VBUS_EN` | AP2171W EN, **active high**, 100k pull-down so VBUS is off at boot |
| 5 | `IO5` | `VBUS_FAULT` | AP2171W FLG, open drain active low, 10k pull-up |
| 12 | `IO8` | `SDA` | 4.7k pull-up |
| 13 | `USB_D-` (IO19) | `USB_DM` | fixed by silicon |
| 14 | `USB_D+` (IO20) | `USB_DP` | fixed by silicon |
| 15 | `IO3` | — | **leave unconnected**, strapping |
| 16 | `IO46` | — | **leave unconnected**, strapping |
| 17 | `IO9` | `SCL` | 4.7k pull-up |
| 24 | `IO47` | `LED_FAULT` | 1k series, red |
| 25 | `IO48` | `LED_STAT` | 1k series, yellow |
| 26 | `IO45` | — | **leave unconnected**, strapping |
| 27 | `IO0` | `IO0` | 10k to 3V3, tact switch to GND — strapping |
| 18–21 | `IO10`–`IO13` | `EXP_IO10`–`13` | J5 expansion header |
| 28–31 | `IO35`–`IO38` | unconnected | free only because the N4 has no PSRAM |
| 36 | `RXD0` (IO44) | `UART_RX` | from CH340C TXD |
| 37 | `TXD0` (IO43) | `UART_TX` | to CH340C RXD |

Note the symbol exposes IO19/IO20 as `USB_D-`/`USB_D+`. `IO26`–`IO32` are not
brought out by the module; the SPI flash consumes them.

`IO26`–`IO32` are consumed by the module's SPI flash and are not brought out.

### CH340C (SOP-16)

Verified against KiCad's `Interface_USB:CH340C` symbol.

| Pin | Name | Net |
|---|---|---|
| 1 | `GND` | `GND` |
| 2 | `TXD` | `UART_RX` |
| 3 | `RXD` | `UART_TX` |
| 4 | `V3` | **`3V3`** — see note |
| 5 | `UD+` | `PROG_DP` |
| 6 | `UD-` | `PROG_DM` |
| 7, 8 | `NC` | unconnected — confirms the **C** variant's internal crystal |
| 13 | `~DTR` | `DTR` |
| 14 | `~RTS` | `RTS` |
| 16 | `VCC` | `3V3` |
| 9–12, 15 | `~CTS`, `~DSR`, `~RI`, `~DCD`, `R232` | unconnected |

The symbol types `V3` as a power **output**, which is the 5V-operation case
where it sources 3.3V. At 3.3V operation it is an input and ties to `VCC`.

**The `V3` pin is a classic failure.** It behaves differently by supply voltage:
tie `V3` to `VCC` when running the part at 3.3V, or decouple it with 100nF to
GND when running at 5V. This design runs the CH340C at **3.3V**, so `V3` ties to
`3V3`. That also means `TXD`/`RXD` are 3.3V logic and need no level shifting to
the S3 — which is why the part is on the 3.3V rail in the first place.

Note `TXD`/`RXD` cross: the CH340C's TXD drives the module's RXD0.

### Auto-reset (2 × MMBT3904, esptool-compatible)

Verified against KiCad's `Transistor_BJT:MMBT3904` (parent `Q_NPN_BEC`):
**pin 1 = B, pin 2 = E, pin 3 = C.**

| Transistor | Base (1) | Emitter (2) | Collector (3) |
|---|---|---|---|
| Q1 | `DTR` via 1k | `RTS` | `EN` |
| Q2 | `RTS` via 1k | `DTR` | `IO0` |

The cross-coupled emitters are the whole trick: both transistors stay off when
DTR and RTS are at the same level, so a normal serial session never resets the
board. Only the opposing-level sequence esptool drives pulls `EN` or `IO0` low.

### AP2114H-3.3 (SOT-223)

Verified against the Diodes AP2114 datasheet pin table: the **H** package is
**1 = GND, 2 = VOUT, 3 = VIN**, tab = `VOUT`. The schematic uses KiCad's generic
`Regulator_Linear:AP1117-33` symbol, which has identical pin numbering; the
*value* is what reaches the BOM. Give the tab ≥100mm² of pour, and note the pour
is on the *output* rail.

**Order the H, never the HA.** `AP2114HA` is the same SOT-223 with
**1 = VIN, 2 = GND, 3 = VOUT** — fitting it puts the input rail onto ground. Both
are stocked at LCSC. `tools/test/test_bom.py` pins `C150716`.

**Why not the AMS1117**, which this was until : same package, same
pinout, but 1200 mV of dropout against the AP2114H's 450 mV. USB may legally
deliver 4.75 V at the port and a thin metre of cable at half an amp costs another
200 mV, which left the AMS1117 with **10 mV** of headroom — on a part feeding a
chip that draws 350 mA bursts every time it transmits. That is the most reported
failure of AMS1117-powered ESP32 boards, and it presents as random resets that
look like a firmware bug. `tools/test/test_power.py` now checks this headroom.

| Pin | Name | Net |
|---|---|---|
| 1 | `GND` | `GND` |
| 2 | `VO` + tab | `3V3` |
| 3 | `VI` | `5V` |

### AP2171W (SOT-23-5)

Verified against KiCad's `Power_Management:AP2171W` and the Diodes datasheet:
**1 = OUT, 2 = GND, 3 = FLG, 4 = EN, 5 = IN.** No `ILIM` — the limit is fixed at
1.5 A typical by the part.

| Pin | Signal | Net |
|---|---|---|
| 1 | `OUT` | `VBUS_A` |
| 2 | `GND` | `GND` |
| 3 | `FLG` | `VBUS_FAULT` — open drain, **active low**, 10k pull-up to `+3V3` |
| 4 | `EN` | `VBUS_EN` — **active high**, 100k pull-down |
| 5 | `IN` | `+5V` |

**Order the AP217 1W, never the AP216 1W.** They share the package and the
pinout and differ *only* in enable polarity: AP2161 is active **low**, so with
R11 pulling `EN` to ground it would sit switched on at power-up, and
`board_vbus_set(true)` driving the pin high would turn VBUS **off**. The UPS
would never enumerate and the power-cycle recovery would run backwards. Both are
stocked at LCSC; `tools/test/test_bom.py` pins `C110466`.

### USBLC6-2SC6 ×2

One per connector, placed **on the connector side** of the pair, never the MCU
side. `VBUS` pin to that connector's 5V net, `GND` to ground, `I/O1`/`I/O2` in
series with D-/D+.

Verified against KiCad's `Power_Protection:USBLC6-2SC6` (parent `USBLC6-2P6`):
**1 = I/O1, 2 = GND, 3 = I/O2, 4 = I/O2, 5 = VBUS, 6 = I/O1.** Pins 1/6 are the
same line and 3/4 are the other, which is what lets the part sit in-line with
the pair without stubs — route in on 1, out on 6.

| Instance | I/O1 (1, 6) | I/O2 (3, 4) | VBUS (5) | GND (2) |
|---|---|---|---|---|
| USBLC6_C (USB-C) | `PROG_DM` | `PROG_DP` | `5V_RAW` | `GND` |
| USBLC6_A (USB-A) | `USB_DM` | `USB_DP` | `VBUS_A` | `GND` |

### Connectors

**USB-C 16P (sink):** all `VBUS` pins tied to `5V_RAW`; all `GND` tied; `D+`
pairs tied together, `D-` pairs tied together, to `PROG_DP`/`PROG_DM`; `CC1` and
`CC2` each 5.1k to GND; `SBU1`/`SBU2` unconnected; shield to `GND`.

**USB-A right-angle (host):** `VBUS` → `VBUS_A`, `D-` → `USB_DM`,
`D+` → `USB_DP`, `GND` → `GND`, shield to `GND`.

**OLED header, 4-pin 2.54mm:** `VCC` → `3V3`, `GND` → `GND`, `SDA`, `SCL`.
Through-hole. Economic PCBA fits these by hand ($3.50 labour plus $0.0173 a
joint), so it is a few cents on top of a once-per-order fee, not a blocker.

---

## 6. Decoupling

| Location | Parts |
|---|---|
| Module `3V3` | C5 22µF + C9 100nF, as close as the RESET button allows (~11mm) |
| Regulator in / out | C4 22µF in, C2 100µF out, + C3 100nF |
| `VBUS_A` | C7 **100µF** + C8 100nF, at the USB-A connector |
| CH340C `VCC` | 100nF |
| `EN` | 1µF to GND (with the 10k pull-up) |
| Each USBLC6 | 100nF on its VBUS pin |
| OLED header `VCC` | 100nF |

Bulk capacitance at the module's 3V3 pin is the single most important passive on
the board. Wi-Fi transmit bursts are why ESP32 designs brown out.

**Every capacitance here is a nominal that has to be derated before it means
anything.** Class-II ceramics lose capacitance under DC bias, worst near their
rating: C7's 100µF 6.3V part measures nearer 38µF on a 5V rail. Voltage ratings
are part of the value in the schematic for exactly this reason, and
`tools/test/test_power.py` derates before judging anything. See `BOM.md`.

---

## 7. Verify before ordering

**Pinouts — all verified, against primary datasheets as well as the symbols:**

- [x] **ESP32-S3-WROOM-1** — Espressif datasheet. Pin 36 is `RXD0`, 37 is `TXD0`; IO19/20 are `USB_D-`/`D+`.
- [x] **CH340C** — 3.3V operation with `V3` tied to `VCC` is the datasheet's own instruction. Draws 4–12mA.
- [x] **USBLC6-2SC6** — standoff 5.25V, breakdown 6.1V. 5.25V is exactly USB's maximum, which is the point of the part.
- [x] **AP2114H-3.3** — Diodes datasheet pin table. H package, tab is `VOUT`. **Not HA.**
- [x] **AP2171W** — Diodes datasheet. 1=OUT, 2=GND, 3=FLG, 4=EN, 5=IN, enable **active high**. **Not AP2161W.**
- [x] **MMBT3904** — 1=B, 2=E, 3=C. Cross-coupled emitters give the esptool truth table.
- [x] **Connector pinouts** — `tools/test/test_connectors.py` checks J1 and J2 against the USB spec on every build, including that CC1 and CC2 stay separate nets.

Electrical suitability was audited separately — pinout is not the same question,
and the AMS1117's dropout proved it. See **`docs/datasheet-audit.md`**.

**Still needs a human with a browser:**

- [ ] **CPL rotation** against JLC's assembly preview. `tools/test/check_cpl_rotation.py` lists the placements in package families where KiCad and JLC disagree; the preview is the only authority.
- [ ] **D1–D4 polarity** against the render. Each carries a pin-1 dot on silk (pin 1 is the cathode); the stock footprint marks are kept as well.
- [ ] **Basic vs Extended** per BOM line — JLC charges ~$3 per unique extended part per order.

Ordered connectors, stock checked :

| Part | Chosen | LCSC |
|---|---|---|
| USB-C 16P sink | HRO TYPE-C-31-M-12 | `C165948` |
| USB-A right-angle | XKB U231-091N-4BLRA00-S (USB 3.0 9P, used as 2.0 — the smallest SMD A receptacle available) | `C2880618` |

## 8. ERC and DFM checks

Once drawn, these are mechanical:

```sh
bash tools/test/run-all.sh # 16 checks, ERC and netlist among them
```

The BOM is **not** exported from the schematic with `kicad-cli`. `tools/gen-bom.py`
derives `hardware/bom/bom.csv` and `cpl.csv` from the same table that generates
the schematic, on every build, because a hand-exported BOM sat stale here for
days and still named the AP2161W when it was finally read.

Expect ERC to flag the intentionally unconnected pins (`IO3`, `IO45`, `IO46`,
CH340C's unused modem lines, `SBU1`/`SBU2`). Mark them with no-connect flags
rather than suppressing the rule, so a genuinely forgotten net still shows up.

Before ordering, confirm against `hardware.md` §layout: antenna keepout with the
module at a board edge and no copper beneath it, D+/D- routed as tight matched
pairs over continuous ground, and ≥100mm² of copper on the regulator tab.
There are **no mounting holes** — the enclosure uses case clips, to keep the
board at 36 × 58 mm.
