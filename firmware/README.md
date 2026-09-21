# Firmware

ESP-IDF v5.x. See `../docs/firmware.md` for architecture.

## Build

```sh
. $IDF_PATH/export.sh
cd firmware
idf.py set-target esp32s3
idf.py menuconfig          # ups-adaptor board -> pick your target
idf.py build
```

Select the target board under **ups-adaptor board**. It defaults to **Rev A**,
the reference target. Building for the Freenove devkit is an explicit choice:

```sh
idf.py menuconfig   # ups-adaptor board -> Freenove ESP32-S3-WROOM devkit
```

The devkit build compiles out the VBUS load switch and the status LEDs, because
that hardware only exists on Rev A. Application code contains no GPIO numbers,
so the pin map only ever changes here.

## Flash

On the Freenove devkit, use the **UART** USB-C port, not the **USB** one. Both
are wired to the same S3 and GPIO19/20 belong to the USB-A host pigtail.

```sh
idf.py -p /dev/cu.usbserial-XXXX flash monitor
```

After first flash, configuration is via the web UI — connect to the
`ups-adaptor-XXXX` SoftAP and open `http://192.168.4.1/`.
