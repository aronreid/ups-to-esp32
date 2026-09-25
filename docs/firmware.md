# ups-adaptor firmware architecture

## Design rules

1. **The web interface is the primary UI.** Setup, status, diagnostics and
 firmware update all happen in a browser. No app, no serial console required
 for normal use, no phone pairing. The OLED is optional and off by default.
2. **No GPIO numbers outside `components/board`.** Rev A is the reference
 target and the default. The Freenove devkit is a development fallback that
 lets firmware run before boards are fabricated; its missing hardware is a
 fact to compile around, never a design constraint.
3. **Absent is not zero.** A UPS that does not report a usage must produce
 `null` in JSON and no `VAR` line in NUT, never `0.0`.
4. **Never require a physical reset.** Wi-Fi dropouts, UPS unplugs and UPS
 firmware wedges are all recovered in software.
5. **If the OLED is fitted, it shows everything the LEDs show.** On Rev A the
 module sits directly over both LEDs, so the screen is the only local
 indicator on a board that has one. Every `board_status_t` state gets a line
 on the screen, and every state that lights the red LED is drawn in reverse.
 Enforced twice: `status_text()` in `display.c` switches with no default, so a
 missing state is a `-Wswitch` error under IDF's `-Werror=all`, and
 `tools/test/test_display_parity.py` checks the same from source, because
 the shipped sdkconfig builds without the display.

## Component layout

```
main/ startup order and wiring only
components/board/ pin maps, VBUS load switch, fault line, button, status LEDs
components/ups_hid/ USB host, descriptor parsing, feature-report polling
components/nut_server/ TCP 3493, read-only NUT subset
components/webui/ HTTP setup + status + OTA, embedded single page
components/netmgr/ Wi-Fi STA, provisioning SoftAP, reconnect forever, mDNS
components/display/ optional SSD1306, compiled out when not fitted
```

`ups_data_t` is the only shared state. `ups_hid` writes it under a mutex;
`nut_server` and `webui` read snapshots. Nothing else holds UPS state.

## USB HID acquisition

The UPS is a USB **device**; the board is the **host** and must supply VBUS.
The UPS port sources no power, which is the single most common reason a build
like this never enumerates.

Sequence per attach:

1. `board_vbus_set(true)`, settle 500ms.
2. Enumerate, read string descriptors into `mfr` / `model` / `serial`.
3. `GET_DESCRIPTOR(Report)` and walk it with `hid_parse_report_descriptor`.
4. `hid_apply_quirks` for known vendor descriptor bugs.
5. Poll on a 2s interval.

**Most useful values arrive only via `GET_REPORT(Feature)` control transfers.**
Espressif's `esp_hid` host component is oriented toward input reports and will
not fetch these, so `ups_hid` issues the control transfers directly against
`usb_host`. Input reports do arrive for some status changes and are handled as
an optimisation, not as the source of truth.

After `FAILURES_BEFORE_RESET` (5) consecutive failed polls, `ups_hid_recover`
drops VBUS for 2s and re-enumerates. This fires **once** per wedge, not on
every cycle, so a UPS that is simply unplugged does not sit in a power-cycling
loop.

## Pass-through policy

**Whatever the UPS reports gets published.** The parser discovers every Power
Device and Battery System field in the descriptor; nothing is filtered on the way
in. What differs is which output a value can reach.

| Channel | What it carries |
|---|---|
| NUT (3493) | Standard NUT variable names, plus `ups.status` flags |
| `/api/status` | The same, as JSON, for the web UI |
| `/api/raw` | **Every discovered usage**, by page and usage ID, named or not |
| MQTT | Every usage, named where possible |

NUT gets standard names rather than raw usages because NUT variable names are a
namespace that Home Assistant, Synology and `upsmon` parse — a variable they do
not recognise is ignored, so inventing names achieves nothing there. NUT's own
`usbhid-ups` works the same way. `/api/raw` exists precisely so nothing is lost:
no namespace applies over HTTP, so the raw usage list goes out verbatim.

`ups_varmap.c` holds the usage+collection to NUT name table, currently 35
entries covering the standard namespace.

### Collections disambiguate values

A usage's meaning depends on its enclosing collection. `Voltage` (0x84:0x30)
appears three times on the captured APC — under `Input`, `Battery` and
`PowerSummary` — so a bare usage lookup cannot tell input voltage from battery
voltage. The parser tracks a collection stack and records the nearest
*significant* enclosing collection with each field; `UPS` and `PresentStatus`
wrap everything and are skipped. `hid_find_in` is the disambiguating lookup.

### What the captured APC yields

Running `tools/decode-hid.py` on `captures/051d-0002-*` gives, from 86 fields:

- **20 standard NUT variables**: `input.voltage`, `input.voltage.nominal`,
 `input.transfer.low`/`.high`, `battery.charge`, `battery.runtime`,
 `battery.voltage`, `battery.voltage.nominal`, `battery.capacity`,
 `battery.capacity.full`, `battery.charge.low`, `battery.type`, `ups.load`,
 `ups.realpower.nominal`, `ups.delay.shutdown`, `ups.beeper.status`,
 `ups.test.result`, `ups.mfr`, `ups.model`, `ups.serial`.
- **11 status bits** feeding `ups.status` — `ACPresent`, `Charging`,
 `Discharging`, `BelowRemainingCapacityLimit`, `NeedReplacement`, `Overload`,
 `ShutdownImminent`, `CommunicationLost`, `BatteryPresent`,
 `RemainingTimeLimitExpired`, `VoltageNotRegulated`. These are flags, not
 variables, which is why they have no NUT variable name.
- **~10 vendor or internal usages** with no NUT equivalent, published raw.

Note this unit reports no `Output` collection at all, so there is no
`output.voltage` — normal for a line-interactive Back-UPS. Absent stays absent
rather than being filled with the input reading.

## Supporting more UPS models

**The generic path is the product. Profiles are the exception.**

USB HID Power Device is a real standard and most units follow it, so a UPS with
no profile at all is expected to work. `components/ups_hid/ups_profiles.c` holds
a table keyed on VID and PID whose only job is to patch models that *deviate* —
a wrong unit exponent, a usage in the wrong page, a value needing inversion.

The rule: **never add a profile to make a UPS work.** Add one only when a unit is
observed reporting something incorrectly, and record what was observed in the
`note` field. An empty table should still yield a working NUT server.

Matching is most-specific-first: exact `vid`+`pid`, then `vid` + `UPS_PID_ANY`,
then `NULL` for the generic path. `NULL` is a normal outcome, not a failure, and
the matched profile name is logged and exposed in `/api/status` so you can see
which path a device took.

To add a model:

1. Capture it with `tools/capture-ups.sh`, which dumps the report descriptor and
 NUT's own decode to `captures/`.
2. Compare `usages.txt` against the mapping table below. If everything lines up,
 **you are done** — no profile needed.
3. Only if a value is wrong, add a table entry with a `fixup` that corrects that
 one field, and say in `note` what you measured and how.

Note that one board serves one UPS. Several UPSes means several boards, which is
the cheap answer at roughly $10 each. Monitoring multiple units from a single
board needs a USB hub IC and is deferred to Rev B; `ups_data_t` being a single
struct rather than an array is a deliberate part of keeping Rev A simple.

### Known quirk: CyberPower voltage scaling

Several CPS models declare a wrong unit exponent for `output.voltage`, giving
1230 or 12.3 where the real value is 123. NUT's `cps-hid` subdriver patches
this at runtime and so does `hid_apply_quirks`, keyed on VID and usage rather
than on model string.

**Still unverified against a CyberPower**, because no CyberPower has been
captured — `ups_profiles.c` carries the entry as an empty placeholder and says
so. What *has* been shown, on a real APC, is that the generic path needs no
quirk at all: report 49 required shift 0 and report 38 required shift −2, and
`hid_unit_decimal_offset` produced both correctly, giving 120.0 V and 27.14 V.
That is this whole class of bug not happening without a vendor table. When a
CyberPower is on the bench, check it against a meter before trusting any
correction.

## HID usage to NUT variable mapping

Usage IDs below are confirmed against a real capture (APC Back-UPS RS 1000G,
`captures/051d-0002-*`). Decode any new unit with `tools/decode-hid.py`.

| Page | Usage | `ups_data_t` field | NUT variable | Unit |
|---|---|---|---|---|
| 0x85 Battery | 0x66 RemainingCapacity | `battery_charge` | `battery.charge` | % |
| 0x85 Battery | 0x68 RunTimeToEmpty | `battery_runtime` | `battery.runtime` | s |
| 0x84 Power | 0x30 Voltage (battery path) | `battery_voltage` | `battery.voltage` | V |
| 0x84 Power | 0x30 Voltage (input path) | `input_voltage` | `input.voltage` | V |
| 0x84 Power | 0x32 Frequency | `input_frequency` | `input.frequency` | Hz |
| 0x84 Power | 0x30 Voltage (output path) | `output_voltage` | `output.voltage` | V |
| 0x84 Power | 0x35 PercentLoad | `ups_load` | `ups.load` | % |
| 0x84 Power | 0x44 ConfigActivePower | `ups_realpower_nom` | `ups.realpower.nominal` | W |

Note that `Voltage` (0x84:0x30) appears several times under different parent
collections — input, output and battery — so a bare usage lookup is ambiguous.
The capture shows report IDs 9, 38 and 49 all carrying `Voltage`. Resolving which
is which requires tracking the enclosing collection, which the parser does not
yet do. **This is the main open problem for M1.**

### Scaling

A field's real value is `raw * 10^(unit_exponent - unit_offset)`, where
`unit_offset = 2*length_exp + 3*mass_exp` from the HID Unit field, because HID's
SI Linear system measures in centimetres and grams rather than metres and
kilograms. That gives 7 for volts and watts and 0 for amps, seconds, hertz and
dimensionless values.

This is the generic explanation for voltages arriving as 1230 or 12.3 instead of
123, and it is why no vendor profile is needed for it. Confirmed on the APC
capture: report 49 declares exponent 7 for volts (raw is already volts, max 255)
while report 9 declares exponent 5 (centivolts, max 655.35).

### Status bits

Assembled into `ups.status`. Usage IDs confirmed from the APC `PresentStatus`
bitfield in report 22:

| Page | Usage | Bit | NUT flag |
|---|---|---|---|
| 0x85 | 0xD0 ACPresent | `UPS_STATUS_ONLINE` | `OL` |
| 0x85 | 0x45 Discharging | `UPS_STATUS_ONBATT` / `UPS_STATUS_DISCHARGE` | `OB` / `DISCHRG` |
| 0x85 | 0x42 BelowRemainingCapacityLimit | `UPS_STATUS_LOWBATT` | `LB` |
| 0x85 | 0x44 Charging | `UPS_STATUS_CHARGING` | `CHRG` |
| 0x85 | 0x4B NeedReplacement | `UPS_STATUS_REPLACEBATT` | `RB` |
| 0x84 | 0x65 Overload | `UPS_STATUS_OVERLOAD` | `OVER` |
| 0x84 | 0x69 ShutdownImminent | — | drives `LB` as a fallback |
| 0x84 | 0x73 CommunicationLost | — | marks the snapshot stale |
| 0x85 | 0xD1 BatteryPresent | — | absent battery is a fault |
| 0x85 | 0xDB VoltageNotRegulated | — | informational |

Where `ACPresent` is absent, derive `OL` as "not Discharging" rather than
reporting `OFF`. A UPS reporting neither yields `OFF`, not a guess.

**Confirmed against live reports.** These IDs were taken from the
HID Power Device spec assignment and, until M1, only produced a coherent
bitfield on paper — macOS would not release the device to userspace, so only the
descriptor had been captured.

A real APC Back-UPS RS 1000G has now been read through a full transfer to
battery and back: `OL` on mains, `OB,DISCHRG` with the plug pulled, `OL,CHRG` on
restore, with `battery.charge` falling 100→97% and `battery.voltage`
26.74→25.40 V in between. **`ACPresent`'s polarity is the one that matters** —
it decides `OL` against `OB`, and inverted it reports "on battery" to Home
Assistant while the mains is fine. It is the right way round. See
`captures/051d-0002-m1-values-20260919/`.

Still unproven: `LB`. It is mapped from `BelowRemainingCapacityLimit` but no run
has taken the battery low enough to assert it.

## Power and runtime reporting

**Runtime is measured. Power usually is not.**

`RunTimeToEmpty` (0x85:0x68) is reported directly, in seconds, and becomes
`battery.runtime`. The UPS computes it from present load and battery state.

Power is another matter. The captured APC Back-UPS RS 1000G reports
`PercentLoad` and `ConfigActivePower` but **neither `ActivePower` nor
`ApparentPower`, and no `Current` or `Frequency`**. Most consumer UPSes are the
same: they do not meter power. So watts can only be derived:

```
watts ≈ PercentLoad% × ConfigActivePower
```

`ups_power_watts` returns the best available figure and sets an `estimated`
flag saying which route it took, so every consumer can label it honestly. It
returns `UPS_VALUE_ABSENT` when neither route exists, never a fabricated zero.

Limits of the derived figure, which matter when planning against it:

- **1% granularity.** About 6W steps on a 600W unit. A load under ~30W reads as
 0–5% and is effectively invisible.
- **No power factor.** Without `ApparentPower` there is no VA, so the number
 trusts the UPS's own load calibration.
- **Useful for trend and headroom, not energy accounting.** Do not bill from it.

### NUT variable policy

`ups.realpower` is published **only when the UPS actually measures it.** A
derived value is exposed through `/api/status` and MQTT, clearly marked
estimated, and never presented as a measured NUT variable. Publishing a computed
figure as `ups.realpower` would be exactly the fabrication that "absent is not
zero" exists to prevent — a Home Assistant user cannot tell a metered watt from
an inferred one, and will make capacity decisions on it.

Home Assistant users who want a watts sensor anyway can build one from
`ups.load` and `ups.realpower.nominal`, which is the same arithmetic done
somewhere the estimate is visible.

### Planning a failure exercise against this

Three things to know before treating these numbers as a budget:

1. **The runtime figure reflects load at that instant.** Read it, then add load,
 and it was optimistic before you started.
2. **It is the UPS's own estimate, and it flatters an aged battery.** A cell that
 has lost half its capacity often still reports a confident runtime.
3. **Derived watts inherit 1% granularity**, so small changes are invisible until
 they cross a step.

The honest measurement is empirical: **record what actually happened.** Proposed
use for the unallocated flash (see Flash layout) is an event log that captures,
on every transfer to battery, a timestamp, load percentage, starting charge, the
runtime the UPS predicted, and how long the transfer actually lasted. After two
or three real events that log is worth far more than the UPS's own estimate,
because it is measured under real load with the actual battery. Not implemented;
recorded here so the flash stays reserved for it.

## NUT protocol subset

Read-only. No `INSTCMD`, no `SET`, no shutdown control in Rev A firmware.

| Command | Behaviour |
|---|---|
| `VER` | `ups-adaptor <version>` |
| `NETVER` | `1.2` |
| `LIST UPS` | one entry, `ups`, with a description |
| `LIST VAR ups` | every populated variable |
| `GET VAR ups <var>` | one variable, or `ERR VAR-NOT-SUPPORTED` |
| `GET DESC ups <var>` | short description |
| `USERNAME` / `PASSWORD` / `LOGIN` / `LOGOUT` | accepted, credentials ignored |
| `GET NUMLOGINS ups` | count of connected clients |
| `STARTTLS` | `ERR FEATURE-NOT-SUPPORTED` |
| anything else | `ERR UNKNOWN-COMMAND` |

Home Assistant issues `LIST UPS` then `LIST VAR` and little else. `upsmon`
additionally expects `LOGIN` and `GET NUMLOGINS` to answer coherently, so they
are implemented even though authentication is not enforced — the device is
read-only and lives on a trusted LAN. **If shutdown commands are ever added,
authentication stops being optional.**

## Web interface

Single embedded page, no CDN and no framework: the device is frequently on a
network with no internet route.

| Route | Method | Purpose |
|---|---|---|
| `/` | GET | status + setup page |
| `/api/status` | GET | JSON snapshot, polled every 3s |
| `/api/scan` | GET | visible APs for the setup form |
| `/api/wifi` | POST | store credentials, reconnect |
| `/api/ups/reset` | POST | power-cycle the UPS USB interface |
| `/api/cmd` | POST | `{cmd, arg}` — run a UPS command, see below |
| `/api/ota/check` | POST | ask GitHub what the latest release is |
| `/api/ota/install` | POST | download and write it — only ever from a click |
| `/api/ota/auto` | POST | `{enabled}` — permit the twelve-hourly check |
| `/api/factory` | POST | wipe config, reboot to provisioning — **M4** |

First boot with no stored credentials brings up SoftAP `ups-adaptor-XXXX` with
a captive-portal redirect, so setup needs only a browser. Holding BOOT at
power-on clears credentials, which is the recovery path for a device that was
given the wrong SSID.

## Being a NUT server without NUT

NUT's wire protocol is public and line-based, so a client cannot tell the
difference between this and `upsd` — which matters, because NUT itself could
not run here. Its driver calls `fork`, `chroot`, `setuid` and `opendir`, links
libusb, and publishes to `upsd` through a Unix socket under a state directory.
ESP-IDF has tasks rather than processes, lwIP has no `AF_UNIX`, and the USB
stack is Espressif's own. Porting it means replacing the process model, the
IPC, the config parsing and the USB layer, at which point nothing of NUT is
left. Implementing the protocol is the smaller job by a wide margin, and the
data model was already in NUT's vocabulary.

Implemented: `VER`, `NETVER`/`PROTVER`, `HELP`, `LIST UPS`, `LIST VAR`,
`LIST CMD`, `LIST RW`, `LIST CLIENT`, `GET VAR`, `GET DESC`, `GET UPSDESC`,
`GET CMDDESC`, `GET NUMLOGINS`, `INSTCMD`, `SET VAR`, `LOGIN`, `LOGOUT`,
`USERNAME`/`PASSWORD`, and `STARTTLS` refused with `ERR
FEATURE-NOT-SUPPORTED`. Unknown input gets NUT's own error vocabulary --
`ERR UNKNOWN-UPS`, `ERR VAR-NOT-SUPPORTED`, `ERR CMD-NOT-SUPPORTED` -- because
a client that receives an error it does not recognise behaves worse than one
that receives a refusal it does.

`LIST CMD` advertises only the commands the attached UPS actually implements,
from the same capability bits that drive the web UI. `GET VAR` is answered by
searching the string `LIST VAR` builds, so the two cannot disagree about which
variables exist. A variable the UPS does not report is absent rather than
zero: a NUT client treats a present variable as authoritative.

There is no access control. The device serves read-only readings on a LAN, and
`USERNAME`/`PASSWORD` are accepted and ignored rather than refused, because
refusing a credential a client volunteers only breaks the client.

## Commands, and why the UI is built from the UPS

`/api/cmd` takes NUT's own instcmd names -- `test.battery.start.quick`,
`test.battery.start.deep`, `test.battery.stop`, `beeper.disable`,
`beeper.enable`, `beeper.mute`, `load.off` (arg = seconds) -- plus
`battery.charge.low` (arg = percent) and `battery.date`, which NUT models as
read-write variables rather than commands. One vocabulary across both outputs
means the dispatch on 3493 is a lookup, not a translation. Each maps to one
writable Feature usage:

| Command | Usage | Page |
|---|---|---|
| `test.*` | `Test` 0x58 | Power Device |
| `beeper.*` | `AudibleAlarmControl` 0x5A | Power Device |
| `lowbatt.limit` | `RemainingCapacityLimit` 0x29 | Battery System |
| `load.off` | `DelayBeforeShutdown` 0x57 | Power Device |

**Nothing is assumed from the model name.** At enumeration `discover_caps`
looks each usage up in the parsed descriptor and sets a bit in
`ups_data_t.caps`; a command whose bit is clear returns `ESP_ERR_NOT_SUPPORTED`
and the web UI never draws its button. The bench APC reports all four. A UPS
with none gets a page that says so, and that is the same code path.

**Writes are read-modify-write.** A report can carry several fields — on this
APC report 21 holds `DelayBeforeShutdown` next to its neighbours — so writing a
freshly zeroed buffer would clobber them. Reading first costs one control
transfer and means a beeper command cannot set a shutdown timer by accident.

**Commands run on the polling task.** `ctrl_in` and `ctrl_out` both pump
`usb_host_client_handle_events`, and that pump belongs to one task; a command
arriving on the web server's task would be a second pumper on the same client.
Requests are queued, executed between polls, and the caller waits for a result.

**The `Test` usage is written with one table and read with another.** NUT's
`usbhid-ups` writes 1/2/3 for quick/deep/abort, but reads 0 "No test",
1 "Done and passed", 2 "Done and warning", 3 "Done and error", 4 "Aborted",
5 "In progress", 6 "No test initiated". Inferring one table from the other is a
mistake this project made and caught on hardware: the idle APC returns 6, which
under the spec-shaped guess reads as "error". A status page that calls a healthy
battery FAILED is worse than one that says nothing. The tables were checked
against the shipped `usbhid-ups` binary.

**`load.off` is not like the others.** It cuts power to everything the UPS
feeds, and on a finished board that includes this adaptor — it goes dark and
cannot report what happened. This APC has `DelayBeforeShutdown` and no
`DelayBeforeStartup`, so there is no matching "on": the outlets stay dead until
mains returns. The web UI keeps the button disabled until someone types `OFF`.

Three details in the network path are decisions rather than defaults:

**The setup AP is open.** A passphrase on a setup network has to be printed on
the board or in the manual, so it is public anyway, and it stops a phone
joining by itself. The only thing the AP accepts is the credentials for the
network the person is standing in.

**Provisioning reboots rather than switching mode in place.** A live
APSTA-to-STA switch has to tear down the access point the browser is talking to
from inside the request that browser is waiting on, then bring up a station and
get a lease. The reboot does the same work in a known order with no
half-states. `netmgr_provision` defers it by 1.2 s so the HTTP reply leaves
first — otherwise the page reports a failure for something that worked.

**The captive-portal DNS answers every question with 192.168.4.1.** That is
what makes a phone show the setup page by itself: it fetches a known URL after
joining and displays whatever comes back. Without a resolver on an isolated AP
the lookup fails and no sheet appears. The reply is assembled by hand — echo
the question, append one answer record — because that is a dozen lines against
a DNS library for one probe.

The same wildcard route is an ordinary 404 once the device is on a real
network. Redirecting every stray request there would break a browser rather
than help it.

## Discovery

`netmgr` advertises `ups-adaptor-XXXX.local` over mDNS once an address is acquired
-- the same `XXXX` as the setup access point, from the last two bytes of the AP
MAC, so two boards on one network never claim the same name. The same name is
sent as the DHCP hostname, so it is also what a router's client list shows. It
also advertises `_http._tcp` on 80 and `_nut._tcp` on 3493. This is the normal way to reach
the web UI, and it works on boards with no OLED fitted. Setup is idempotent and
re-runs harmlessly after a reconnect. An mDNS failure is logged and ignored --
the IP address still works, and the device must keep serving NUT regardless.

## Status indication

Three layers, in decreasing order of detail: the **web UI** (everything), the
**optional OLED** (UPS power state, readings, IP, NUT clients, Wi-Fi,
firmware, and the LED state), and **two
plain LEDs**. The OLED is a superset of the LEDs by rule (design rule 5).

The LEDs are deliberately coarse, and are a green/red 0603 pair rather than an
addressable RGB part -- a WS2812B is an extended component at JLC, so a $0.06
LED costs $3.13 on a small run, and it would pull the RMT driver in for status
detail the other two layers already carry.

| State | Green | Red | OLED line 3 |
|---|---|---|---|
| Booting | solid | off | `STATUS BOOTING` |
| Provisioning AP up | fast blink | off | `STATUS SETUP` |
| Connecting or retrying | slow blink | off | `STATUS CONNECTING` |
| Online, UPS present | solid | off | `STATUS OK` |
| Online, no UPS | solid | slow blink | `STATUS NO UPS`, reversed |
| VBUS fault | off | solid | `STATUS VBUS FAULT`, reversed |

`board_status_set` takes a semantic state, not pins, so the LED scheme can
change without touching a caller. The display reads the same state back with
`board_status_get`, so the screen and the LEDs cannot disagree. They used to:
the screen drew its own conclusions from `netmgr` and `ups_hid`, and printed
`VBUS FAULT` only while no UPS was attached, while the red LED showed a fault
whether a UPS was attached or not. On the devkit the calls compile to a log line;
the patterns are verified on Rev A.

## Optional OLED

Gated on `CONFIG_UPSA_HAVE_OLED`, default off, and offered only on boards whose
header takes a standard module as seated (`UPSA_BOARD_OLED_OK`): the devkit
today, not Rev A, whose header is wired in reverse. When disabled the whole component
is `#if`'d out to no-op stubs, so `main.c` calls it unconditionally with no
`#ifdef` at the call site and a board without the module pays nothing.

When enabled it probes the bus first; an absent or unresponsive panel logs a
warning and the device carries on serving NUT. The font is a 43-glyph 5x7 subset
(space, `% - . / : ?`, digits, `A-Z`, lowercase folded up) because no status line
needs more. Assumes I2C address 0x3C; some modules are 0x3D.

### Screen layout

Rows 0-2 are the same on every page, so an alarm is never paged out of view:

| Row | Shows |
|---|---|
| 0 | UPS model, or `UPS-ADAPTOR` with none attached |
| 1 | What the UPS is doing: `ON LINE`, `ON LINE  CHARGING`, `ON BATTERY`, `ON BATTERY  LOW`, `OVERLOAD`, `REPLACE BATTERY`, or `UPS NOT FOUND`. Everything but on line is reversed |
| 2 | The LED state (design rule 5), reversed when the red LED is lit |

Rows 4-7 alternate every 5 seconds between two pages:

| Row | Page 1 | Page 2 |
|---|---|---|
| 4 | `BATT 97%  RUN 45M` | `BATT 26.7V  OUT 120V` |
| 5 | `IN 120V   LOAD 12%` | NUT status words, `FLAGS OL CHRG` |
| 6 | IP address, or the setup AP | Wi-Fi signal, `WIFI -55 DBM` |
| 7 | `NUT 2 CLIENTS` | `FW <version>`, or `UPDATE <tag>` when one is available |

`NUT n CLIENTS` is there because "is Home Assistant actually watching?" is
otherwise the hardest thing to check on this device.

**Burn-in.** An OLED in a closet shows the same rows for months, and those burn
into the panel first. After 5 minutes with no change of state the screen dims
to minimum contrast. Every 2 minutes all text shifts one pixel sideways,
through a 0-3 pixel cycle, so no column stays lit forever. Any change of state
(device, attach, UPS status) brings full brightness back, and nothing dims
while row 1 or row 2 is showing an alarm.

**BOOT at runtime.** GPIO0 is otherwise only read at power-on (hold it to clear
Wi-Fi settings). While running, a press wakes a dimmed screen, and on a lit
screen flips to the other page.

## Wi-Fi reliability

`netmgr` reconnects indefinitely with backoff capped at 30s — never
exponential-forever, because a bridge offline for an hour must still rejoin
within 30s of the AP returning. Radio power save is disabled: a NUT client may
connect at any moment and the saving is irrelevant on a mains-powered device.

The backoff waits on an `esp_timer` one-shot, not inside the disconnect handler.
Wi-Fi and IP events are delivered one at a time by a single default event loop
task, so sleeping in a handler stalls every other event behind it — including
`GOT_IP` and the mDNS bring-up that hangs off it. The handler arms the timer and
returns; the timer's own task calls `esp_wifi_connect`. Re-arming an already
armed timer is harmless, which collapses a burst of disconnect events into one
pending attempt, and a successful connection cancels it.

## Flash layout

4MB, two 1.94MB app slots. An OTA update is written to whichever slot is not
running and then booted from, so a bad image rolls back rather than bricking a
device in a closet. The slot is generous here because the web UI is embedded in
the binary rather than living on a filesystem: the application is 1.0MB of the
1.94MB available.

There is no spare region. The two slots consume the part exactly, which is the
cost of moving from the 8MB N8 to the 4MB N4: the event log of transfers to
battery that the larger part had room for is not available here. Reinstating it
means going back to N8.

## Firmware update

Images come from this repository's GitHub **releases**, not from a branch and
not from a file the owner uploads. A release is a deliberate act with a tag on
it, so publishing one is the only thing that can move a device.

Two separate consents, and only one of them is automatic:

| | Automatic? | Why |
|---|---|---|
| Noticing | only if enabled, then every 12h | a device on a private network reaching the internet on a timer is the owner's decision |
| Installing | **never** | it writes flash and reboots something the house may depend on |

**Which asset.** `ups-adaptor-<target>.bin`, where target is
`BOARD_OTA_TARGET`. A release with no asset for this board is not an update.

**The filename is not trusted.** Every image declares its board in its app
descriptor's project name (`ups-adaptor-reva`, `ups-adaptor-freenove`), and the
client reads the incoming descriptor and refuses anything that is not its own
name *before writing a byte* to the other slot:

```
E (53492) ota: REFUSED: image is "ups-adaptor-reva", this board runs "ups-adaptor-freenove"
```

That is not hypothetical. `v0.02c` shipped a Rev A build under the devkit's
filename, and a devkit installed it and came up believing it had a load switch
on GPIO4. A name is a label anyone can get wrong; a descriptor is what the
image says about itself. It is also what makes a future Rev B safe — an older
unit cannot take its firmware however the file is named.

A refused tag is **remembered for the rest of the boot** (`OTA_INCOMPATIBLE`,
`ota_status_t.refused`), so the periodic check stops putting an Install button
in front of someone for a file that cannot be installed. It is not persisted:
the usual fix is to re-upload the asset under the same tag, and a memory that
survived a reboot would keep refusing the corrected file. An explicit *check
now* clears it, so nobody is ever stuck.

**Rollback is earned, not assumed.** A freshly installed image boots on trial
and reverts unless it confirms itself. Confirming on Wi-Fi association is
worthless against the bugs that actually shipped here — a handler table too
small to register and a web server stack too small to answer both let the
device boot, associate and confirm itself seconds before the first request
killed it. It now takes five minutes of uptime *and* three served HTTP requests.
See `docs/RELEASING.md`.

## Prior art

GPL-3.0 is chosen to stay compatible with reusing logic from existing ESP32 NUT
server projects. Where code or protocol detail is taken from one, attribute it
in the file header. NUT's own `usbhid-ups` and `cps-hid` subdriver are the
reference for HID interpretation and are GPL-2.0-or-later.
