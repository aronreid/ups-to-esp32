# UPS compatibility

My UPS Pal reads a UPS through the USB HID Power Device class, the same
interface Network UPS Tools (NUT) uses, and it reads each UPS's own description
of itself rather than a list of models. Compatibility is established at three
levels: **verified on hardware** (a unit plugged into the board, with the
captures in `captures/`), **verified against the real descriptor** (a report
descriptor published for that model on the NUT issue tracker, read by the
board's parser, with every value NUT decoded from the same unit matching the
board's), and **expected to work** because the UPS is a standard USB HID Power
Device. Anything not listed below is in the third group: most USB HID UPSes
work, and if yours does not, the page's "Send my UPS info to the developer"
button sends exactly what is needed to add it.

## Verified on hardware

| UPS | USB ID | What works |
|---|---|---|
| CyberPower EC850LCD | `0764:0501` | Full read, all four control groups (test, beeper, low-battery threshold, shutdown); `LB` raised on a real run-down |
| APC Back-UPS ES 600M1 (BE600M1) | `051d:0002` | Full read, load changes, a real transfer to battery and back |
| APC Back-UPS RS 1000G | `051d:0002` | Full read, repeated plug cycles, a real transfer to battery and back, a real deep test |

All three go through the same generic code path, with no model-specific code.
Tripp Lite is supported by the parser (standard HID models on `09ae:` plus the
protocol number, and the legacy `09ae:0001` protocol, which reports no charge,
so charge is estimated from battery voltage, and has no controls) and is not
yet verified on hardware.

### What each reports

| NUT variable | APC RS 1000G | CyberPower EC850LCD |
|---|---|---|
| `battery.charge` | yes | yes |
| `battery.runtime` | yes | yes |
| `battery.charge.low` | yes | yes, writable |
| `battery.voltage` | yes | yes, from `UPS.PowerSummary.Voltage` as NUT reads it |
| `input.voltage` | yes | yes |
| `output.voltage` | no | yes |
| `input.frequency` | no | no: `Frequency` is absent from the descriptor |
| `ups.load` | yes | yes |
| `ups.realpower.nominal` | yes | yes (510 W) |
| `ups.realpower` | estimated | no: the descriptor has no instantaneous value |
| `ups.status` | `OL`, `OB`, `CHRG`, `DISCHRG` | `OL`; `OL DISCHRG` during a self-test |
| `ups.beeper.status` | yes | yes |
| `ups.test.result` | yes | yes |
| `battery.date` | yes | no: `ManufacturerDate` is absent, so the UPS cannot store one |

Commands are discovered from each descriptor, never assumed. The EC850LCD
offers quick test, deep test, stop test, beeper enable, disable and mute, and
`load.off`.

### What the commands do

| Command | APC RS 1000G | CyberPower EC850LCD |
|---|---|---|
| `test.battery.start.quick` | works: about 8 s on battery, then "Done and passed" | works: about 4 s on battery, then "Done and passed" |
| `test.battery.start.deep` | works, but hides its progress: a real run-down to about 22 %, during which the USB reports can freeze at "on mains, 100 %" until the USB link is power-cycled | runs the quick test instead: the unit has no deep test over USB |
| `test.battery.stop` | no visible effect: a running test completes anyway | not verified |
| `beeper.enable` / `beeper.disable` | work | work |

**Quick test** (`test.battery.start.quick`) works on all three. The APC RS
1000G starts about 10 s after the command and spends about 8 s on battery; the
EC850LCD spends about 4 s on battery. Both finish with "Done and passed".

**Deep test** (`test.battery.start.deep`) is where the vendors differ. The USB
HID Power Device standard defines the write value for "deep" and nothing about
what it does, so each vendor decides.

- On the **APC RS 1000G** it is a real run-down: the UPS discharges to about
  22 % charge, which takes about 40 minutes at 10 % load, then reports a pass.
  During the run-down the UPS's USB reports can freeze at "on mains, 100 %, no
  test initiated" while the battery is draining; the Power-cycle USB control on
  the page makes it re-enumerate and report the truth (`OL DISCHRG`, "In
  progress", the real charge). A real outage during such a freeze would be
  reported as mains. The page's confirm box says all of this before it starts.
- On the **CyberPower EC850LCD** there is no deep test over USB. The unit
  accepts the command and reports a pass, but runs its normal short self-test,
  with `ups.status` reading `OL DISCHRG`, never `OB`. The UPS has one test
  control and no runtime-calibration usage in its descriptor; NUT's `cps-hid`
  writes the same value to the same control and gets the same result, and
  CyberPower's own software offers one self-test on this line. The page
  disables the Deep test button on this model and says why.
- On **any other UPS** the page watches the test: a test that finishes inside
  three minutes is reported as "this UPS ran its short self-test instead", and
  no sign of a start after 75 s prompts a USB power cycle to see what the UPS
  is really doing.

**Stop test** (`test.battery.stop`) has no visible effect on the APC units: the
UPS accepts the command and the quick test runs to completion and passes. On
the EC850LCD the quick test is about 4 s long and the stop command is not
verified.

**Beeper** controls (enable, disable, mute) and **shutdown** (`load.off`) are
offered where the descriptor carries them; the EC850LCD carries all of them.

**Status around tests.** The EC850LCD can report no status bits at all for
about two minutes after a test followed by a USB power cycle, which the NUT
server shows as `OFF`. NUT had the same class of problem on APC Back-UPS RS
units (NUT issue 2104), fixed there by acting on `OFF` only when it persists.

**Battery replacement date.** The EC850LCD cannot store one: the usage is
absent from its report descriptor, so there is nowhere to write it and nothing
to read back. The firmware detects this and does not offer the control. The
APC RS 1000G carries the field.

Sources: the
[HID Power Device usage tables](https://www.usb.org/sites/default/files/pdcv10.pdf),
[NUT issue 2104](https://github.com/networkupstools/nut/issues/2104),
[NUT issue 2388](https://github.com/networkupstools/nut/issues/2388),
[NUT issue 2805](https://github.com/networkupstools/nut/issues/2805),
[PowerPanel Personal for Windows](https://dl4jz3rbrsfum.cloudfront.net/documents/CyberPower_UM_PowerPanel-Personal-Windows-v2.4.6.pdf) and the
[EC850LCD manual](https://dl4jz3rbrsfum.cloudfront.net/documents/CyberPower_UM_EC850LCD.pdf).

## Verified against the real descriptor

Each model below has a report descriptor published by its owner on the NUT
issue tracker. The board's parser finds charge, runtime and on-mains /
on-battery status in every one, and every value NUT decoded from that unit's
own logs matches the board's. This is strong evidence, short of a hardware
test: the board has not been plugged into one of these. 57 of the 66
descriptors examined pass (the table lists each model once, so repeated
captures of one model share a row); the other 9 are either not UPS descriptors (two
serial tunnels, two vendor-only interfaces, one corrupted capture) or lack one
of charge, runtime or status as a readable value (a Liebert GXT4, a 33-byte
Eaton Ellipse ECO capture, a `09d6:0001` unit with runtime only as an interrupt
report, and `5131:2007`).

| Vendor | USB ID | Model | Source |
|---|---|---|---|
| Eaton | `0463:ffff` | 5E | [NUT issue 515](https://github.com/networkupstools/nut/issues/515) |
| Eaton | `0463:ffff` | Eaton, model reported as "unknown 2000" | [NUT pull request 2562](https://github.com/networkupstools/nut/pull/2562) |
| Eaton | `0463:ffff` | 3S 700 | [NUT issue 2887](https://github.com/networkupstools/nut/issues/2887#issuecomment-3764113336) |
| Eaton | `0463:ffff` | 5SC 1500i R | [NUT issue 1513](https://github.com/networkupstools/nut/issues/1513) |
| Eaton | `0463:ffff` | 5SC 750 | [NUT issue 2380](https://github.com/networkupstools/nut/issues/2380#issuecomment-2108986084) |
| Eaton | `0463:ffff` | 9PX 2200GRT-L | [NUT issue 3295](https://github.com/networkupstools/nut/issues/3295) |
| Eaton | `0463:ffff` | Protection Station | [NUT issue 1286](https://github.com/networkupstools/nut/issues/1286) |
| Eaton | `0463:ffff` | Ellipse ECO | [NUT issue 1507](https://github.com/networkupstools/nut/issues/1507#issuecomment-1229544307) |
| IBM | `04b3:0001` | 1000VA/750W Tower UPS | [NUT issue 778](https://github.com/networkupstools/nut/issues/778#issuecomment-819955904) |
| APC | `051d:0002` | Back-UPS 500 | [NUT issue 1776](https://github.com/networkupstools/nut/issues/1776#issuecomment-1373586808) |
| APC | `051d:0002` | Back-UPS BX1600MI | [NUT issue 1208](https://github.com/networkupstools/nut/issues/1208) |
| APC | `051d:0002` | Back-UPS ES 850G2 | [NUT issue 1304](https://github.com/networkupstools/nut/issues/1304) |
| APC | `051d:0002` | Back-UPS ES 850M2 | [NUT issue 2996](https://github.com/networkupstools/nut/issues/2996) |
| APC | `051d:0002` | Back-UPS XS 1400U | [NUT issue 1189](https://github.com/networkupstools/nut/issues/1189) |
| APC | `051d:0002` | Back-UPS XS 1500M | [NUT issue 2827](https://github.com/networkupstools/nut/issues/2827#issuecomment-2693258810) |
| APC | `051d:0002` | Smart-UPS 1000 | [NUT issue 2441](https://github.com/networkupstools/nut/issues/2441) |
| APC | `051d:0002` | Smart-UPS 1500 RM | [NUT issue 2695](https://github.com/networkupstools/nut/issues/2695#issuecomment-2501915179) |
| APC | `051d:0003` | Smart-UPS 3000 | [NUT issue 2954](https://github.com/networkupstools/nut/issues/2954) |
| APC | `051d:0003` | Smart-UPS SRT 3000 RMXL | [NUT issue 1072](https://github.com/networkupstools/nut/issues/1072) |
| APC | `051d:0003` | Smart-UPS X 750 | [NUT issue 1261](https://github.com/networkupstools/nut/issues/1261#issuecomment-1038451781) |
| Minuteman | `05dd:a011` | ED2000RM2U | [NUT issue 3152](https://github.com/networkupstools/nut/issues/3152) |
| Minuteman | `05dd:a0a0` | MINUTEMAN UPS | [NUT pull request 1848](https://github.com/networkupstools/nut/pull/1848#issuecomment-1429053267) |
| Legrand | `0665:5161` | Keor DK 3k | [NUT issue 3702](https://github.com/networkupstools/nut/issues/3702) |
| Legrand | `1cb0:0032` | Legrand UPS | [NUT issue 3540](https://github.com/networkupstools/nut/issues/3540) |
| Legrand | `1cb0:0038` | KEOR PDU 800 | [NUT pull request 1138](https://github.com/networkupstools/nut/pull/1138) |
| PowerWalker | `06da:ffff` | VFI 2000 RT HID | [NUT issue 1114](https://github.com/networkupstools/nut/issues/1114) |
| PowerWalker | `06da:ffff` | VI 1200 SHL / VI 2200 SHL | [NUT issue 1270](https://github.com/networkupstools/nut/issues/1270) |
| PowerWalker | `06da:ffff` | VI 3000 RT HID | [NUT issue 1824](https://github.com/networkupstools/nut/issues/1824#issuecomment-1402393035) |
| PowerWalker (CyberPower OEM) | `0764:0601` | VI 500 R1U | [NUT issue 1512](https://github.com/networkupstools/nut/issues/1512#issuecomment-2199747873) |
| PowerWalker (CyberPower OEM) | `0764:0601` | VI 750 | [NUT issue 645](https://github.com/networkupstools/nut/issues/645) |
| CyberPower | `0764:0501` | CP 1500C | [NUT issue 2997](https://github.com/networkupstools/nut/issues/2997) |
| CyberPower | `0764:0501` | CP1300EPFCLCD | [NUT pull request 1245](https://github.com/networkupstools/nut/pull/1245) |
| CyberPower | `0764:0501` | CP1500AVRLCDa | [NUT issue 439](https://github.com/networkupstools/nut/issues/439#issuecomment-1011420900) |
| CyberPower | `0764:0501` | CP1500PFCLCD | [NUT issue 2642](https://github.com/networkupstools/nut/issues/2642#issuecomment-2378074359) |
| CyberPower | `0764:0501` | CP900EPFCLCD | [NUT issue 437](https://github.com/networkupstools/nut/issues/437#issuecomment-1238306455) |
| CyberPower | `0764:0501` | ST Series | [NUT issue 2439](https://github.com/networkupstools/nut/issues/2439#issuecomment-2111210355) |
| CyberPower | `0764:0501` | VP1600ELCD | [NUT issue 3557](https://github.com/networkupstools/nut/issues/3557) |
| CyberPower | `0764:0601` | CP1000PFCLCDa | [NUT issue 439](https://github.com/networkupstools/nut/issues/439) |
| CyberPower | `0764:0601` | Unit reporting product " 600" | [NUT pull request 2718](https://github.com/networkupstools/nut/pull/2718) |
| CyberPower | `0764:0601` | OL1000EXL | [NUT issue 982](https://github.com/networkupstools/nut/issues/982) |
| CyberPower | `0764:0601` | OR2200LCDRT2U | [NUT issue 2791](https://github.com/networkupstools/nut/issues/2791#issuecomment-2622693904) |
| CyberPower | `0764:0601` | OR600ERM1U | [NUT issue 2728](https://github.com/networkupstools/nut/issues/2728) |
| CyberPower | `0764:0601` | PR3000ELCDSL | [NUT issue 3089](https://github.com/networkupstools/nut/issues/3089) |
| Tripp Lite | `09ae:3016` | Tripp Lite UPS, model not named | [NUT issue 414](https://github.com/networkupstools/nut/issues/414) |
| Tripp Lite | `09ae:4003` | Tripp Lite UPS, model not named | [NUT issue 1268](https://github.com/networkupstools/nut/issues/1268) |
| Liebert | `10af:0002` | PSA5 | [NUT issue 2816](https://github.com/networkupstools/nut/issues/2816) |
| Liebert | `10af:0002` | PSI5 | [NUT issue 2271](https://github.com/networkupstools/nut/issues/2271#issuecomment-1976540478) |
| Liebert | `10af:1000` | GXT5 | [NUT issue 3613](https://github.com/networkupstools/nut/issues/3613) |
| Other | `075d:0300` | Smart-Battery | [NUT issue 3015](https://github.com/networkupstools/nut/issues/3015#issuecomment-3155681144) |
| Other | `0d9f:0004` | HID UPS Battery | [NUT issue 1695](https://github.com/networkupstools/nut/issues/1695) |
| Other | `2e66:0201` | SLC TWIN PRO2 (3 kVA and below) | [NUT issue 1312](https://github.com/networkupstools/nut/issues/1312#issuecomment-1055098137) |

## Known vendor quirks

**CyberPower scaling (`0764:0501`).** The board applies the same corrections as
NUT's `cps-hid`, and only on this USB ID: battery voltage is scaled by 2/3
when the first reading is more than 1.4 times the nominal, and frequency by
0.1 when it reads in tenths of a hertz. Battery charge is capped at 100 %. NUT
has no output-voltage correction for CyberPower, and neither does the board;
the EC850LCD needs none of these corrections.

**CyberPower EC850LCD missing variables.** No input frequency, no
instantaneous real power and no battery replacement date: the usages are
absent from the descriptor. No deep test over USB (see above).

**APC Back-UPS ES 600M1 over-long reply.** The unit answers a feature request
with a longer packet than asked for. The firmware handles it.

**APC Back-UPS RS 1000G frozen reports.** During a deep test the UPS can keep
reporting "on mains, 100 %, no test" until its USB port is power-cycled (see
above).

**APC Smart-UPS with Modbus (`051d:0003`).** Some Smart-UPS units report only
charge, runtime and status over HID; their voltages, load and power are in
APC's Modbus register map, which travels over USB in two vendor reports on the
same HID interface. Where a UPS's descriptor has them, the board reads the
same registers NUT's `apc_modbus` does and publishes `input.voltage`,
`output.voltage`, `output.current`, `output.frequency`, `ups.load`,
`ups.realpower`, `ups.power`, their nominal ratings, `battery.voltage`,
`battery.temperature` and `ups.efficiency`. HID stays the source of status and
of any value HID also reports. The UPS answers only once Modbus is enabled in
its front-panel menu (Configuration, Modbus, Enable); until then the board asks
again every five minutes. Not verified on hardware.

**Tripp Lite HID corrections.** The board carries NUT's `tripplite-hid`
corrections: battery voltage scaled by 0.1 on protocols 1xxx to 2xxx, the
SMART1500LCDT scaling on protocols 3016 and 3024, a battery chemistry string
workaround on 1003 and 2005, and status flags read from the Battery System
page where a unit (the OMNI1000LCD) places them, without which that unit would
never be seen on battery.

## Not supported

**Serial over USB.** Many inexpensive UPSes, and some larger ones, use a
USB-to-serial bridge and a text protocol (Megatec/Q1 and its relatives,
Voltronic, Riello, Richcomm, Powerware's BCM/XCP) rather than USB HID. NUT reads
them with `nutdrv_qx`, `blazer_usb`, `riello_usb`, `richcomm_usb` and
`bcmxcp_usb`; the board does not speak those protocols. NUT knows 22 USB IDs of
this kind, among them `0665:5161`, `06da:0002` to `06da:0005`, `06da:0201`,
`06da:0601`, `05b8:0000`, `0f03:0001`, `14f0:00c9`, `1a86:7523`, `0925:1234`,
`04b4:5500`, `03f0:1f01`, `03f0:1f02` and `0592:0002`.

Two IDs are shared: `0001:0000` and `0665:5161` are used both by serial units
and by real HID UPSes (a Belkin-family unit and the Legrand Keor DK 3k above).
The board decides by what the UPS says about itself, not by its ID. Tripp
Lite's older `09ae:0001` protocol is not HID either, but the board supports it
as described above.
