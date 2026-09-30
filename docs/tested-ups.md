# Tested UPSes

Every unit here was read by the firmware on real hardware, and the evidence is
in `captures/`. Nothing is listed on the strength of "it should work": the
HID parsing is generic, so most USB HID Power Device units *should*, and that
is exactly the claim this page refuses to make on their behalf.

**Two vendors is the honest total.** It is enough to show the parser is not
written around one descriptor, and not enough to call anything broadly
supported.

## Tested

| UPS | USB ID | Descriptor | Fields | Status |
|---|---|---|---|---|
| APC Back-UPS RS 1000G | `051d:0002` | 1133 B | 86 | **Working**: full read, 16 plug cycles, a real transfer to battery and back |
| CyberPower EC850LCD | `0764:0501` | 496 B | 36 | **Working**: full read, all four control groups, the project's primary target |

Both are read by the same generic code path. Neither needed a vendor quirk.

### What each one reports

| NUT variable | APC RS 1000G | CyberPower EC850LCD |
|---|---|---|
| `battery.charge` | yes | yes |
| `battery.runtime` | yes | yes |
| `battery.charge.low` | yes | yes (writable) |
| `battery.voltage` | yes | **no**: `Voltage` is not in its Battery collection |
| `input.voltage` | yes | yes |
| `output.voltage` | - | yes |
| `input.frequency` | - | **no**: `Frequency` absent from the descriptor |
| `ups.load` | yes | yes |
| `ups.realpower.nominal` | yes | yes (510 W) |
| `ups.realpower` | estimated | **no** instantaneous value in the descriptor |
| `ups.status` | `OL`, `OB`, `DISCHRG`, `CHRG` observed | `OL`, `OL DISCHRG` (self-test) observed |
| `ups.beeper.status` | yes | yes |
| `ups.test.result` | yes | yes |
| `battery.date` | yes | **no**: `ManufacturerDate` (0x85,0x85) absent |

Commands offered are discovered from each descriptor, never assumed. The
EC850LCD offers quick test, deep test, stop test, beeper enable/disable/mute
and `load.off`; its `caps` word reads 15, i.e. TEST | BEEPER | LOWBATT |
SHUTDOWN with BATTDATE clear.

### What the commands actually do

Offering a command and honouring it are different things, so each one is
measured on the real unit rather than read off the descriptor.

| Command | APC RS 1000G | CyberPower EC850LCD |
|---|---|---|
| `test.battery.start.quick` | **works**: starts ~10 s after the command, ~8 s on battery, "Done and passed" | **works**: ~4 s on battery, then "Done and passed" |
| `test.battery.start.deep` | **works, but hides**: ran 40 min, 100 → 22 %, then passed; its USB reports froze at "on mains, 100 %, no test" until the USB was power-cycled | **runs the quick test instead**: ~4 s, "Done and passed", charge 100 → 99 % |
| `test.battery.stop` | **no visible effect**: sent mid quick test, which still ran its full ~8 s and passed | not yet measured |

The EC850LCD has a single test control (Output → Test, `0x84:0x58`) and no
runtime-calibration usage anywhere in its descriptor. Writing 2 ("deep") is
accepted and reported as a pass, but the unit runs its normal short self-test.
NUT's `cps-hid` writes the same value to the same control, so NUT on a host
gets the same result. A second EC850LCD, at another site and with 0 % load,
did the same on 2026-09-27: its deep test was over within about two minutes,
charge back at 100 % and "passed". Its descriptor is byte-identical to the
first unit's; it was not re-checked across a USB power cycle.

**The web page now says so.** A table in the page (`DEEP_KNOWN`) records what
each tested model does: on an EC850LCD the Deep test button is disabled with
the reason beside it; on the APC RS 1000G the confirm box says it takes about
40 minutes to ~22 % and that the reports may freeze. For any other UPS the page
watches the test: finished inside three minutes means "this UPS ran its short
self-test instead", and no sign of a start after 75 s means "press Power-cycle
UPS link to see what it is really doing". A newly measured model is one line in
that table. **There is no deep test on this model over USB.**

While testing, it keeps reporting mains present and raises only discharging:
`ups.status` reads `OL DISCHRG`, never `OB`, with `ups.test.result` "In
progress". Evidence: `captures/0764-0501-ec850lcd-selftest-20260927`.

**The APC's deep test is real; the CyberPower's is not.** The CyberPower was
checked three ways: its own reports, two USB power cycles forcing fresh reads,
and someone watching it return to mains after four seconds. CyberPower's own
documents agree: the EC850LCD manual, PowerPanel Personal and `pwrstat` offer
one self-test; "Runtime Calibration" appears only in PowerPanel Business
Edition, for the managed lines.

APC doesn't document a deep test on the RS 1000G either (manual and PowerChute
Personal: self-test only), but writing 2 to Test starts a real run-down, which
NUT's `apc-hid` and apcupsd both rely on. apcupsd clears the old result first
(abort, then 0), refuses below 100 % charge or 10 % load, and expects the UPS
to stop at about 25 %. Ours stopped at 22 % after about 40 minutes at 10 % load.

**The APC's reports froze during it.** For about 15 minutes the board, polling
fresh feature reports every cycle, was told "on mains, 100 %, No test
initiated" while the battery was draining. A USB power cycle from the page
(the VBUS switch) made it re-enumerate and report the truth: `OL DISCHRG`, "In
progress", 83 %. Nobody has published exactly this; the nearest are an RS 800
shown `OL` while visibly on battery during a deep test (nut-upsuser, 2009),
units reporting `OFF` briefly at the start of a test (NUT #2388, #2104), and a
BX1200MI whose charge stuck until the cable was replugged (NUT #2805). A real
outage during such a freeze would be reported as mains, so this matters beyond
testing. A user report, not APC, says the RS then refuses another deep test
until fully recharged.

**The USB standard does not say what "deep" means.** The HID Power Device
usage tables define Test's write values (0 none, 1 quick, 2 deep, 3 abort) and
read values (1 passed … 5 in progress, 6 no test initiated) and nothing more;
"calibration" appears nowhere in them. Every NUT HID subdriver writes 2 and
none implements `calibrate.start`. What a deep test does is the vendor's
choice, which is why the two units here differ.

**Status glitches around tests.** The EC850LCD reported no status bits at all
for about two minutes after a test and USB cycle, which this firmware's NUT
server shows as `OFF`. NUT had the same class of problem on APC RS units
(#2104: `OFF` at test start tripped `upsmon`), fixed by acting on `OFF` only
if it persists and by treating `OL DISCHRG` as calibration.

Evidence: `captures/051d-0002-selftest-20260927`,
`captures/0764-0501-ec850lcd-selftest-20260927`. Sources:
[HID Power Device usage tables](https://www.usb.org/sites/default/files/pdcv10.pdf),
[nut-upsuser 2009](https://alioth-lists.debian.net/pipermail/nut-upsuser/2009-October/005506.html),
[NUT #2104](https://github.com/networkupstools/nut/issues/2104),
[NUT #2388](https://github.com/networkupstools/nut/issues/2388),
[NUT #2805](https://github.com/networkupstools/nut/issues/2805),
[NUT #643](https://github.com/networkupstools/nut/issues/643),
[PowerPanel Personal for Windows](https://dl4jz3rbrsfum.cloudfront.net/documents/CyberPower_UM_PowerPanel-Personal-Windows-v2.4.6.pdf),
[EC850LCD manual](https://dl4jz3rbrsfum.cloudfront.net/documents/CyberPower_UM_EC850LCD.pdf).

### Battery replacement date

Worth stating plainly because it is a feature people look for: **the EC850LCD
cannot store one.** The usage is absent from its report descriptor, so there is
nowhere on the UPS to write it and nothing to read back. The firmware detects
this and does not offer the control. The APC does carry the field.

## Supported but not yet tested: Tripp Lite

Tripp Lite has two USB interfaces, and the firmware now handles both. **Neither
has been run against a real Tripp Lite yet**; both are ports of Network UPS
Tools, which does run on them.

- **Standard HID** (current models: AVR, ECO, INTERNET, OMNI, SMART, SMX, SU;
  USB ID `09ae:` + the protocol number, e.g. `2010`, `3015`). The generic
  parser reads these. `ups_profiles.c` adds NUT's `tripplite-hid.c` corrections:
  battery voltage x0.1 on protocols 1xxx-2xxx, the SMART1500LCDT scaling on
  3016/3024, a chemistry-string workaround on 1003 and 2005, and status flags
  moved back to the Battery System page where a unit (OMNI1000LCD) misplaces
  them -- without which such a unit would never be seen on battery.
- **Legacy protocol** (older OMNIVS, SMARTPRO, some SMART and INTERNETOFFICE
  revisions; USB ID `09ae:0001`). A vendor protocol, not HID Power Device: short
  commands by SET_REPORT, replies on the interrupt endpoint. `tripplite_legacy.c`
  ports NUT's `tripplite_usb.c` decoding for all six protocol variants (1001,
  2001, 3003, 0004, 3005, 3017), and turns the UPS's watchdog off at start-up as
  NUT does. It reports status, battery voltage, input and output voltage,
  frequency and load; **battery charge is estimated from voltage**, because
  these units do not report it. Controls (self-test, beeper, shutdown) are not
  ported. `tools/test/test_tripplite_legacy.py` checks the decoding against the
  example replies NUT documents from real units.

  Before this, a legacy unit parsed to zero fields, failed every poll, and sent
  the board into its VBUS recovery loop, power-cycling the UPS's USB port on a
  backoff indefinitely.

Not ported: NUT reads Tripp Lite's `UPS.PowerSummary.Voltage` as output voltage,
where APC uses the same path for battery voltage. That needs a capture to
settle, not a guess.

## Known vendor quirks

**CyberPower voltage scaling.** Several CPS models report `output.voltage` with
a bad exponent in the report descriptor, which NUT patches at runtime; the
symptom is `1230` or `12.3` where `123` is meant. The EC850LCD tested here did
**not** need it. It reported 120.0 V against a 120 V supply. The quirk hook
exists (`hid_apply_quirks`) and stays, because the next CPS model may need it.

## Untested, and what that means

Any other USB HID Power Device unit (Eaton, Tripp Lite, Liebert, PowerCOM, and
every other APC or CyberPower model) is **untested**, not unsupported. The
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
| `captures/051d-0002-esp32-m0b` | The same descriptor read by the ESP32: byte-for-byte identical |
| `captures/051d-0002-hotplug-20260919` | 16 plug/unplug cycles, heap logged on every one |
| `captures/051d-0002-m1-values-20260919` | Live values through a transfer to battery and back |
| `captures/0764-0501-ec850lcd-20260921` | CyberPower descriptor, decoded values and a NUT session |
| `captures/051d-0002-selftest-20260927` | APC quick, deep and stop: quick ~8 s; deep 40 min to 22 %, with 15 min of frozen reports |
| `captures/0764-0501-ec850lcd-selftest-20260927` | CyberPower quick and deep both ~4 s, deep re-checked across two USB power cycles |

`tools/test/test_parser.py` runs the parser against these descriptors on the
host, so a change that breaks either unit fails in CI rather than on a shelf.
