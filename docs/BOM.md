# Bill of materials

Rev A, per board. The upload files are `hardware/bom/bom.csv` and
`hardware/bom/cpl.csv`; this page is the same list with the reasoning. Prices
are LCSC's at quantity 10 and they move: treat them as an order of magnitude,
not a quote. Check stock on every line before ordering.

## Parts

| Ref | Part | LCSC | Qty 10 | Ext |
|---|---|---|---|---|
| U1 | ESP32-S3-WROOM-1-**N4** | `C2913197` | $2.93 | $2.93 |
| U2 | CH340C, WCH (`C7464026` is the same part under a newer listing if `C84681` shows as discontinued) | `C84681` | $0.50 | $0.50 |
| U3 | **AP2114H**-3.3TRG1 SOT-223, 450 mV dropout | `C150716` | $0.062 | $0.06 |
| U4 | **AP2171W**G-7 SOT-23-5, active-high enable | `C110466` | $0.092 | $0.09 |
| D1, D2 | USBLC6-2SC6 SOT-23-6, ST | `C7519` | $0.176 | $0.35 |
| J1 | HRO TYPE-C-31-M-12, USB-C 16P | `C165948` | $0.097 | $0.10 |
| J2 | XKB U231-091N-4BLRA00-S, USB-A right-angle (USB 3.0 9P, used as 2.0) | `C2880618` | $0.14 | $0.14 |
| Q1, Q2 | MMBT3904 SOT-23 | `C20526` | ~$0.01 | $0.02 |
| SW1, SW2 | XKB TS-1187A-B-A-B tact switch | `C318884` | Basic | ~$0.04 |
| D3 | KENTO KT-0603**Y** yellow | `C2287` | ~$0.01 | ~$0.01 |
| D4 | KENTO KT-0603R red | `C2286` | Basic | ~$0.01 |
| F1 | PTTC SMD1206P200TF, 2 A hold / 3.5 A trip | `C545216` | $0.038 | $0.04 |
| J3 | 4-pin 2.54 mm socket, through-hole | | | |
| J5 | 7-pin 2.54 mm header, through-hole | | | |

### Passives

Every passive is pinned to an LCSC code, including commodity values, because
JLC's matcher mis-sizes the footprints: it reads KiCad's `R_0402_1005Metric`
(imperial 0402, metric 1005) as metric 0402, which is imperial 01005, a
quarter the length of the pads, below Economic PCBA's 0402 floor and out of
stock, and it leaves capacitor lines unselected. A blank code is never safe.
Each part below is JLC Basic, stocked in the millions, and **imperial** 0402
where the footprint says 0402.

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

Three lines are one part serving two specifications, which is one fewer
unique part on the order:

- **C8** is specified 100 nF 25 V but sits on VBUS_A, a 5 V rail. A 16 V X7R
  is 3.2x margin, so it shares C3/C6/C9's part.
- **C5** is specified 22 µF 6.3 V on +3V3 and **C4** 22 µF 16 V on +5V. One
  25 V part in the same 0805 covers both, and holds more capacitance under DC
  bias than either.
- **C1** is specified 16 V; the Basic 0402 1 µF is 25 V.

The two 5.1 k resistors are the USB-C CC1 and CC2 pull-downs that make the
connector a sink. Without them no charger supplies 5 V.

## Parts that must not be substituted

Each of these has a look-alike that shares the package, often the pinout
drawing and most of the part number, and is stocked at LCSC, so a search by
package or description reaches the wrong one. Order by LCSC code.

| Order | Never | Why |
|---|---|---|
| **AP2171W** `C110466` | AP2161W `C176957` | Same package and pinout; the AP2161 has an active-**low** enable. With R11 pulling EN to ground it would sit switched on at power-up, and firmware driving the pin high to turn VBUS on would turn it off: the UPS never enumerates and the power-cycle recovery runs backwards. |
| **AP2114H**-3.3TRG1 `C150716` | AP2114**HA**-3.3TRG1 `C460314` | Same SOT-223; the HA has pin 1 VIN, pin 2 GND. Fitting it puts the input rail on ground. |
| **AP2114H**-3.3 `C150716` | AMS1117-3.3 (JLC Basic) | Same package and pinout; 1200 mV dropout against 450 mV. On a USB supply that can sit at 4.5 V at the regulator it falls out of regulation on every Wi-Fi transmit burst, which presents as random resets. |
| **KT-0603Y** yellow `C2287` | a green 0603 LED | A green LED's forward voltage is about 3.3 V, the rail driving it through 1 k. A green part lights on some boards and not others. |
| **CH340C** | CH340G | Same SOP-16; the G needs an external 12 MHz crystal on pins 7 and 8, which are unconnected on this board because the C has its own. |
| capacitors **with the voltage rating** in the value | the cheapest part at the capacitance | A value without a rating hands the choice to JLC's matcher, which on a 5 V rail reaches for a 6.3 V 1206. See below. |

## Capacitor voltage ratings

Every capacitor value carries its rating, because two things depend on it:

**Reliability.** A ceramic on a 5 V rail wants 16 V or better: USB VBUS is
5 V +5%, and an unplugged cable's inductance rings on top of that.

**Capacitance.** Class-II ceramics lose capacitance under DC bias, worst near
their rating. A 100 µF 6.3 V X5R is about 48 µF at 3.3 V and nearer 38 µF at
5 V. Every figure on the schematic is a nominal; derate before judging it.

**C7 is the one compromise.** It is the USB host port's bulk, where effective
capacitance is what matters, and JLC's Basic library has no 1206 above 6.3 V
at this value: a 25 V part in the same package is 22 µF, which after derating
is less capacitance than the 6.3 V part. So C7 runs at 79% of its rating,
giving about 38 µF against the 120 µF USB nominally asks of a host port. The
inrush a device draws on insertion is bounded by the load switch's current
limit, not by this capacitor, and nothing is enumerated at the moment of
insertion for a droop to disturb. If a UPS ever fails to enumerate on
plug-in, a second 100 µF in parallel is the first thing to try; there is room
beside C7.

## Extended-part fees

JLC's Economic assembly charges about **$3 per distinct Extended part per
order** to load its reel; Basic and Preferred Extended parts carry no fee. On
a ten-board order that is $0.30 a board per Extended line, which outweighs
every passive on the board put together. Rev A has nine Extended lines, and
none has a Basic or Preferred equivalent that fits:

- **U1** the module, **U2** CH340C, **J1** USB-C, **J2** USB-A: nothing in
  either library.
- **U4** AP2171W: its active-low sibling would run the UPS power-cycle
  backwards.
- **U3** AP2114H-3.3: the Basic AMS1117-3.3 has the wrong dropout.
- **D1/D2** USBLC6-2SC6, **D3** yellow LED, **F1**: no Basic or Preferred
  part in the footprint.

Consolidating Basic values saves nothing: Basic lines carry no fee, and the
parts cost a fraction of a cent. Nor do larger passives: an 0805 100 nF
costs more than the 0402, and every SMD part is machine-placed whatever its
size.

## Through-hole parts

J3 and J5 are the only through-hole parts and are **not in `cpl.csv`**.
Economic PCBA does fit through-hole parts, by hand, at a labour fee per order
plus a few cents a joint; 11 joints is a few dollars on the order. Either
have JLC fit them or solder them yourself.

## What a board costs

Parts come to roughly **$4.70** a board. The module is about 60% of that;
everything else together is less than $2. Assembly fees on top are mostly
fixed per order (setup, stencil, nine Extended lines), so the per-board price
falls steeply up to about ten boards and slowly after that. See
[`hardware.md`](hardware.md), *Manufacturing and cost*.

## Before ordering

1. Check stock on every line in the table above.
2. Confirm **Basic vs Extended** for every line in JLC's parts library; their
   site shows this only in a browser.
3. Decide whether JLC fits J3 and J5.
4. Follow the placement checks in [`jlcpcb.md`](jlcpcb.md).
