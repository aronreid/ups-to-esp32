# UPS to ESP32 Module <img src="docs/img/flag-ca.svg" height="22" alt="Canadian flag">

[![Licence: GPL-3.0-or-later](https://img.shields.io/badge/firmware-GPL--3.0--or--later-blue)](LICENSE)
[![Latest release](https://img.shields.io/github/v/release/aronreid/ups-to-esp32?include_prereleases&label=latest%20release)](../../releases)
[![Hardware: CERN-OHL-S v2](https://img.shields.io/badge/hardware-CERN--OHL--S%20v2-blue)](hardware/LICENSE)

**Put a USB-only UPS on your network for the cost of a $6 board: no Pi, no SD card to
corrupt, no vendor network card that costs more than the UPS.**

Plug it into your UPS's USB port and it speaks the real Network UPS Tools protocol on
TCP 3493, so Home Assistant, Synology, TrueNAS, unraid and `upsmon` see a normal `upsd`
and connect exactly like they would to one, because it *is* one, not an approximation.

<p align="center">
  <img src="docs/img/reva-board.jpg" width="300" alt="A Rev A board, powered and running, with a UPS cable in its USB-A port">
  &nbsp;&nbsp;
  <img src="docs/img/reva-case.jpg" width="420" alt="The same board in its printed case">
</p>

---

## The problem

A UPS in a rack, a basement or a cupboard has exactly one monitoring interface: a USB
type-B socket. The usual fix is a Raspberry Pi parked beside it running NUT: an entire
Linux computer, its own power draw, its own SD card to corrupt, to do nothing but
translate one protocol. Vendor network cards cost more than the UPS. Most small UPSes
can't take one at all.

This board is the translator, and nothing else. ~$6 in parts, no computer, no card.

---

## NUT-compatible

Speaks the real NUT network protocol on TCP 3493. A client can't tell it from `upsd`,
because there's nothing to tell apart. It reads whatever the UPS's own USB descriptor
reports (charge, runtime, voltage, load, standard status flags) and offers only the
commands that UPS actually has: self-test, beeper, shutdown. Never a button for
something the UPS can't do.

| Client | Configuration |
|---|---|
| **Home Assistant** | Add the built-in **NUT** integration. Usually discovered on its own; otherwise host `ups-esp32-XXXX.local`, port `3493`, UPS name `ups`, no credentials. |
| **Synology DSM** | Control Panel → Hardware & Power → UPS → *Network UPS server*, point at the board's address. |
| **TrueNAS / unraid** | UPS service in "slave"/network mode, same address and port. |
| **Linux** | `upsmon` / `upsc ups@ups-esp32-XXXX.local` |
| **Prometheus** | Scrape `http://ups-esp32-XXXX.local/metrics` directly, no exporter in between. Metric names follow `nut_exporter`, so its dashboards and alerts carry over. |

MQTT with Home Assistant discovery is planned as a secondary output; NUT is primary.

---

## The web UI

![the status page](docs/img/ui-desktop.png)

Everything the UPS reports, in words rather than protocol vocabulary ("Running on mains
power", not `OL`), with the raw NUT status alongside it for anyone who wants it. The NUT
connection string sits at the top, ready to paste into a client. The controls underneath
are the ones *this* UPS actually implements, and the destructive one asks you to type
`OFF` before it will arm.

*Shown with sample data rather than a real installation.*

---

## What else you get

- **A web UI**: battery gauge, live readings, the UPS's own controls, and the NUT
  connection string ready to paste into a client.
- **Wi-Fi setup in a browser, no app.** First boot raises an access point with a
  captive portal; afterward it's at `ups-esp32-XXXX.local`.
- **Updates itself over the air, safely.** Nothing installs without a click, and a bad
  update rolls itself back automatically.
- **An optional OLED**: charge, runtime, volts and load at a glance, no browser needed.
  **On Rev A, read the warning under *The board* before fitting one.**
- **Recovers on its own** from a Wi-Fi drop or a UPS that stops answering. Nobody has
  to go babysit it.

Readings only ever come from what the UPS itself reports. A value it doesn't expose
shows up absent, never guessed at as zero.

---

## The board: Rev A

36 × 58 mm, two layers, ESP32-S3-WROOM-1-N4.

![top view](docs/img/board-top.png)

- **USB-A host port** to the UPS, with switched, current-limited VBUS: a UPS's USB port
  supplies no power itself, so the board provides it
- **USB-C** for power, programming and console via a CH340C with auto-reset
- ESD protection on both ports, a 2 A resettable fuse
- Expansion header, and an I²C header for an optional 128×64 SSD1306 OLED
- Every part on the top side, because JLCPCB's Economic PCBA places one side only

The photos at the top are a real Rev A board, running, and the same board in its
printed case. The views here are renders of the design.

### The case

Printable, no supports, no screws needed to close it: the board drops into the tray, and
the cover snaps on and holds it down. STLs are in
[`hardware/enclosure/reva/`](hardware/enclosure/reva/). Rev A's case has no name label
to drop, so there is no `-noname` variant.

| File | What it is |
|---|---|
| `ups-adaptor-tray.stl` | the tray, for sitting on a shelf |
| `ups-adaptor-tray-wall.stl` | **wall-mount tray**: two countersunk holes for #6 / 3.5 mm flat-head wood screws, 23 mm apart on the centreline. Screw the empty tray to the wall, drop the board in, snap the cover on: the screws end up hidden inside |
| `ups-adaptor-cover.stl` | the cover, with light pipes over the LEDs and pinholes for RESET and BOOT |
| `ups-adaptor-lightpipes.stl` | two 3 mm light pipes for the plain STL covers above, printed in clear PETG and pressed into the light pipe guides |
| `ups-adaptor-cover-3color.3mf` | the plain cover as a three-colour Bambu Studio / Orca project: STATUS, FAULT, RESET and BOOT inlaid into the top in filament 2, and the light pipes printed straight into the case in filament 3, no rod to press in. Just a thin plug right inside each light pipe's chamfered opening, printed in the first layers against the bed; the guide tube stays open below it down to the LED, so it reads as a lit window rather than a full clear rod |
| `ups-adaptor-cover-blank.stl`, `ups-adaptor-cover-oled-blank.stl` | the same covers with no words at all |

There is also an OLED cover, with a window for a display on J3, but it isn't published here. The display is disabled in firmware on every board shipped so far (wired to the wrong pins), so there is nothing to fit it to yet.

Both trays carry "Contains FCC ID: 2AC7Z-ESPS3WROOM1" and "Contains IC: 21098-ESPS3WROOM1" engraved underneath: the Wi-Fi module's certification requires them on the outside of anything it is enclosed in. They are not a compliance claim for the board itself.

Either tray takes either cover. Print the cover upside down, as the STL is oriented; the labels are then the first layers, so a multi-colour print changes filament only there.

### Schematic

[![the schematic, click for the PDF](docs/img/schematic.svg)](docs/schematic.pdf)

[PDF](docs/schematic.pdf) · [SVG](docs/img/schematic.svg) · KiCad source in
[`hardware/kicad/`](hardware/kicad/). Generated from the same netlist the board is built
from, so it cannot drift from the hardware; CI fails if it has changed without being
re-exported.

### With the optional display fitted

![the board with an OLED fitted](docs/img/board-oled.png)

The I²C header is positioned so a 0.96" module sits **flat over the board** rather
than hanging off an edge. Illustrative: the 3D model is an Adafruit breakout standing
in for the generic modules most people buy.

> [!CAUTION]
> **Rev A: never plug a standard OLED module straight into J3. It will be destroyed.**
> J3 reads **3V3, GND, SDA, SCL** from pin 1 (the square pad). Nearly every 0.96" SSD1306
> module reads **GND, VCC, SCL, SDA**, so seated directly it gets 3V3 on its ground pin.
> The first module plugged into a Rev A board died this way.
>
> **To use a display on Rev A, do one of these:**
>
> 1. **Buy a VCC-first module**: 0.96", 4-pin I²C, SSD1306, with the pins labelled
>    **VCC, GND, SCL, SDA** in that order. Its power pins match J3, so it plugs straight in.
>    The DIYmalls 0.96" module is one; check the photo of the pin labels before ordering,
>    because listings reuse stock photos.
> 2. **Or cross the power pins on a standard module**: wire it with four jumpers:
>    module **VCC → J3 pin 1**, module **GND → J3 pin 2**, SCL → pin 3, SDA → pin 4.
>    Only the power pair crosses.
>
> Either way, flash the **`reva-oled`** build (`ups-adaptor-reva-oled-full.bin`
> for a first flash over USB, or let an already-flashed board update itself
> from then on): it swaps SDA and SCL for the display in software to match.
> The plain Rev A release leaves the display off, on purpose, because it
> cannot know which way round your module is wired. **Before first power,
> read the labels on the module in your hand and put VCC on J3's square pad.**

The board is **generated, not drawn.** The schematic, placement, routing, pours,
silkscreen and fab outputs all come from source, not a hand-edited `.kicad_pcb`.

---

## Getting started

**Flash a board**: download `ups-adaptor-<target>-full.bin` from
[Releases](../../releases) and write it to offset 0:

```sh
esptool.py --chip esp32s3 write_flash 0x0 ups-adaptor-reva-full.bin
```

**ESP32-P4 on PoE (test build).** A second target runs on the Waveshare
ESP32-P4-ETH development board, which is a bought board rather than ours. One
Ethernet cable from a PoE switch powers it and carries the network. It has no
Wi-Fi, so there is no setup step: it takes an address by DHCP and answers at
`http://ups-esp32-XXXX.local/`, with the same web page and NUT server. Download
`ups-adaptor-p4poe-full.bin` and write it to offset 0:

```sh
esptool.py --chip esp32p4 write_flash 0x0 ups-adaptor-p4poe-full.bin
```

It needs ESP32-P4 silicon v1.x, which is what these boards carry. The UPS
plugs into the board's small 4-pin USB header (1.25mm pitch, marked V, D-, D+,
G) through a cable to a USB-A socket. The USB-C port on the board is only the
serial port for flashing and cannot talk to a UPS. That header's 5V is always
on, so unlike Rev A this target cannot power-cycle the UPS's USB. It has been
run on the bench with no UPS attached so far, so treat it as a test build and
tell us what you see.

**Then, with no cable at all:** join the `ups-esp32-XXXX` access point, open
`http://192.168.4.1/`, pick your network. The board joins it while the setup page
is still open and shows you the address it was given, plus `http://ups-esp32-XXXX.local/`. To move it
to another network later, press BOOT five times within four seconds: it forgets the old
one and brings the setup access point back. Point a NUT client at port 3493, UPS name `ups`; reading needs no
credentials.

**A login is required.** Setup asks for a username and password with your Wi-Fi details,
and the page and every change need it, including UPS commands over NUT (`upscmd -u`).
Watching the UPS never does: Home Assistant, a NAS and Prometheus read it with no setup.
Forgot it? Type `clear-login` into the board's USB-C serial console (115200): that removes
only the login. Or press BOOT five times, which clears Wi-Fi and the login and starts it over
in setup. It is plain HTTP, so it keeps people out of the settings rather than encrypting
anything; see [docs/firmware.md](docs/firmware.md#the-login).

A **fixed address**, for a NAS or Home Assistant that points at the board's IP, can be set
in the setup page's "Fixed address (advanced)" or later on the board's Address card. The board
checks that your router answers from it; if not, it goes back to an automatic address until it
restarts and says so on its page, so a typo cannot lose it.

**Build the firmware from source**, ESP-IDF v5.4:

```sh
cd firmware && idf.py build
```

---

## Privacy

Private by design: your UPS's serial number, your Wi-Fi name and any address on your
network never leave the board, and the server never keeps your IP. A short check-in
(hardware ID, firmware version, uptime, why it last restarted) is on by default so the
board can be supported if something goes wrong. The page says so, and one click turns
it off. It never slows the board down: lowest priority, backs off if the server is
slow or unreachable, and behaves exactly the same either way.

**If your UPS doesn't work**, the board helps get it fixed. When a UPS is attached but the
board can't read it for five minutes, and the check-in is on, it sends that UPS's USB
report descriptor once: the description of how that model talks, plus its make, model and
USB ID. No readings, no serial number, nothing about your network. The web page also has a
"send my UPS info to the developer" button that sends the same thing whenever you press it,
even with the check-in off. Turn the check-in off and nothing is sent unprompted.

**Want to help with debugging further?** Anonymous stats are opt-in: tick the box and
it also shares the UPS's make/model, its readings, Wi-Fi signal and free memory, nothing
that identifies you. Untick it and what was kept gets deleted. The database side is
[`telemetry/supabase.sql`](telemetry/supabase.sql), so you can see exactly what's stored.

## Documentation

| | |
|---|---|
| [`docs/tested-ups.md`](docs/tested-ups.md) | which UPSes are tested on hardware, which are checked against their real descriptors, and which cannot work |
| [`docs/firmware.md`](docs/firmware.md) | architecture, HID-to-NUT mapping, the protocol subset, OTA |
| [`docs/hardware.md`](docs/hardware.md) | board specification, pin map, power tree |
| [`docs/schematic.pdf`](docs/schematic.pdf) | the schematic, for reading without KiCad |
| [`docs/hardware-schematic.md`](docs/hardware-schematic.md) | every net and pin, as built |
| [`docs/circuit.md`](docs/circuit.md) | the one-page circuit description |
| [`docs/BOM.md`](docs/BOM.md) | parts, with the substitutions that would destroy the board |
| [`docs/datasheet-audit.md`](docs/datasheet-audit.md) | every part checked against its datasheet, and what that found |
| [`docs/jlcpcb.md`](docs/jlcpcb.md) | manufacturing limits, and what to check before ordering |
| [`docs/reva-readiness.md`](docs/reva-readiness.md) | every way the real board differs from the development one, and the bring-up order |
| [`docs/RELEASING.md`](docs/RELEASING.md) | the rules for shipping firmware, and what each one cost to learn |
| [`docs/CHANGELOG.md`](docs/CHANGELOG.md) | what changed, written as it happens rather than reconstructed at release time |
| [`docs/TOOLCHAIN.md`](docs/TOOLCHAIN.md) | what to install, and the traps |

---

## What's built

Rev A boards are built, running, and reading real UPSes today.

- USB HID Power Device polling, generic across manufacturers
- NUT server on TCP 3493
- Prometheus `/metrics`
- Web UI with live readings and the UPS's own controls
- Wi-Fi setup in a browser, no app
- OTA firmware updates, with automatic rollback
- Optional OLED display

---

## Licence

**Firmware and tools: GPL-3.0-or-later** ([`LICENSE`](LICENSE)); every source file
carries an SPDX header. Copyleft on purpose: it keeps derivatives open, and it is what
allows reusing logic from NUT, whose `usbhid-ups` is GPL-2.0-or-later.

**Hardware: CERN-OHL-S v2** ([`hardware/LICENSE`](hardware/LICENSE)). Make and sell the
board freely; publish your sources if you distribute a modified version.

One third-party file is redistributed: `tools/test/jlc_cpl_rotations.csv`, a verbatim
copy of the reel-rotation table from
[JLCKicadTools](https://github.com/matthewlai/JLCKicadTools), GPL-3.0-or-later, with its
provenance in the file's own header.

## How this was built

The idea and the direction are the author's, who also designed and routed key parts of
the board by hand. [Claude Code](https://claude.com/claude-code) helped finish the
hardware and wrote most of the firmware, tooling and docs, working against real hardware
and reviewed as it went.

---

<p align="center"><img src="docs/img/flag-ca.svg" height="14" alt="">&nbsp; <b>Designed in Canada</b></p>
