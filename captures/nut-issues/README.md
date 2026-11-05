# Report descriptors from NUT's issue tracker

Real USB HID report descriptors from UPSes other people own, reassembled from
`usbhid-ups` debug logs posted publicly on
[networkupstools/nut](https://github.com/networkupstools/nut) issues. Each
`.hex` file is the descriptor's bytes, in hex. `INDEX.json` records where each
one came from, the device it belongs to, how it was extracted, and its length.
Every file was checked: its byte count matches the length the log states, and
its collections open and close in balance. One (`051d-0003-smart-ups-srt3000`)
had no stated length and is kept on structure alone.

No hardware here: these are what the firmware's parser is tested against, so a
change that stops a real UPS being read fails a test instead of reaching
someone's board. `tools/test/test_nut_descriptors.py` runs them.

Six are 27-byte vendor-page descriptors, not HID Power Devices: those UPSes
speak a serial protocol tunnelled through HID, which this board does not read.
