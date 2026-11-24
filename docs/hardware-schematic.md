# Rev A schematic, net by net

Every net and pin of the Rev A board, as built. Connections are given by
**pin function name** as well as pin number, because that is how KiCad
symbols are wired. Physical pin numbers were verified against the part
datasheets and against the KiCad symbols the schematic uses; the per-part
tables record both. The schematic itself is [`schematic.pdf`](schematic.pdf).

Companion to [`hardware.md`](hardware.md), which holds the pin map, power
tree, layout constraints and the OLED header warning, and
[`BOM.md`](BOM.md), which holds the parts.

---

## 1. Net list

31 nets, of which these carry the design. `GND` is a plane on both layers.

| Net | Members |
|---|---|
| `5V_RAW` | USB-C `VBUS` (all VBUS pins tied); USBLC6_C `VBUS`; polyfuse F1 pin A |
| `+5V` | F1 pin B; AP2114H `VIN`; AP2171W `IN`; J5 pin 2; C4 22 µF |
| `+3V3` | AP2114H `VOUT` + tab; module `3V3`; CH340C `VCC` **and `V3`** (see §3); J3 pin 1; J5 pin 1; R3, R4, R5, R6, R12 pull-ups; C2 100 µF, C5 22 µF, C3/C6/C9 100 nF |
| `VBUS_A` | AP2171W `OUT`; USB-A `VBUS`; USBLC6_A `VBUS`; C7 100 µF + C8 100 nF |
| `USB_DM` | module `IO19`; USBLC6_A `I/O1`; USB-A `D-` |
| `USB_DP` | module `IO20`; USBLC6_A `I/O2`; USB-A `D+` |
| `UART_TX` | module `TXD0` (IO43); CH340C `RXD` |
| `UART_RX` | module `RXD0` (IO44); CH340C `TXD` |
| `PROG_DM` | USB-C `D-` (both pairs tied); USBLC6_C `I/O1`; CH340C `UD-` |
| `PROG_DP` | USB-C `D+` (both pairs tied); USBLC6_C `I/O2`; CH340C `UD+` |
| `EN` | module `EN`; R3 10k to 3V3; C1 1 µF to GND; SW1 RESET to GND; Q1 collector |
| `IO0` | module `IO0`; R4 10k to 3V3; SW2 BOOT to GND; Q2 collector |
| `DTR` | CH340C `DTR#`; R1 1k to Q1 base; Q2 emitter |
| `RTS` | CH340C `RTS#`; R2 1k to Q2 base; Q1 emitter |
| `VBUS_EN` | module `IO4`; AP2171W `EN`; R11 100k **pull-down**: the switch is off before firmware runs |
| `VBUS_FAULT` | module `IO5`; AP2171W `FLG` (open drain, active low); R12 10k pull-up to `+3V3` |
| `SDA` | module `IO8`; J3 pin 3; R5 4.7k to 3V3 |
| `SCL` | module `IO9`; J3 pin 4; R6 4.7k to 3V3 |
| `LED_STAT` | module `IO48`; R7 1k to D3 **yellow** anode |
| `LED_FAULT` | module `IO47`; R8 1k to D4 red anode |
| `EXP_IO10` to `EXP_IO13` | module `IO10` to `IO13`; J5 pins 4 to 7 |
| `CC1`, `CC2` | USB-C `CC1`, `CC2`; R9, R10 5.1k to GND, each its own net |

`CC1` and `CC2` each get their own 5.1k to GND and go nowhere else. They are
what make the connector a sink; without them no charger supplies 5 V.

---

## 2. Power tree

```
USB-C VBUS ── polyfuse 2A hold ──┬── AP2114H-3.3 ── 3V3 ── module, CH340C,
 │ OLED, LEDs, J5 pin 1
 ├── J5 pin 2
 └── AP2171W ── VBUS_A ── USB-A ── UPS
 EN ← IO4 (active high, pulled down)
 FLG → IO5 (open drain, pulled up)
```

Worst case on `5V` is about 500 mA: S3 350 mA transmit peak, CH340C 20 mA,
OLED 20 mA when fitted, LEDs 10 mA, UPS interface under 100 mA.

There is no separate 5 V input header. J5 pins 2 and 3 carry 5 V and GND; a
bare header paralleling USB-C VBUS with no ORing diode would let one 5 V
supply be pushed into another, so none is fitted.

No reverse-polarity or backfeed Schottky: a series diode costs 0.3 to 0.4 V
off the 5 V rail, and the AP2171W blocks reverse current itself.

---

## 3. Per-part connections

### ESP32-S3-WROOM-1-N4 (U1)

Pin numbers verified against KiCad's `RF_Module:ESP32-S3-WROOM-1` symbol (41
pins), which is the symbol the schematic uses, and against the Espressif
datasheet. The module needs 3.0 to 3.6 V and a supply good for 500 mA; it gets
3.3 V from a 1 A regulator.

| Pin | Symbol name | Net | Notes |
|---|---|---|---|
| 1, 40, 41 | `GND` | `GND` | all three, plus the footprint's thermal pad, multiple vias |
| 2 | `3V3` | `3V3` | 100 µF bulk **physically adjacent**, plus 100 nF |
| 3 | `EN` | `EN` | 10k to 3V3, 1 µF to GND, tact switch to GND |
| 4 | `IO4` | `VBUS_EN` | AP2171W EN, **active high**, 100k pull-down so VBUS is off at boot |
| 5 | `IO5` | `VBUS_FAULT` | AP2171W FLG, open drain active low, 10k pull-up |
| 12 | `IO8` | `SDA` | 4.7k pull-up |
| 13 | `USB_D-` (IO19) | `USB_DM` | fixed by silicon |
| 14 | `USB_D+` (IO20) | `USB_DP` | fixed by silicon |
| 15 | `IO3` | n/a | **unconnected**, strapping |
| 16 | `IO46` | n/a | **unconnected**, strapping |
| 17 | `IO9` | `SCL` | 4.7k pull-up |
| 18 to 21 | `IO10` to `IO13` | `EXP_IO10` to `13` | J5 expansion header |
| 24 | `IO47` | `LED_FAULT` | 1k series, red |
| 25 | `IO48` | `LED_STAT` | 1k series, yellow |
| 26 | `IO45` | n/a | **unconnected**, strapping |
| 27 | `IO0` | `IO0` | 10k to 3V3, tact switch to GND, strapping, no capacitor |
| 28 to 31 | `IO35` to `IO38` | unconnected | free only because the N4 has no PSRAM |
| 36 | `RXD0` (IO44) | `UART_RX` | from CH340C TXD |
| 37 | `TXD0` (IO43) | `UART_TX` | to CH340C RXD |

The symbol exposes IO19/IO20 as `USB_D-`/`USB_D+`. `IO26` to `IO32` are
consumed by the module's SPI flash and are not brought out. `IO3`, `IO45` and
`IO46` float so their internal pull-downs select 3.3 V `VDD_SPI` and normal
boot.

Two Espressif checklist recommendations are not followed, on purpose. There
is no 499 Ω series resistor on U0TXD: the line runs a few centimetres to a
USB-serial bridge on the same board at 115200 baud, and the radio is inside a
pre-certified module. If the board ever goes for formal emissions testing,
this is the first thing to add. There are no series resistor or capacitor
footprints on the USB lines: that guidance is aimed at high-speed USB, and
this board is full speed only.

### CH340C (U2, SOP-16)

Verified against KiCad's `Interface_USB:CH340C` symbol and the WCH datasheet.

| Pin | Name | Net |
|---|---|---|
| 1 | `GND` | `GND` |
| 2 | `TXD` | `UART_RX` |
| 3 | `RXD` | `UART_TX` |
| 4 | `V3` | **`3V3`** (see note) |
| 5 | `UD+` | `PROG_DP` |
| 6 | `UD-` | `PROG_DM` |
| 7, 8 | `NC` | unconnected: the **C** variant has an internal crystal |
| 13 | `~DTR` | `DTR` |
| 14 | `~RTS` | `RTS` |
| 16 | `VCC` | `3V3` |
| 9 to 12, 15 | `~CTS`, `~DSR`, `~RI`, `~DCD`, `R232` | unconnected |

**The `V3` pin behaves differently by supply voltage.** At 3.3 V operation
it is an input and ties to `VCC`, which is the datasheet's own instruction;
at 5 V it is an output and needs 100 nF to GND instead. This design runs the
CH340C at **3.3 V**, so `V3` ties to `3V3`. That also means `TXD`/`RXD` are
3.3 V logic and need no level shifting to the S3, which is why the part is on
the 3.3 V rail. The KiCad symbol types `V3` as a power output, which is the
5 V case; expect ERC to remark on it. The part draws 4 to 12 mA.

`TXD`/`RXD` cross: the CH340C's TXD drives the module's RXD0.

### Auto-reset (Q1, Q2: 2 x MMBT3904, esptool-compatible)

Verified against KiCad's `Transistor_BJT:MMBT3904` (parent `Q_NPN_BEC`):
**pin 1 = B, pin 2 = E, pin 3 = C.**

| Transistor | Base (1) | Emitter (2) | Collector (3) |
|---|---|---|---|
| Q1 | `DTR` via R1 1k | `RTS` | `EN` |
| Q2 | `RTS` via R2 1k | `DTR` | `IO0` |

The cross-coupled emitters are the whole trick: both transistors stay off
when DTR and RTS are at the same level, so a normal serial session never
resets the board. Only the opposing-level sequence esptool drives pulls `EN`
or `IO0` low. The CH340C drives each base through 1 kΩ, about 2.6 mA, far
more than the collectors' 0.3 mA needs to saturate.

### AP2114H-3.3 (U3, SOT-223)

Verified against the Diodes AP2114 datasheet pin table: the **H** package is
**1 = GND, 2 = VOUT, 3 = VIN**, tab = `VOUT`. The schematic uses KiCad's
generic `Regulator_Linear:AP1117-33` symbol, which has identical pin
numbering; the *value* is what reaches the BOM. The tab has at least 100 mm²
of pour, and the pour is on the *output* rail.

| Pin | Name | Net |
|---|---|---|
| 1 | `GND` | `GND` |
| 2 | `VO` + tab | `3V3` |
| 3 | `VI` | `5V` |

**Order the H, never the HA.** `AP2114HA` is the same SOT-223 with
**1 = VIN, 2 = GND, 3 = VOUT**: fitting it puts the input rail onto ground.
Both are stocked at LCSC. The AP2114H is also chosen over the AMS1117, which
shares the package and pinout, for dropout: 450 mV against 1200 mV. See
[`BOM.md`](BOM.md), *Parts that must not be substituted*.

### AP2171W (U4, SOT-23-5)

Verified against KiCad's `Power_Management:AP2171W` and the Diodes datasheet:
**1 = OUT, 2 = GND, 3 = FLG, 4 = EN, 5 = IN.** There is no `ILIM` pin; the
limit is fixed by the part at 1.5 A typical (1.1 A minimum, 1.9 A maximum).
Input range 2.7 to 5.5 V. `EN` reads high above 2.0 V and gets 3.3 V. `FLG`
is open drain, active low on over-current or over-temperature, with a 7 ms
deglitch. The output rises in 0.6 ms typical, and the switch blocks reverse
current.

| Pin | Signal | Net |
|---|---|---|
| 1 | `OUT` | `VBUS_A` |
| 2 | `GND` | `GND` |
| 3 | `FLG` | `VBUS_FAULT`, open drain, **active low**, 10k pull-up to `+3V3` |
| 4 | `EN` | `VBUS_EN`, **active high**, 100k pull-down |
| 5 | `IN` | `+5V` |

Firmware reads `VBUS_FAULT` active low (`BOARD_HAS_VBUS_FAULT 1` in
`components/board`) and drives `VBUS_EN` high to power the UPS.

**Order the AP217 1W, never the AP216 1W.** They share the package and the
pinout and differ *only* in enable polarity: AP2161 is active **low**, so
with R11 pulling `EN` to ground it would sit switched on at power-up, and
firmware driving the pin high would turn VBUS **off**. The UPS would never
enumerate and the power-cycle recovery would run backwards. Both are stocked
at LCSC.

### USBLC6-2SC6 x2 (D1, D2)

One per connector, placed **on the connector side** of the pair, never the
MCU side. `VBUS` pin to that connector's 5 V net, `GND` to ground,
`I/O1`/`I/O2` in series with D-/D+.

Verified against KiCad's `Power_Protection:USBLC6-2SC6` (parent `USBLC6-2P6`)
and the ST datasheet: **1 = I/O1, 2 = GND, 3 = I/O2, 4 = I/O2, 5 = VBUS,
6 = I/O1.** Pins 1/6 are the same line and 3/4 are the other, which is what
lets the part sit in-line with the pair without stubs: route in on 1, out on
6. Reverse standoff 5.25 V, breakdown 6.1 V; 5.25 V is exactly USB's maximum,
which is the point of the part. Leakage at that voltage is tens of nanoamps.

| Instance | I/O1 (1, 6) | I/O2 (3, 4) | VBUS (5) | GND (2) |
|---|---|---|---|---|
| D1, USBLC6_C (USB-C) | `PROG_DM` | `PROG_DP` | `5V_RAW` | `GND` |
| D2, USBLC6_A (USB-A) | `USB_DM` | `USB_DP` | `VBUS_A` | `GND` |

### Connectors

**J1, USB-C 16P (sink), HRO TYPE-C-31-M-12:** all `VBUS` pins tied to
`5V_RAW`; all `GND` tied; `D+` pairs tied together, `D-` pairs tied together,
to `PROG_DP`/`PROG_DM`; `CC1` and `CC2` each 5.1k to GND; `SBU1`/`SBU2`
unconnected; shield to `GND`.

**J2, USB-A right-angle (host), XKB U231-091N-4BLRA00-S:** `VBUS` to
`VBUS_A`, `D-` to `USB_DM`, `D+` to `USB_DP`, `GND` to `GND`, shield to
`GND`. The five SuperSpeed contacts are unconnected.

**J3, OLED socket, 4-pin 2.54 mm female, from pin 1:** `3V3`, `GND`, `SDA`,
`SCL`. This is not the order most 0.96" SSD1306 modules print; read
[`hardware.md`](hardware.md), *OLED header J3*, before plugging one in. The
two 4.7k pull-ups idle high with no module fitted, so one board build covers
both cases.

**J5, expansion, 7-pin 2.54 mm male, from pin 1:** `3V3`, `5V`, `GND`,
`IO10`, `IO11`, `IO12`, `IO13`.

J3 and J5 are through-hole and are not in the placement file.

---

## 4. Decoupling

| Location | Parts |
|---|---|
| Module `3V3` | C5 22 µF + C9 100 nF, as close as the RESET button allows (about 11 mm) |
| Regulator in / out | C4 22 µF in, C2 100 µF out, + C3 100 nF |
| `VBUS_A` | C7 **100 µF** + C8 100 nF, at the load switch and USB-A connector |
| CH340C `VCC` | C6 100 nF |
| `EN` | C1 1 µF to GND (with the 10k pull-up) |

Bulk capacitance at the module's 3V3 pin is the single most important passive
on the board. Wi-Fi transmit bursts are why ESP32 designs brown out.

**Every capacitance here is a nominal that has to be derated before it means
anything.** Class-II ceramics lose capacitance under DC bias, worst near
their rating: C7's 100 µF 6.3 V part measures nearer 38 µF on a 5 V rail.
Voltage ratings are part of the value in the schematic for exactly this
reason. See [`BOM.md`](BOM.md), *Capacitor voltage ratings*.

---

## 5. Checks before ordering

Pinouts, verified against primary datasheets as well as the KiCad symbols:

- **ESP32-S3-WROOM-1**: pin 36 is `RXD0`, 37 is `TXD0`; IO19/20 are
  `USB_D-`/`D+`.
- **CH340C**: 3.3 V operation with `V3` tied to `VCC`.
- **USBLC6-2SC6**: standoff 5.25 V, breakdown 6.1 V.
- **AP2114H-3.3**: H package, tab is `VOUT`. **Not HA.**
- **AP2171W**: 1 = OUT, 2 = GND, 3 = FLG, 4 = EN, 5 = IN, enable **active
  high**. **Not AP2161W.**
- **MMBT3904**: 1 = B, 2 = E, 3 = C.
- **J1 and J2** against the USB specification, including that CC1 and CC2 stay
  separate nets.

Needs a human with a browser, every order:

- **Placement rotation** against JLC's assembly preview; the preview is the
  only authority. See [`jlcpcb.md`](jlcpcb.md).
- **D1 to D4 polarity** against the render. Pin 1 is the cathode; D3 and D4
  carry a bar across the cathode end on silk.
- **Basic vs Extended** per BOM line.

ERC flags the intentionally unconnected pins (`IO3`, `IO45`, `IO46`, the
CH340C's unused modem lines, `SBU1`/`SBU2`). They carry no-connect flags
rather than a suppressed rule, so a forgotten net still shows up.

Layout, from [`hardware.md`](hardware.md): antenna keep-out with the module at
a board edge and no copper beneath it, D+/D- as tight matched pairs over
continuous ground, at least 100 mm² of copper on the regulator tab, and no
mounting holes: the enclosure holds the 36 x 58 mm board by its edges.
