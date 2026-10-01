# Tested UPSes

This page sorts UPSes into three groups by the evidence behind them, and says
which is which:

- **Tested on hardware.** A unit on the bench, read by the board, with the
  evidence in `captures/`. Three models.
- **Checked against its real descriptor.** A report descriptor from that
  model, published by its owner on the Network UPS Tools issue tracker, read by
  the board's own parser in the test suite: charge, runtime and on-mains /
  on-battery status are all found, and every value NUT itself decoded from the
  same UPS's logs matches ours. Strong evidence, not a bench test. 56 of 65.
- **Not supported.** UPSes that do not speak USB HID at all, but a serial
  protocol carried over USB. The board cannot read these.

Anything not listed is untested. Most USB HID UPSes should work, because the
board reads each UPS's own description of itself rather than a list of models.
If yours does not, the page's "Send my UPS info to the developer" button sends
exactly what is needed to fix it.

## Tested

| UPS | USB ID | Descriptor | Fields | Status |
|---|---|---|---|---|
| APC Back-UPS RS 1000G | `051d:0002` | 1133 B | 86 | **Working**: full read, 16 plug cycles, a real transfer to battery and back |
| CyberPower EC850LCD | `0764:0501` | 496 B | 36 | **Working**: full read, all four control groups, the project's primary target |
| APC Back-UPS ES 600M1 (BE600M1) | `051d:0002` | 1049 B | 86 | **Working from v0.12**: full read, a load change, a real transfer to battery and back. Before v0.12 it restarted the board every 1.5 s (it answers a feature request with a longer packet than asked for) |

All three are read by the same generic code path, with no model-specific code.

### What each one reports

| NUT variable | APC RS 1000G | CyberPower EC850LCD |
|---|---|---|
| `battery.charge` | yes | yes |
| `battery.runtime` | yes | yes |
| `battery.charge.low` | yes | yes (writable) |
| `battery.voltage` | yes | **from v0.14.3**: read from `UPS.PowerSummary.Voltage`, as NUT does; not yet re-checked on this unit |
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

**CyberPower scaling.** This page used to say several CyberPowers report
`output.voltage` ten times wrong and that NUT patches it. Reading NUT's
`cps-hid.c` shows otherwise: NUT has no output-voltage correction. What it does
have, and what the board now does the same way from v0.14.3, applies only to
USB ID `0764:0501`: battery voltage is scaled by 2/3 when the first reading is
more than 1.4 times the nominal, and frequency by 0.1 when it reads in tenths of
a hertz. Battery charge is capped at 100 %. The EC850LCD tested here needed
none of it: 120.0 V against a 120 V supply.

## Checked against its real descriptor

Each row is a report descriptor from one model, published by its owner in a
Network UPS Tools issue (linked), and checked on every change by
`tools/test/test_nut_descriptors.py`, `test_vars.py` and `test_vectors.py`, the
last of which compares every value NUT decoded from that UPS's own logs with
the board's (none disagree). Not tested on hardware: the board has never been
plugged into one of these.

| USB ID | Model | Extra variables | Status logic | Source |
|---|---|---|---|---|
| `0463:ffff` | 5E | 17 | NUT's | [issue](https://github.com/networkupstools/nut/issues/515) |
| `0463:ffff` | Eaton (ups.model reported as 'unknown 2000') | 47 | NUT's | [issue](https://github.com/networkupstools/nut/pull/2562) |
| `0463:ffff` | Eaton 3S 700 | 21 | NUT's | [issue](https://github.com/networkupstools/nut/issues/2887#issuecomment-3764113336) |
| `0463:ffff` | Eaton 5SC 1500i R | 42 | NUT's | [issue](https://github.com/networkupstools/nut/issues/1513) |
| `0463:ffff` | Eaton 5SC 750 | 42 | NUT's | [issue](https://github.com/networkupstools/nut/issues/2380#issuecomment-2108986084) |
| `0463:ffff` | Eaton 9PX 2200GRT-L | 25 | NUT's | [issue](https://github.com/networkupstools/nut/issues/3295) |
| `0463:ffff` | Protection Station Protection Station | 23 | NUT's | [issue](https://github.com/networkupstools/nut/issues/1286) |
| `0463:ffff` | Ellipse ECO | 23 | NUT's | [issue](https://github.com/networkupstools/nut/issues/1507#issuecomment-1229544307) |
| `04b3:0001` | IBM 1000VA/750W Tower UPS | 44 | NUT's | [issue](https://github.com/networkupstools/nut/issues/778#issuecomment-819955904) |
| `051d:0002` | Back-UPS 500 | 10 | NUT's | [issue](https://github.com/networkupstools/nut/issues/1776#issuecomment-1373586808) |
| `051d:0002` | Back-UPS BX1600MI | 20 | NUT's | [issue](https://github.com/networkupstools/nut/issues/1208) |
| `051d:0002` | Back-UPS ES 850G2 | 21 | NUT's | [issue](https://github.com/networkupstools/nut/issues/1304) |
| `051d:0002` | Back-UPS ES 850M2 | 21 | NUT's | [issue](https://github.com/networkupstools/nut/issues/2996) |
| `051d:0002` | Back-UPS XS 1400U | 21 | NUT's | [issue](https://github.com/networkupstools/nut/issues/1189) |
| `051d:0002` | Back-UPS XS 1500M | 21 | NUT's | [issue](https://github.com/networkupstools/nut/issues/2827#issuecomment-2693258810) |
| `051d:0002` | Smart-UPS 1000 | 24 | NUT's | [issue](https://github.com/networkupstools/nut/issues/2441) |
| `051d:0002` | Smart-UPS 1500 RM | 25 | NUT's | [issue](https://github.com/networkupstools/nut/issues/2695#issuecomment-2501915179) |
| `051d:0003` | Smart-UPS 3000 (product Smart-UPS_3000) | 13 | NUT's | [issue](https://github.com/networkupstools/nut/issues/2954) |
| `051d:0003` | Smart-UPS SRT 3000 RMXL (title: APC SRT3000RMX.. | 13 | NUT's | [issue](https://github.com/networkupstools/nut/issues/1072) |
| `051d:0003` | Smart-UPS X 750 | 13 | NUT's | [issue](https://github.com/networkupstools/nut/issues/1261#issuecomment-1038451781) |
| `05dd:a011` | ED2000RM2U | 26 | NUT's | [issue](https://github.com/networkupstools/nut/issues/3152) |
| `05dd:a0a0` | MINUTEMAN UPS | 17 | NUT's | [issue](https://github.com/networkupstools/nut/pull/1848#issuecomment-1429053267) |
| `0665:5161` | Legrand Keor DK 3k | 12 | fixed | [issue](https://github.com/networkupstools/nut/issues/3702) |
| `06da:ffff` | PowerWalker VFI 2000 RT HID (product HID UPS I.. | 47 | NUT's | [issue](https://github.com/networkupstools/nut/issues/1114) |
| `06da:ffff` | PowerWalker VI 1200 SHL / VI 2200 SHL (product.. | 15 | NUT's | [issue](https://github.com/networkupstools/nut/issues/1270) |
| `06da:ffff` | PowerWalker VI 3000 RT HID (product HID UPS) | 43 | NUT's | [issue](https://github.com/networkupstools/nut/issues/1824#issuecomment-1402393035) |
| `075d:0300` | Smart-Battery | 14 | NUT's | [issue](https://github.com/networkupstools/nut/issues/3015#issuecomment-3155681144) |
| `0764:0501` | CP 1500C | 21 | NUT's | [issue](https://github.com/networkupstools/nut/issues/2997) |
| `0764:0501` | CP1300EPFCLCD | 22 | NUT's | [issue](https://github.com/networkupstools/nut/pull/1245) |
| `0764:0501` | CP1500AVRLCDa | 25 | NUT's | [issue](https://github.com/networkupstools/nut/issues/439#issuecomment-1011420900) |
| `0764:0501` | CP1500PFCLCD | 22 | NUT's | [issue](https://github.com/networkupstools/nut/issues/2642#issuecomment-2378074359) |
| `0764:0501` | CP900EPFCLCD | 22 | NUT's | [issue](https://github.com/networkupstools/nut/issues/437#issuecomment-1238306455) |
| `0764:0501` | ST Series | 22 | NUT's | [issue](https://github.com/networkupstools/nut/issues/2439#issuecomment-2111210355) |
| `0764:0501` | VP1600ELCD | 22 | NUT's | [issue](https://github.com/networkupstools/nut/issues/3557) |
| `0764:0601` | CP1000PFCLCDa | 25 | NUT's | [issue](https://github.com/networkupstools/nut/issues/439) |
| `0764:0601` | CPS 0764:0601 unit reporting product ' 600' (m.. | 23 | NUT's | [issue](https://github.com/networkupstools/nut/pull/2718) |
| `0764:0601` | OL1000EXL | 23 | NUT's | [issue](https://github.com/networkupstools/nut/issues/982) |
| `0764:0601` | OR2200LCDRT2U | 22 | NUT's | [issue](https://github.com/networkupstools/nut/issues/2791#issuecomment-2622693904) |
| `0764:0601` | OR600ERM1U | 25 | NUT's | [issue](https://github.com/networkupstools/nut/issues/2728) |
| `0764:0601` | PowerWalker VI 500 R1U (CPS OEM, product 500R) | 23 | NUT's | [issue](https://github.com/networkupstools/nut/issues/1512#issuecomment-2199747873) |
| `0764:0601` | PowerWalker VI 750 (CPS OEM, product 750R) | 23 | NUT's | [issue](https://github.com/networkupstools/nut/issues/645) |
| `0764:0601` | PR3000ELCDSL | 24 | NUT's | [issue](https://github.com/networkupstools/nut/issues/3089) |
| `09ae:3016` | TRIPP LITE UPS (USB product string; model not .. | 19 | NUT's | [issue](https://github.com/networkupstools/nut/issues/414) |
| `09ae:4003` | TRIPP LITE UPS (USB product string; model not .. | 26 | NUT's | [issue](https://github.com/networkupstools/nut/issues/1268) |
| `0d9f:0004` | HID UPS Battery | 17 | NUT's | [issue](https://github.com/networkupstools/nut/issues/1695) |
| `10af:0002` | Liebert PSA5 | 13 | NUT's | [issue](https://github.com/networkupstools/nut/issues/2816) |
| `10af:0002` | Liebert PSI5 | 13 | NUT's | [issue](https://github.com/networkupstools/nut/issues/2271#issuecomment-1976540478) |
| `10af:1000` | GXT5 | 5 | NUT's | [issue](https://github.com/networkupstools/nut/issues/3613) |
| `1cb0:0032` | Legrand UPS | 20 | NUT's | [issue](https://github.com/networkupstools/nut/issues/3540) |
| `1cb0:0038` | Legrand KEOR PDU 800 | 24 | NUT's | [issue](https://github.com/networkupstools/nut/pull/1138) |
| `2e66:0201` | SLC TWIN PRO2<=3KVA | 18 | NUT's | [issue](https://github.com/networkupstools/nut/issues/1312#issuecomment-1055098137) |

"Extra variables" counts the NUT variables from NUT's own table for that make
that the board found in the descriptor. "Status logic" is NUT's where that
table reports the power state, else the board's own.

The other 9 of the 65: five are not UPS descriptors (two serial tunnels, two
vendor-only interfaces, one corrupted capture), and four are UPSes whose
descriptor lacks one of charge, runtime or status as a readable value (a
Liebert GXT4, a 33-byte Eaton Ellipse ECO capture, a `09d6:0001` unit with
runtime only as an interrupt report, and `5131:2007`).

## Not supported: serial over USB

Many inexpensive UPSes, and some larger ones, use a USB-to-serial bridge and a
text protocol (Megatec/Q1 and its relatives, Voltronic, Riello, Richcomm,
Powerware's BCM/XCP) rather than USB HID. NUT reads them with `nutdrv_qx`,
`blazer_usb`, `riello_usb`, `richcomm_usb` and `bcmxcp_usb`; this board does
not speak those protocols. NUT knows 22 USB IDs of this kind, among them
`0665:5161`, `06da:0002` to `06da:0005`, `06da:0201`, `06da:0601`,
`05b8:0000`, `0f03:0001`, `14f0:00c9`, `1a86:7523`, `0925:1234`, `04b4:5500`,
`03f0:1f01`, `03f0:1f02` and `0592:0002`.

Two IDs are shared: `0001:0000` and `0665:5161` are used both by serial units
and by real HID UPSes (a Belkin-family unit and a Legrand Keor DK 3k above).
The board decides by what the UPS says about itself, not by its ID. Tripp
Lite's older `09ae:0001` protocol is not HID either, but the board supports
it (below).

**APC Smart-UPS with Modbus.** Some Smart-UPS units on `051d:0003` can also be
switched to APC's Modbus protocol from the front panel, which NUT reads with
`apc_modbus` and which gives more than their HID interface. This board reads
their HID interface: charge, runtime and status, and whatever else the model
puts there.

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
