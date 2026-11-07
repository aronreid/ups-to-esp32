# Licensing

## Firmware

**GPL-3.0-or-later** ([`../LICENSE`](../LICENSE)); every source file carries an SPDX
header. Vendored third-party code keeps its own licence: see
[`../THIRD-PARTY-NOTICES.md`](../THIRD-PARTY-NOTICES.md). Copyleft on purpose: it keeps
derivatives open, and it is what allows reusing logic from NUT, whose `usbhid-ups` is
GPL-2.0-or-later.

## Hardware

**The Rev A design is CERN-OHL-S v2** ([`../hardware/LICENSE`](../hardware/LICENSE)).
Make and sell Rev A boards freely; publish your sources if you distribute a modified
version.

The firmware also builds for Rev B, the production board. Rev B's hardware design is not
published. If it is, it will be licensed CC BY-NC-SA 4.0: free to study, change and
build for yourself, with a commercial licence needed to sell boards made from it.

## Name and logo

"My UPS Pal" and its logo are trademarks and are not covered by either licence. They
identify boards made and tested by their maker: see
[`../TRADEMARKS.md`](../TRADEMARKS.md) for what you can and cannot do with them. Photos
of real boards are all rights reserved ([`img/README.md`](img/README.md)).

## Contributing

See [`../CONTRIBUTING.md`](../CONTRIBUTING.md). A pull request needs a sign-off, which
also lets the project offer the commercial licence above.

## Redistributed file

One third-party file is redistributed: `tools/test/jlc_cpl_rotations.csv`, a verbatim
copy of the reel-rotation table from
[JLCKicadTools](https://github.com/matthewlai/JLCKicadTools), GPL-3.0-or-later, with its
provenance in the file's own header.
