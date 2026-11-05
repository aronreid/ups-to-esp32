# Running the firmware on an ESP32-S3 dev board

The firmware also builds for a generic ESP32-S3 dev board, so you can try it on a
board you already have. You build and flash it yourself. These builds are not
released and do not update over the air: to get a newer version, pull the code and
build again.

## What you need

- **An ESP32-S3 board** that brings out GPIO19 and GPIO20, the S3's native USB
  pins. Most S3 boards with two USB ports have one marked "USB" or "OTG" (native)
  and one marked "UART" or "COM" (the serial chip). The UPS goes on the native one.
  A classic ESP32, a C3 or a C6 has no USB host and cannot do this.
- **OTG enabled** Not all dev boards enable OTG on the native USB port by default and
  require a jumper or solder bridge to enable.
  - For example, the [AITRIP 3PCS ESP32-S3-DevKitC-1-N16R8](https://www.amazon.com/dp/B0CGYXJB6Y)
    requires a solder bridge on the underside of the board.
- **A USB connector on those pins** to connect to the UPS. There are several ways to do this:
  - A USB-A port via OTG adapter or a cable wired to the ESP32's native USB port, then use a
    standard USB-A to USB-B cable to the UPS
  - Using a dev board with a USB-C port, with a [USB-C to USB-B cable](https://www.amazon.com/dp/B0F2YGZ68S)
    to connect to the UPS.
- **5 V going out to the UPS on that socket.** The UPS's USB port takes no power of
  its own and never connects without it. Many dev boards do not put 5 V on their
  native port by default. Check your board's schematic: some have a solder jumper
  for it, others need 5 V wired to the socket's VBUS pin.

## Pick the build

| Your board | Build | What you get |
|---|---|---|
| Any S3, with or without PSRAM | `dev-s3` | NUT, the web page, Home Assistant, Prometheus, battery tests |
| An S3 on an N16R8 module (16 MB flash, 8 MB octal PSRAM) | `dev-s3-psram` | All of the above, plus Tailscale, HTTPS, the outage log and long history |

The module name is printed on its metal shield, for example `ESP32-S3-WROOM-1-N16R8`.
An N8R8 or N8R2 takes the `dev-s3` build.

## Build

Install ESP-IDF 5.5.5 as described in [`../firmware/README.md`](../firmware/README.md), then:

```sh
cd firmware

# dev-s3
idf.py -B build-dev-s3 -DSDKCONFIG=build-dev-s3/sdkconfig \
    -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.dev-s3" \
    -DUPSA_IMAGE_NAME=ups-adaptor-dev-s3 build

# dev-s3-psram
idf.py -B build-dev-s3-psram -DSDKCONFIG=build-dev-s3-psram/sdkconfig \
    -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.revb;sdkconfig.defaults.dev-s3-psram;sdkconfig.defaults.tailscale" \
    -DUPSA_IMAGE_NAME=ups-adaptor-dev-s3-psram build
```

Keep the defaults files in that order. The PSRAM build uses the Rev B settings and
then changes only what differs on a dev board.

## Flash

Flash over the board's serial port (the "UART" or "COM" one), not the port the UPS
uses:

```sh
idf.py -B build-dev-s3 -p /dev/cu.usbserial-XXXX flash monitor
```

On first boot the board starts its setup network. Join it and open
`http://192.168.4.1/` to enter your Wi-Fi details and a login, the same as on a
My UPS Pal board.

## What is different from a My UPS Pal board

- **No switch on the UPS's USB power.** When a UPS stops answering, the board
  closes the connection and connects to it again. A My UPS Pal board can also
  switch the UPS's USB power off and on, which clears more stuck UPSes. If yours
  stays stuck, unplug its USB cable and plug it back in.
- **No status lights and no display.** The web page shows everything.
- **No updates over the air.** The page's update check finds no image for this
  board, because none is published.
- **Tested less.** Every release is tested on Rev A and Rev B boards. The dev board
  builds are checked to compile, and run on the same code, but not every release is
  tried on a dev board.

## Check-in

Like a My UPS Pal board, a dev board sends a short check-in by default: a hardware
ID, which build it runs (`ups-adaptor-dev-s3` or `ups-adaptor-dev-s3-psram`), the
firmware version, uptime and why it last restarted. It tells the project how many
people use these builds, which decides how much time goes into them. One click on
the page turns it off. See [`privacy.md`](privacy.md).

If it does not find your UPS, open an issue with the board you used and the UPS
model.
