# Firmware updates

A board updates itself over the network from this repository's
[Releases](../../../releases). No cable, no computer beside it.

## What a release contains

Every release carries one application image per board, the matching full
image for a first flash over USB, and `SHA256SUMS.txt`:

| File | Board | Use |
|---|---|---|
| `ups-adaptor-reva.bin` | Rev A | what a Rev A board downloads when it updates |
| `ups-adaptor-reva-oled.bin` | Rev A with the OLED fitted | the same, for that build |
| `ups-adaptor-revb.bin` | Rev B | what a Rev B board downloads when it updates |
| `ups-adaptor-*-full.bin` | each board | bootloader, partition table and application, for a first flash at offset 0 |

Each image names its board in its header, and a board refuses an image that
is not its own.

## How an update happens

1. The board checks the release list once every twelve hours. The Firmware
   section of its page shows the newest release for its channel, with the
   release notes, and a **Check for updates** button checks at once.
2. **Install** downloads the image into the board's spare application slot,
   checks its signature and its board name, and asks for a restart. The UPS is
   read throughout; nothing is interrupted until the restart.
3. After the restart the new firmware runs on trial. Once it has stayed up for
   five minutes and answered a few requests, it is kept. If it does not come
   back, the bootloader starts the previous firmware again. Nothing is lost
   either way: the Wi-Fi details, login and settings live outside the
   application slots.

A board that is updated from a release that had no login must have one set
right after the update; see the README.

## Signed updates

Release images are signed with the project's key, and a board running a
release checks the signature of an update before installing it. An image that
is unsigned, signed with another key, or changed after it was signed is
refused: the page says so, and the board keeps running what it has. The check
is in the firmware, not burned into the chip, so a USB cable can always flash
anything.

If you build your own firmware:

- A build without `sdkconfig.defaults.signed` accepts updates by board name
  only. The board stays fully yours.
- A board running one of the project's releases accepts only the project's
  signed updates. To run your own build on it, flash it over USB.
- Installing a project release on a board that ran your own build switches it
  to accepting only signed updates, until it is flashed by cable again.

## Channels

Which release a board offers depends on its channel. The channel is set per
board by the project's fleet server when the board checks in; a board that
has never checked in is on `production`.

| Channel | Offers | Installs |
|---|---|---|
| `production` | full releases only, never a release candidate | when you press Install |
| `debug` | the release named for that board, otherwise the latest full release | by itself |
| `dev` | the newest release of any kind, including release candidates | by itself |

Boards sold to customers are on `production`. The other two channels exist so
that release candidates run on real hardware before a release is made, and a
board is placed on them only on purpose.

## Release candidates

A release candidate (`vX.Y-rcN`) is a pre-release. Production boards never
see one. Candidates are removed from the release list once the release they
led to is published, so the list stays a list of releases.

## Flashing over USB

For a first flash, or to recover a board, write the full image at offset 0
with `esptool.py` (ESP-IDF's flashing tool):

```sh
esptool.py --chip esp32s3 write_flash 0x0 ups-adaptor-reva-full.bin
```

This erases the board's settings. To update an already set-up board by cable
without losing them, write the application image alone at `0x20000`; see
[`../firmware/README.md`](../firmware/README.md).
