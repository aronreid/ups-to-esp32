# Rev A readiness: what the devkit work does and does not prove

All firmware to date has run on a **Freenove ESP32-S3-WROOM devkit**. Rev A is
the reference target and has never been powered. This page audits every place
the two differ, so the differences are a list rather than a hope.

Written 2026-09-21, against `v0.04` plus the fixes below.

## The structural protection

Application code contains **no GPIO numbers at all**:

```
$ grep -rn "GPIO_NUM_[0-9]" firmware/components firmware/main | grep -v components/board/
  (nothing)
```

Every pin lives in `components/board/include/board.h`, and every conditional on
which board is being built lives in `components/board/board.c` — eight `#if`
blocks, nowhere else. `tools/test/test_board_targets.py` compiles `board.c` for
both targets with `-Werror` on every harness run, and asserts that an
unselected target still fails loudly rather than silently inheriting a pin map.

That is why a session of devkit work cannot quietly reshape Rev A: there is
almost nothing for it to reshape.

## What the devkit cannot exercise, and what was found by reading it

| Rev A feature | Devkit | Status |
|---|---|---|
| AP2171W load switch (`EN`, GPIO4) | absent — VBUS hardwired | **two defects found and fixed, below** |
| AP2171W fault flag (`FLG`, GPIO5) | absent | **one defect found and fixed, below** |
| Status/fault LEDs (GPIO48/47) | absent | reviewed; see below |
| VBUS power-cycle recovery | returns `ESP_ERR_NOT_SUPPORTED` | **the defects were here** |

### 1. The recovery ran in the wrong task (Rev A only)

`ups_hid_recover()` performed the VBUS cycle and then cleared the HID report
map **in the calling task** — the web server, for the "reset UPS link" button.

On the devkit that is harmless: `board_vbus_cycle()` returns immediately, so
nothing happens in between. On Rev A it is a **two-second blocking window**
(`CONFIG_UPSA_VBUS_CYCLE_MS`) during which `ups_task` keeps running. The UPS
loses power, disconnects, VBUS returns, and the device can re-enumerate and
repopulate `s_map` *before the web server task reaches its `memset`*. Clearing
it then wipes a freshly parsed descriptor that the poll plan already points
into, and the UPS sits attached and unreadable until a human unplugs it.

A recovery that causes the fault it exists to clear, on the board where the
recovery is the reason the load switch is fitted.

Fixed by routing recovery through the existing command queue so it runs in
`ups_task` like every other operation that touches the map. The automatic
recovery path needed care: it already runs *inside* `ups_task`, so it calls the
worker directly — queueing there would have deadlocked.

### 2. The poll plan outlived the map it indexes

The same function cleared `s_map` but left `s_poll_count`, `s_poll_rid[]`,
`s_poll_bytes[]` and `s_date_field` untouched. Those are indexes and pointers
*into* the map, so the poll loop would read a zeroed descriptor through a plan
that still looked valid. Now cleared together.

### 3. `FLG` was read while VBUS was deliberately off

`board_vbus_fault()` returned whatever the pin said, and the supervisor samples
it every 500 ms with priority over every other status. Firmware holds VBUS off
on purpose twice — from `board_init()` until `ups_task` raises it, and for two
seconds on every power-cycle recovery. A fault reading in those windows means
nothing, and would light the red FAULT LED and set `vbus_fault` on the status
page *every time the board recovered a wedged UPS*.

Now returns false unless the switch is commanded on.

### 4. Status LEDs — reviewed, not run

`led_task` drives GPIO48 (green) and GPIO47 (red) from 8-slot bitmap patterns
on a 100 ms tick. Reviewed for correctness; it cannot be verified without the
board. Both pins are ordinary GPIOs on the S3 — not strapping pins, not part of
the module's SPI flash range (GPIO26–32) — and `board_init()` drives both low
before the task starts. First bring-up should confirm green solid at boot and
red slow-blink with no UPS attached.

## Pin map sanity

| Signal | Rev A | Strapping? | Flash? |
|---|---|---|---|
| `VBUS_EN` | GPIO4 | no | no |
| `VBUS_FAULT` | GPIO5 | no | no |
| `LED_STATUS` | GPIO48 | no | no |
| `LED_FAULT` | GPIO47 | no | no |
| `I2C_SDA` / `SCL` | GPIO8 / GPIO9 | no | no |
| `FACTORY_BTN` | GPIO0 | **yes** — BOOT, deliberate | no |
| USB D−/D+ | GPIO19 / GPIO20 | no | fixed by silicon |

GPIO0 is a strapping pin by design: it is the BOOT button, read once at startup
for the factory reset. GPIO3, GPIO45 and GPIO46 are unused. GPIO26–32 are
consumed by the module's SPI flash and are not assigned.
`tools/test/test_pinmap.py` checks this against the schematic on every run.

## Still unproven on Rev A

Honest list. None of these can be closed without a board:

- VBUS actually rising and the UPS enumerating from a cold start.
- The power-cycle recovery working end to end against a wedged UPS.
- `FLG` asserting on a real overcurrent, and the 1.5 A limit behaving.
- The status LEDs.
- The OLED on I2C (optional, off by default).
- The CH340C auto-reset circuit, and flashing over the USB-C port.
- Antenna performance with the module overhanging the board edge.

## Bring-up order when boards arrive

1. Power only, no UPS. Expect green solid, then the provisioning AP.
2. Flash over USB-C — this exercises the CH340C and the auto-reset.
3. Attach the UPS. Expect VBUS to rise, enumeration, readings.
4. `POST /api/ups/reset` — the power-cycle path, now serialised into `ups_task`.
   Expect VBUS off for 2 s, re-enumeration, readings back, and **no** FAULT LED
   during the cycle.
5. Short the USB-A port's VBUS to ground briefly. Expect FAULT, and expect it
   to clear.
