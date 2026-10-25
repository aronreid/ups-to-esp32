# Building the firmware

The firmware builds with **ESP-IDF 5.5.5** for both boards. `CMakeLists.txt`
refuses any other ESP-IDF line, so a build on 5.4 or 6.x stops at configure
with a message rather than producing an image that differs from a release.

## Install ESP-IDF

```sh
git clone --branch v5.5.5 --depth 1 --recursive --shallow-submodules \
    https://github.com/espressif/esp-idf.git ~/esp/esp-idf
~/esp/esp-idf/install.sh esp32s3
. ~/esp/esp-idf/export.sh        # in every shell, before idf.py
```

`export.sh` uses whichever `python3` is first on `PATH`, and expects the
version `install.sh` ran with. If it reports that its virtual environment is
not found, put that Python version first on `PATH` and run it again.

## Choose a board

The board is a build-time choice made with `SDKCONFIG_DEFAULTS`. Each target
gets its own build directory, because ESP-IDF reads the defaults only when it
first generates `sdkconfig`; reusing one directory for two boards builds the
second with the first board's settings.

| Board | `SDKCONFIG_DEFAULTS` | Image name |
|---|---|---|
| Rev A | `sdkconfig.defaults` | `ups-adaptor-reva` |
| Rev A with the OLED fitted | `sdkconfig.defaults;sdkconfig.defaults.reva-oled` | `ups-adaptor-reva-oled` |
| Rev B | `sdkconfig.defaults;sdkconfig.defaults.revb;sdkconfig.defaults.tailscale` | `ups-adaptor-revb` |

```sh
cd firmware

# Rev A
idf.py -B build-reva -DSDKCONFIG=build-reva/sdkconfig \
    -DSDKCONFIG_DEFAULTS="sdkconfig.defaults" \
    -DUPSA_IMAGE_NAME=ups-adaptor-reva build

# Rev B
idf.py -B build-revb -DSDKCONFIG=build-revb/sdkconfig \
    -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.revb;sdkconfig.defaults.tailscale" \
    -DUPSA_IMAGE_NAME=ups-adaptor-revb build
```

`UPSA_IMAGE_NAME` is written into the image header. A board installs an
update only if that name matches its own, so a Rev A board never takes a Rev B
image. The pin map and everything else that differs between the boards lives
in `components/board`; application code contains no GPIO numbers.

Two more overlays exist:

- `sdkconfig.defaults.signed` compiles in the signed-update check that release
  images carry. A build with it refuses updates that are not signed with the
  same key it was signed with, and an image built with it must itself be signed
  before it runs. Leave it out for your own builds unless you manage your own
  key; see [`../docs/updates.md`](../docs/updates.md).
- `sdkconfig.defaults.tailscale` adds Tailscale. It is part of every Rev B
  build and does not fit on Rev A.

## Flash and watch the console

Over the board's USB-C port. The serial adapter's auto-reset enters the
bootloader by itself, so no button is needed:

```sh
idf.py -B build-reva -p /dev/cu.wchusbserial-XXXX -b 460800 flash monitor
```

To update a board that is already set up without losing its Wi-Fi details and
login, write only the application:

```sh
esptool.py --chip esp32s3 -p /dev/cu.wchusbserial-XXXX -b 460800 \
    write_flash 0x20000 build-reva/ups-adaptor-reva.bin
```

The console runs at 115200 baud. Opening the port does not reset the board;
to see the log from power-on, pulse RTS (EN) low and high.

After the first flash, everything else is set up in a browser: join the
`ups-esp32-XXXX` access point and open `http://192.168.4.1/`.

## Where things are

| Path | What it holds |
|---|---|
| `main/` | start-up and the task layout |
| `components/board` | per-board pin maps and board names |
| `components/ups_hid` | the USB host and HID Power Device reader |
| `components/nut_server` | the NUT protocol server on TCP 3493 |
| `components/webui` | the web page and its JSON API |
| `components/netmgr` | Wi-Fi, the setup access point, the fixed address |
| `components/ota` | update checks, download, verification and rollback |
| `components/auth`, `tlscert` | the login and the board's HTTPS certificate |
| `components/history`, `outagelog` | Rev B's history and outage log |
| `components/tailnet`, `microlink`, `wireguard_lwip` | Tailscale (Rev B) |
| `components/display` | the optional OLED |
| `components/fleet` | the heartbeat check-in |

Architecture and behaviour are described in
[`../docs/firmware.md`](../docs/firmware.md).
