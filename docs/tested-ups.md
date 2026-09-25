# Tested UPSes

Every unit here was read by the firmware on real hardware, and the evidence is
in `captures/`. Nothing is listed on the strength of "it should work" — the
HID parsing is generic, so most USB HID Power Device units *should*, and that
is exactly the claim this page refuses to make on their behalf.

**Two vendors is the honest total.** It is enough to show the parser is not
written around one descriptor, and not enough to call anything broadly
supported.

## Tested

| UPS | USB ID | Descriptor | Fields | Status |
|---|---|---|---|---|
| APC Back-UPS RS 1000G | `051d:0002` | 1133 B | 86 | **Working** — full read, 16 plug cycles, a real transfer to battery and back |
| CyberPower EC850LCD | `0764:0501` | 496 B | 36 | **Working** — full read, all four control groups, the project's primary target |

Both are read by the same generic code path. Neither needed a vendor quirk.

### What each one reports

| NUT variable | APC RS 1000G | CyberPower EC850LCD |
|---|---|---|
| `battery.charge` | yes | yes |
| `battery.runtime` | yes | yes |
| `battery.charge.low` | yes | yes (writable) |
| `battery.voltage` | yes | **no** — `Voltage` is not in its Battery collection |
| `input.voltage` | yes | yes |
| `output.voltage` | — | yes |
| `input.frequency` | — | **no** — `Frequency` absent from the descriptor |
| `ups.load` | yes | yes |
| `ups.realpower.nominal` | yes | yes (510 W) |
| `ups.realpower` | estimated | **no** instantaneous value in the descriptor |
| `ups.status` | `OL`, `OB`, `DISCHRG`, `CHRG` observed | `OL` observed |
| `ups.beeper.status` | yes | yes |
| `ups.test.result` | yes | yes |
| `battery.date` | yes | **no** — `ManufacturerDate` (0x85,0x85) absent |

Commands offered are discovered from each descriptor, never assumed. The
EC850LCD offers quick test, deep test, stop test, beeper enable/disable/mute
and `load.off`; its `caps` word reads 15, i.e. TEST | BEEPER | LOWBATT |
SHUTDOWN with BATTDATE clear.

### Battery replacement date

Worth stating plainly because it is a feature people look for: **the EC850LCD
cannot store one.** The usage is absent from its report descriptor, so there is
nowhere on the UPS to write it and nothing to read back. The firmware detects
this and does not offer the control. The APC does carry the field.

## Known vendor quirks

**CyberPower voltage scaling.** Several CPS models report `output.voltage` with
a bad exponent in the report descriptor, which NUT patches at runtime; the
symptom is `1230` or `12.3` where `123` is meant. The EC850LCD tested here did
**not** need it — it reported 120.0 V against a 120 V supply. The quirk hook
exists (`hid_apply_quirks`) and stays, because the next CPS model may need it.

## Untested, and what that means

Any other USB HID Power Device unit — Eaton, Tripp Lite, Liebert, PowerCOM, and
every other APC or CyberPower model — is **untested**, not unsupported. The
parser reads the descriptor rather than matching a model, so the likely outcome
is that it works and reports whatever that unit exposes.

If you have one, `GET /api/descriptor` returns the raw report descriptor
exactly as the device sent it, and `tools/decode-hid.py` reads the same file.
That capture is what makes a bug report actionable, and it is the same input
the firmware saw.

## Evidence

| Capture | What it shows |
|---|---|
| `captures/051d-0002-20260917-125739` | APC descriptor read on a Mac, decoded, with the NUT mapping |
| `captures/051d-0002-esp32-m0b` | The same descriptor read by the ESP32 — byte-for-byte identical |
| `captures/051d-0002-hotplug-20260919` | 16 plug/unplug cycles, heap logged on every one |
| `captures/051d-0002-m1-values-20260919` | Live values through a transfer to battery and back |
| `captures/0764-0501-ec850lcd-20260921` | CyberPower descriptor, decoded values and a NUT session |

`tools/test/test_parser.py` runs the parser against these descriptors on the
host, so a change that breaks either unit fails in CI rather than on a shelf.
