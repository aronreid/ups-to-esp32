# ups-adaptor

Put a USB-only UPS on the network, without a computer next to it.

A small ESP32-S3 board that plugs into a UPS's USB port, speaks USB HID Power Device
to it, and serves the readings over Wi-Fi as a **NUT server on TCP 3493** — so Home
Assistant, Synology, TrueNAS, unraid and `upsmon` can monitor a UPS that has no network
card. Plus a web UI on port 80 for setup and for the readings.

![the board](docs/img/board-iso.png)

## Why

A UPS sited away from any computer has one monitoring interface: a USB type-B port.
Getting it into Home Assistant otherwise means parking a Raspberry Pi beside it, which
is a lot of computer for a protocol translator.

## What it does

- **NUT server on 3493.** The protocol, not an approximation of it: `LIST UPS`,
  `LIST VAR`, `LIST CMD`, `GET VAR`, `INSTCMD`, `SET VAR`, `LOGIN`, and NUT's own
  error vocabulary. Clients cannot tell it from `upsd`.
- **A web UI** with live readings, a battery gauge, a few minutes of history, and the
  UPS's controls — self-test, beeper, low-battery threshold.
- **Controls discovered, not assumed.** The firmware reads the UPS's report descriptor
  and offers only what that UPS actually implements. A model with no self-test usage
  gets no self-test button, on both the web UI and `LIST CMD`.
- **Wi-Fi setup in a browser.** First boot raises a `ups-adaptor-XXXX` access point
  with a captive portal. No app, no serial console.
- **Recovers by itself.** Wi-Fi reconnects with a capped backoff. VBUS to the UPS runs
  through a current-limited load switch on a GPIO, so firmware can power-cycle a UPS
  interface that has stopped answering.
- **mDNS** — `ups-adaptor.local`, advertising both `_http._tcp` and `_nut._tcp`.

Readings are generic. The parser walks the HID descriptor rather than matching a model,
so it should cover USB HID Power Device units from APC, CyberPower, Eaton, Tripp Lite,
Liebert and others. A field the UPS does not report is absent rather than zero: a NUT
client treats a variable it receives as authoritative.

## State of this project

**The firmware works and runs today** on an ESP32-S3 development board: it enumerates a
UPS, parses its descriptor, polls feature reports, serves the web UI and answers the NUT
protocol.

**No Rev A board has been fabricated yet.** The design passes every check in this
repository — 0 DRC violations, 0 unconnected pads, 0 floating grounds, every JLCPCB
fabrication and assembly limit — and the gerbers, BOM and placement files are generated
and committed. But nothing here has been built, and nothing in these documents claims
it has.

| | |
|---|---|
| Firmware: USB host, HID parsing, feature polling | working |
| Web UI: provisioning, captive portal, live status, controls | working |
| NUT server on 3493 | working |
| OTA firmware update | not started |
| MQTT with Home Assistant discovery | not started |
| Rev A board fabricated and brought up | not started |

## The board

36 × 58 mm, two layers, ESP32-S3-WROOM-1-N4.

- **USB-A host port** to the UPS, with switched VBUS (the UPS's port supplies none)
- **USB-C** for power, programming and console via a CH340C with auto-reset
- ESD protection on both ports, a 2 A resettable fuse
- Expansion header and an I²C header for an optional 128×64 OLED
- Every part on the top side, because JLCPCB's Economic PCBA places one side only

The board is **generated, not drawn**. `tools/` builds the schematic, placement,
routing, pours, silkscreen and fab outputs from source; `tools/build-pcb.sh` runs the
whole pipeline and `tools/pack-fab.sh` produces the files an order takes. Editing the
`.kicad_pcb` by hand means losing the edit on the next run.

## Getting started

Firmware build, flashing and the KiCad toolchain: [`docs/TOOLCHAIN.md`](docs/TOOLCHAIN.md).

```sh
cd firmware && idf.py build            # ESP-IDF v5.4
tools/test/run-all.sh                  # 19 checks, no hardware needed
tools/pack-fab.sh                      # the three files a PCB order takes
```

First boot: join the `ups-adaptor-XXXX` access point, open `http://192.168.4.1/`, pick
your network. The board restarts onto it and appears at `http://ups-adaptor.local/`.

Point a NUT client at port 3493 with UPS name `ups` and no credentials.

## Documentation

| | |
|---|---|
| [`docs/hardware.md`](docs/hardware.md) | board specification, pin map, power tree |
| [`docs/hardware-schematic.md`](docs/hardware-schematic.md) | every net and pin, as built |
| [`docs/firmware.md`](docs/firmware.md) | architecture, HID-to-NUT mapping, protocol subset |
| [`docs/jlcpcb.md`](docs/jlcpcb.md) | manufacturing limits, and what to check before ordering |
| [`docs/datasheet-audit.md`](docs/datasheet-audit.md) | every part against its datasheet |
| [`docs/BOM.md`](docs/BOM.md) | parts, with the substitutions that would destroy the board |
| [`docs/TOOLCHAIN.md`](docs/TOOLCHAIN.md) | what to install, and the traps |
| [`docs/RELEASING.md`](docs/RELEASING.md) | the rules for publishing firmware, and what shipped before they existed |

## Licence

**Firmware and tools: GPL-3.0-or-later** ([`LICENSE`](LICENSE)); every source file
carries an SPDX header. This is copyleft on purpose — it keeps derivatives open, and it
is what allows reusing logic from NUT, whose `usbhid-ups` is GPL-2.0-or-later.

**Hardware: CERN-OHL-S v2** ([`hardware/LICENSE`](hardware/LICENSE)). Make and sell the
board freely; publish your sources if you distribute a modified version.

One third-party file is redistributed: `tools/test/jlc_cpl_rotations.csv`, a verbatim
copy of the reel-rotation table from
[JLCKicadTools](https://github.com/matthewlai/JLCKicadTools), GPL-3.0-or-later, with its
provenance in the file's own header.

## How this was built

Most of this repository — firmware, the KiCad generation scripts, the test harness and
the documentation — was written by [Claude Code](https://claude.com/claude-code) against
real hardware, directed and reviewed by a human. That is stated here rather than left to
be inferred.

It is also why the test harness is the size it is. Every measured claim in these
documents comes from an instrument, a capture or a check you can re-run, and
`tools/test/run-all.sh` is how you verify the design rather than take its word for it.
