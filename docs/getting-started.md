# Getting started

## 1. Flash the board

Download `ups-adaptor-<board>-full.bin` from [Releases](../../../releases): `reva` for a
Rev A board, `reva-oled` for a Rev A with the display fitted, `revb` for Rev B. Write it
to offset 0 over the board's USB-C port with `esptool.py` (ESP-IDF's flashing tool):

```sh
esptool.py --chip esp32s3 write_flash 0x0 ups-adaptor-revb-full.bin
```

The board resets itself into the bootloader; no button is needed. A board that arrived
flashed skips this step.

## 2. Join it to your network

With no cable at all: join the `ups-esp32-XXXX` Wi-Fi access point the board raises,
open `http://192.168.4.1/`, and pick your network. The board joins it while the setup
page is still open and shows you the address it was given, plus
`http://ups-esp32-XXXX.local/`.

To move it to another network later, press BOOT five times within four seconds: it
forgets the old network and brings the setup access point back.

## 3. Set the login

Setup asks for a username and password with your Wi-Fi details. The page and every
change need it, including UPS commands over NUT (`upscmd -u`). Watching the UPS never
does: Home Assistant, a NAS and Prometheus read it with no setup.

The login is plain HTTP on Rev A: it keeps people out of the settings rather than
encrypting anything. Rev B can also serve its page over HTTPS. See
[`firmware.md`](firmware.md#the-login).

A board with no login set can be claimed by the first device on your network that asks.
That is the state of a board after the serial `clear-login` command, and of one updated
from a release that had no login. Set the login right away.

**Forgot it?** Type `clear-login` into the board's USB-C serial console (115200 baud):
that removes only the login. Or press BOOT five times, which clears Wi-Fi and the login
and starts the board over in setup.

## 4. Point a client at it

NUT clients: port 3493, UPS name `ups`, no credentials to read. Prometheus:
`http://ups-esp32-XXXX.local/metrics`. The README's *Works with* table has the settings
for Home Assistant, Synology, TrueNAS, unraid and `upsmon`.

## A fixed address

For a NAS or Home Assistant that points at the board's IP rather than its name, set a
fixed address in the setup page's "Fixed address (advanced)" or later on the board's
Address card. The board checks that your router answers from it; if not, it goes back to
an automatic address until it restarts and says so on its page, so a typo cannot lose it.

## Recovery

| Situation | What to do |
|---|---|
| Moved house or changed Wi-Fi | BOOT five times within four seconds: setup access point returns |
| Forgot the login | `clear-login` on the serial console, or BOOT five times |
| The page is unreachable after an update | wait: a new version that does not come up is rolled back by the board itself within minutes |
| Nothing responds at all | flash the `-full` image over USB (step 1); this erases the settings |
| Giving the board away | BOOT five times first: it clears the Wi-Fi details, the login, the HTTPS keys and, on Rev B, the Tailscale identity |

Updates, channels and signed images: [`updates.md`](updates.md).
