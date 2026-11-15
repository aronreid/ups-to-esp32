# My UPS Pal: Quick Install Guide

Five steps, about five minutes. You need a USB-connected UPS, a USB-C power supply
(any 5 V phone charger), and a phone or laptop with Wi-Fi.

## 1. Connect the UPS

Plug the UPS's USB cable into the board's **USB-A** port.

## 2. Power the board

Plug the USB-C supply into the board's **USB-C** port. Plug the supply into a
**battery-backed** outlet on the UPS. A board on unprotected mains goes dark at the
exact moment it has something to report.

## 3. Join your Wi-Fi

1. On your phone or laptop, join the Wi-Fi network named **`ups-esp32-XXXX`**.
2. Open **http://192.168.4.1/** if the setup page doesn't appear by itself.
3. Pick your home network, enter its password, and choose a **username and
   password** for the board.
4. The page shows the address the board was given. It is also reachable at
   `http://ups-esp32-XXXX.local/`.

## 4. Check the page

Open the board's address. You should see the battery gauge and live readings from
your UPS. Reading never asks for a login; changing settings or sending UPS commands does.

## 5. Point your software at it

NUT port **3493**, UPS name **`ups`**, no credentials to read.

| Client | Where |
|---|---|
| **Home Assistant** | Add the **NUT** integration (often found automatically). Host `ups-esp32-XXXX.local`. |
| **Synology** | Control Panel → Hardware & Power → UPS → *Network UPS server*. |
| **TrueNAS / unraid** | UPS service in network ("slave") mode. |
| **Linux** | `upsc ups@ups-esp32-XXXX.local` |
| **Prometheus** | Scrape `http://ups-esp32-XXXX.local/metrics`. |

## If something's wrong

| Problem | Try |
|---|---|
| No readings | Reseat the UPS's USB cable. The board recovers on its own, but check the UPS is switched on. |
| Moved house or changed your router | Press **BOOT five times within four seconds**. The board forgets its network and brings the setup network back. |
| Forgot the login | Press BOOT five times (also clears Wi-Fi), or type `clear-login` into the USB-C serial console at 115200. |
| Board not flashed | Download `ups-adaptor-reva-full.bin` from [Releases](../../../releases) and run `esptool.py --chip esp32s3 write_flash 0x0 ups-adaptor-reva-full.bin` over the USB-C port. |
| UPS not supported | See [tested-ups.md](tested-ups.md). The web page has a "send my UPS info to the developer" button. |

Updates are offered on the board's page and never install without you pressing Install.

More detail: [README](../README.md) · [firmware notes](firmware.md)
