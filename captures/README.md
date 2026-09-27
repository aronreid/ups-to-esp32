# Reference captures

A real UPS's USB HID report descriptor, decoded, with the NUT variable mapping
derived from it. Kept because the firmware's parser is tested against it — see
`tools/test/test_parser.py` and `tools/test/test_descriptor.py` — and because a
worked example of a Power Device descriptor is hard to come by.

| File | What it is |
|---|---|
| `report-descriptor.bin` | the descriptor exactly as the device returns it |
| `report-descriptor.hex` | the same bytes, annotated |
| `decoded.txt` | every field: report ID, bit offset, width, units, scaling |
| `usages.txt` | the HID usages present, by page |
| `SUMMARY.txt` | what the unit is and what it reports |

The device is an APC Back-UPS RS 1000G, USB `051d:0002`. Its serial number is
replaced with `REDACTED-SERIAL`; it identified one specific unit and proved
nothing the VID, PID and model string do not.

Host-side `ioreg` and `system_profiler` dumps are deliberately not committed —
they carry the capturing machine's own serial numbers and account details.
`tools/capture-ups.sh` writes them locally and `.gitignore` keeps them there.
