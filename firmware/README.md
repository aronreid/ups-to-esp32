# Firmware

ESP-IDF v5.x. See `../docs/firmware.md` for architecture.

## Build

```sh
. $IDF_PATH/export.sh
cd firmware
idf.py set-target esp32s3
idf.py build
```

Rev A is the only target. Application code contains no GPIO numbers -- the pin
map lives in `components/board` -- so a future revision is a new block there.

## Flash

Over the board's USB-C port. The CH340C's auto-reset enters the bootloader by
itself, so no button is needed:

```sh
idf.py -p /dev/cu.wchusbserial-XXXX flash monitor
```

After first flash, configuration is via the web UI — connect to the
`ups-esp32-XXXX` SoftAP and open `http://192.168.4.1/`.
