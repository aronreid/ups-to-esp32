# Rev A circuit

One page, the whole board. Exact pin numbers and passive values are in
[`hardware-schematic.md`](hardware-schematic.md); this is the shape of it.

## The whole board

```
 ┌──────────────┐
 │ USB-C │ power + programming + console
 │ (16P sink) │
 └──┬───┬────┬──┘
CC1,CC2│ │ │ D+/D-
 5.1k↓ 5V│ │
 GND │ └──────────┐
 │ │
 ┌────▼────┐ ┌────▼─────┐
 │ polyfuse│ │ USBLC6-C │ ESD
 │ 500mA │ └────┬─────┘
 └────┬────┘ │
 │ ┌────▼─────┐
 ══════╪══════ 5V │ CH340C │ USB <-> serial
 │ │ │ @ 3V3 │ V3 tied to VCC
 │ │ └──┬────┬──┘
 ┌────▼──┐ │ TXD │ │ DTR,RTS
 │AP2114H│ │ RXD │ │
 │ 3.3 │ │ │ ▼
 └───┬───┘ │ │ ┌──────────────┐
 │ │ │ │ Q1,Q2 MMBT3904│ auto-reset
 ═════╪═══ 3V3 │ └───┬───────┬──┘
 │ │ │ │ │ EN │ │ IO0
 │ │ │ │ │ │ │
 │ │ │ │ ┌──────▼──────▼───────▼──────┐
 │ │ │ │ │ │
 │ │ │ └────┤ ESP32-S3-WROOM-1-N8 │
 │ │ │ │ │
 │ │ │ │ IO43/44 UART0 -> CH340C │
 │ │ │ │ IO19/20 USB host │
 │ │ │ │ IO4 VBUS enable │
 │ │ │ │ IO8/9 I2C │
 │ │ │ │ IO47/48 status LEDs │
 │ │ │ └──┬────┬────┬────┬──────┬───┘
 │ │ │ IO8/9│ IO4│ │IO19│IO20 │IO47/48
 │ │ │ │ │ │ │ │
 │ │ └───────────┤ │ │ │ │
 │ │ ┌──────────▼─┐ │ │ │ ┌──▼──┐
 │ │ │ OLED hdr │ │ │ │ │ LEDs│ grn + red
 │ │ │ 4-pin, opt │ │ │ │ │ 1k │
 │ │ └────────────┘ │ │ │ └─────┘
 │ │ │ │ │
 │ └───────────────┐ │ │ │
 │ ┌───▼───▼┐ │ │
 │ │ AP2171W │ │ │ EN from IO4
 │ │ ILIM │ │ │ so firmware can
 │ └────┬────┘ │ │ power-cycle the UPS
 │ │ VBUS_A│ │
 │ ┌────▼───────▼────▼────┐
 │ │ USBLC6-A │ ESD
 │ └────┬───────┬────┬────┘
 │ │ │ │
 │ ┌────▼───────▼────▼────┐
 └──────────────┤ USB-A receptacle │ right-angle
 GND │ VBUS D- D+ GND │
 └──────────┬───────────┘
 │
 [ UPS ]
```

## What each block is for

| Block | Why it is there |
|---|---|
| **USB-C + polyfuse** | One connector for power, flashing and console. CC1/CC2 pull-downs are what make a charger supply 5V — not optional. |
| **AP2114H-3.3** | 3.3V for the module, CH340C, OLED and LEDs. Tab is the output rail; give it ≥100mm² of copper. 450mV dropout, not the AMS1117's 1200mV — on a 5V rail that can legally sag to 4.75V, that is the difference between working and random resets. Order the **H**, not the HA, whose pinout puts VIN on ground. |
| **CH340C at 3.3V** | USB-to-serial. Running it at 3.3V means its TXD/RXD need no level shifting to the S3. `V3` ties to `VCC` at this voltage — at 5V it would need a cap instead, and getting that backwards kills the part. |
| **Q1/Q2 auto-reset** | Lets `esptool` reset and enter bootloader without touching buttons. The cross-coupled emitters mean both transistors stay off when DTR and RTS match, so opening a serial monitor does *not* reset the board. |
| **AP2171W** | Switched 5V to the UPS. `EN` on IO4 is the reliability feature: firmware power-cycles a wedged UPS instead of someone walking to the closet. Current limit is fixed at 1A — a fault limit, not an operating one. The **7**1 matters: AP2161 is active-low enable and would invert the whole feature. |
| **Two USBLC6** | ESD on both USB ports, placed on the connector side of each pair. |
| **USB-A host** | The board supplies VBUS here. **The UPS port sources no power** — this is the single most common reason a build like this never enumerates. |
| **OLED header** | Optional, unpopulated by default. Header and footprint always fitted so a $2 module can be plugged in for debug. |
| **Two LEDs** | Green status, red fault. Plain LEDs rather than a WS2812, which would be an extended part costing $3.07 per order. |

## The three things that will bite

1. **Antenna keepout.** The WROOM-1's antenna must overhang the board edge, no
 copper either layer beneath it, nothing metallic within ~15mm. Place the
 module first; lay out everything else around it.
2. **Bulk capacitance at the module's 3V3 pin.** 100µF, physically adjacent.
 Wi-Fi transmit bursts are why ESP32 designs brown out.
3. **USB pairs.** Tight, matched, top layer, continuous ground beneath, no
 stubs. 12 Mbps needs no controlled impedance, but it does need sane routing.
