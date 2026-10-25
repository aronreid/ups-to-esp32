# My UPS Pal firmware architecture

## Design rules

1. **The web interface is the primary UI.** Setup, status, diagnostics and
 firmware update all happen in a browser. No app, no serial console required
 for normal use, no phone pairing. The OLED is optional and off by default.
2. **No GPIO numbers outside `components/board`.** Rev A and Rev B each have
 a block in `board.h`. A new revision is a new block there.
3. **Absent is not zero.** A UPS that does not report a usage must produce
 `null` in JSON and no `VAR` line in NUT, never `0.0`.
4. **Never require a physical reset.** Wi-Fi dropouts, UPS unplugs and UPS
 firmware wedges are all recovered in software.
5. **If the OLED is fitted, it shows everything the LEDs show.** On Rev A the
 module sits directly over both LEDs, so the screen is the only local
 indicator on a board that has one. Every `board_status_t` state gets a line
 on the screen, and every state that lights the red LED is drawn in reverse.
 `status_text()` in `display.c` switches with no default, so a missing state
 is a `-Wswitch` error under IDF's `-Werror=all`, and a host-side test checks
 the same from source, because the shipped sdkconfig builds without the
 display.

## Component layout

```
main/ startup order and wiring only
components/board/ pin maps, VBUS load switch, fault line, button, status LEDs
components/ups_hid/ USB host, descriptor parsing, feature-report polling
components/nut_server/ TCP 3493, NUT protocol
components/webui/ HTTP setup + status + OTA, embedded single page, /metrics
components/netmgr/ Wi-Fi STA, provisioning SoftAP, reconnect forever, mDNS
components/auth/ the login: hashing, tokens, lockout
components/ota/ release checks, download, signature and name checks, rollback
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
| `/api/vars` | Every NUT variable the UPS provides, as JSON |
| `/api/descriptor` | The UPS's USB report descriptor, byte for byte, so nothing the UPS says is lost |

NUT gets standard names rather than raw usages because NUT variable names are a
namespace that Home Assistant, Synology and `upsmon` parse: a variable they do
not recognise is ignored, so inventing names achieves nothing there. NUT's own
`usbhid-ups` works the same way. `/api/descriptor` exists so nothing is lost:
the descriptor goes out as the UPS sent it, for decoding off the board.

`ups_varmap.c` holds the usage+collection to NUT name table, covering the
standard namespace.

### Collections disambiguate values

A usage's meaning depends on its enclosing collection. `Voltage` (0x84:0x30)
appears three times on an APC Back-UPS (under `Input`, `Battery` and
`PowerSummary`), so a bare usage lookup cannot tell input voltage from battery
voltage. The parser tracks a collection stack and records the nearest
*significant* enclosing collection with each field; `UPS` and `PresentStatus`
wrap everything and are skipped. `hid_find_in` is the disambiguating lookup.

### What an APC Back-UPS RS 1000G yields

Decoding the descriptor in `captures/051d-0002-*` gives, from 86 fields:

- **20 standard NUT variables**: `input.voltage`, `input.voltage.nominal`,
 `input.transfer.low`/`.high`, `battery.charge`, `battery.runtime`,
 `battery.voltage`, `battery.voltage.nominal`, `battery.capacity`,
 `battery.capacity.full`, `battery.charge.low`, `battery.type`, `ups.load`,
 `ups.realpower.nominal`, `ups.delay.shutdown`, `ups.beeper.status`,
 `ups.test.result`, `ups.mfr`, `ups.model`, `ups.serial`.
- **11 status bits** feeding `ups.status`: `ACPresent`, `Charging`,
 `Discharging`, `BelowRemainingCapacityLimit`, `NeedReplacement`, `Overload`,
 `ShutdownImminent`, `CommunicationLost`, `BatteryPresent`,
 `RemainingTimeLimitExpired`, `VoltageNotRegulated`. These are flags, not
 variables, which is why they have no NUT variable name.
- **About 10 vendor or internal usages** with no NUT equivalent, published raw.

This unit reports no `Output` collection at all, so there is no
`output.voltage`: normal for a line-interactive Back-UPS. Absent stays absent
rather than being filled with the input reading.

## Supporting more UPS models

**The generic path is the product. Profiles are the exception.**

USB HID Power Device is a real standard and most units follow it, so a UPS with
no profile at all is expected to work. `components/ups_hid/ups_profiles.c` holds
a table keyed on VID and PID whose only job is to patch models that *deviate*:
a wrong unit exponent, a usage in the wrong page, a value needing inversion.

The rule: **never add a profile to make a UPS work.** Add one only when a unit is
observed reporting something incorrectly, and record what was observed in the
`note` field. An empty table should still yield a working NUT server.

Matching is most-specific-first: exact `vid`+`pid`, then `vid` + `UPS_PID_ANY`,
then `NULL` for the generic path. `NULL` is a normal outcome, not a failure, and
the matched profile name is logged and exposed in `/api/status` so you can see
which path a device took.

To add a model:

1. Capture its report descriptor, and NUT's own decode of the unit if a NUT
 host is available, into `captures/`.
2. Compare the usages against the mapping table below. If everything lines up,
 **you are done**: no profile needed.
3. Only if a value is wrong, add a table entry with a `fixup` that corrects that
 one field, and say in `note` what you measured and how.

One board serves one UPS. Several UPSes means several boards. `ups_data_t` is a
single struct rather than an array.

### Known quirk: CyberPower voltage scaling

Several CPS models declare a wrong unit exponent for `output.voltage`, giving
1230 or 12.3 where the real value is 123. NUT's `cps-hid` subdriver patches
this at runtime and so does `hid_apply_quirks`, keyed on VID and usage rather
than on model string. The generic path needs no quirk for a correctly declared
exponent: `hid_unit_decimal_offset` handles a report declaring volts and one
declaring centivolts on the same UPS (see Scaling, below).

## HID usage to NUT variable mapping

Usage IDs below are confirmed against a real capture (APC Back-UPS RS 1000G,
`captures/051d-0002-*`).

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

`Voltage` (0x84:0x30) appears several times under different parent collections
(input, output and battery). On the APC capture, report IDs 9, 38 and 49 all
carry `Voltage`; the parser tells them apart by the enclosing collection, as
described above.

### Scaling

A field's real value is `raw * 10^(unit_exponent - unit_offset)`, where
`unit_offset = 2*length_exp + 3*mass_exp` from the HID Unit field, because HID's
SI Linear system measures in centimetres and grams rather than metres and
kilograms. That gives 7 for volts and watts and 0 for amps, seconds, hertz and
dimensionless values.

This is the generic explanation for voltages arriving as 1230 or 12.3 instead of
123, and it is why no vendor profile is needed for it. On the APC capture,
report 49 declares exponent 7 for volts (raw is already volts, max 255) while
report 9 declares exponent 5 (centivolts, max 655.35); both decode correctly.

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
| 0x84 | 0x69 ShutdownImminent | - | drives `LB` as a fallback |
| 0x84 | 0x73 CommunicationLost | - | marks the snapshot stale |
| 0x85 | 0xD1 BatteryPresent | - | absent battery is a fault |
| 0x85 | 0xDB VoltageNotRegulated | - | informational |

Where `ACPresent` is absent, `OL` is derived as "not Discharging" rather than
reporting `OFF`. A UPS reporting neither yields `OFF`, not a guess.

`ACPresent` decides `OL` against `OB`; inverted, it would report "on battery"
to Home Assistant while the mains is fine. Through a full transfer to battery
and back, the flags read `OL` on mains, `OB,DISCHRG` with the plug pulled and
`OL,CHRG` on restore.

`LB` follows the UPS's own `BelowRemainingCapacityLimit` flag; the firmware
does not compute it. A UPS may raise the flag when its runtime estimate drops
under `battery.runtime.low` rather than when charge reaches
`battery.charge.low`, and that estimate wanders under a fluctuating load, so
`LB` can arrive at a charge well above the limit.

## Power and runtime reporting

**Runtime is measured. Power usually is not.**

`RunTimeToEmpty` (0x85:0x68) is reported directly, in seconds, and becomes
`battery.runtime`. The UPS computes it from present load and battery state.

Power is another matter. The APC Back-UPS RS 1000G reports `PercentLoad` and
`ConfigActivePower` but **neither `ActivePower` nor `ApparentPower`, and no
`Current` or `Frequency`**. Most consumer UPSes are the same: they do not meter
power. So watts can only be derived:

```
watts ≈ PercentLoad% × ConfigActivePower
```

`ups_power_watts` returns the best available figure and sets an `estimated`
flag saying which route it took, so every consumer can label it honestly. It
returns `UPS_VALUE_ABSENT` when neither route exists, never a fabricated zero.

Limits of the derived figure, which matter when planning against it:

- **1% granularity.** About 6W steps on a 600W unit. A load under about 30W
 reads as 0 to 5% and is effectively invisible.
- **No power factor.** Without `ApparentPower` there is no VA, so the number
 trusts the UPS's own load calibration.
- **Useful for trend and headroom, not energy accounting.** Do not bill from it.

### NUT variable policy

`ups.realpower` is published **only when the UPS actually measures it.** A
derived value is exposed through `/api/status`, clearly marked
estimated, and never presented as a measured NUT variable. Publishing a computed
figure as `ups.realpower` would be the fabrication that "absent is not zero"
exists to prevent: a Home Assistant user cannot tell a metered watt from an
inferred one, and will make capacity decisions on it.

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

The honest measurement is empirical: record what actually happened during a
real transfer to battery.

## NUT protocol subset

| Command | Behaviour |
|---|---|
| `VER` | `My UPS Pal <version>` |
| `NETVER` / `PROTVER` | `1.2` |
| `HELP` | the command list |
| `LIST UPS` | one entry, `ups`, with a description |
| `LIST VAR ups` | every populated variable |
| `LIST RW ups` | the writable variables |
| `LIST CMD ups` | the commands the attached UPS implements |
| `LIST CLIENT ups` | connected clients |
| `GET VAR ups <var>` | one variable, or `ERR VAR-NOT-SUPPORTED` |
| `GET DESC ups <var>` | short description |
| `GET UPSDESC ups` / `GET CMDDESC ups <cmd>` | descriptions |
| `GET NUMLOGINS ups` | answers 1 |
| `USERNAME` / `PASSWORD` / `LOGIN` / `LOGOUT` | accepted; credentials are checked at a write |
| `INSTCMD ups <cmd>` / `SET VAR ups <var> <value>` | need the login, see The login |
| `STARTTLS` | `OK STARTTLS` and a TLS handshake on the same socket when the board has a certificate; otherwise `ERR FEATURE-NOT-CONFIGURED` |
| anything else | `ERR UNKNOWN-COMMAND` |

Home Assistant issues `LIST UPS` then `LIST VAR` and little else. `upsmon`
additionally expects `LOGIN` and `GET NUMLOGINS` to answer coherently.

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
| `/api/cmd` | POST | `{cmd, arg}`: run a UPS command, see below |
| `/api/ota/check` | POST | ask GitHub what the latest release is |
| `/api/ota/install` | POST | download and write it |
| `/api/ota/auto` | POST | `{enabled}`: permit the twelve-hourly check |
| `/api/reboot` | POST | restart the board |
| `/api/auth/login` | POST | `{user, pass}` → `{token}` |
| `/api/auth/logout` | POST | forget this browser's token |
| `/api/auth/set` | POST | `{user, pass}` creates the login (the first time, no login needed) or changes it |

Every POST is refused if a browser sent it from another page (its `Origin`
names a different host): otherwise any web page open anywhere on the LAN could
post `load.off` to the board in the background. Scripts send no `Origin` and
are unaffected. Every POST except login and logout also needs the page's token
in `X-Auth-Token`, or the username and password as HTTP Basic, and so does every
read of the page's data (see The login, below): `curl -u admin:pass -X POST http://ups-esp32-XXXX.local/api/reboot`.

**The page names the failure.** A request that gets no reply at all reads
"Can't reach the board at <host>. Is this phone or computer on the same network
as it (not a VPN or guest Wi-Fi)?"; a reply the page cannot parse gives its
HTTP status and says to reload; a board waiting for a login says "locked" in
its subtitle rather than "connecting…".

There is no factory-reset route: clearing the network is what makes a board in
a closet unreachable, so it takes someone at the board (below).

First boot with no stored credentials brings up SoftAP `ups-esp32-XXXX` with
a captive-portal redirect, so setup needs only a browser. Pressing BOOT five
times within four seconds clears credentials, Wi-Fi and the login, and
restarts into setup, which is the recovery path for a device given the wrong
network. Holding BOOT at power-on cannot do it: GPIO0 is the download strap,
so the ROM flasher runs instead. A long hold is ruled out too, because a
serial terminal raising DTR alone holds GPIO0 low through the auto-reset
transistor.

## Being a NUT server without NUT

NUT's wire protocol is public and line-based, so a client cannot tell the
difference between this and `upsd`, which matters, because NUT itself cannot
run here. Its driver calls `fork`, `chroot`, `setuid` and `opendir`, links
libusb, and publishes to `upsd` through a Unix socket under a state directory.
ESP-IDF has tasks rather than processes, lwIP has no `AF_UNIX`, and the USB
stack is Espressif's own. Porting it means replacing the process model, the
IPC, the config parsing and the USB layer, at which point nothing of NUT is
left. Implementing the protocol is the smaller job by a wide margin, and the
data model is already in NUT's vocabulary.

Unknown input gets NUT's own error vocabulary: `ERR UNKNOWN-UPS`,
`ERR VAR-NOT-SUPPORTED`, `ERR CMD-NOT-SUPPORTED`. A client that receives an
error it does not recognise behaves worse than one that receives a refusal it
does.

`LIST CMD` advertises only the commands the attached UPS actually implements,
from the same capability bits that drive the web UI. `GET VAR` is answered by
searching the string `LIST VAR` builds, so the two cannot disagree about which
variables exist. A variable the UPS does not report is absent rather than
zero: a NUT client treats a present variable as authoritative.

`USERNAME` and `PASSWORD` always answer `OK`, as `upsd` does, and are checked
only when a write asks for them: see The login, below.

## The login

**Required.** Setup asks for a username and password with the Wi-Fi details.
A board with no login shows only "Create a login for this board" until one
exists, and until then refuses every change, on the page and over NUT, except
creating it. The login cannot be turned off, only changed, and a password is at
least 8 characters. **While no login exists, the first host on the LAN to call
`/api/auth/set` creates it and owns the board.** That is a new board, or one
after `clear-login`. The README says so.

**The whole page is behind it**: `/api/status`, the debug, network, scan,
descriptor and release-notes endpoints answer 401 without the page's token (or
HTTP Basic, for scripts: `curl -u admin:pass`). **Monitoring is not**: NUT's
`LIST`/`GET` and `/metrics` stay open, as `upsd` and a Prometheus exporter are,
so Home Assistant, a NAS and Prometheus watch the UPS with no setup. NUT's
writes (`INSTCMD`, `SET VAR`) need the login, refused in `upsd`'s words and
order: `ERR USERNAME-REQUIRED`, `ERR PASSWORD-REQUIRED`, `ERR ACCESS-DENIED`.
A monitor that volunteers credentials and only reads (Synology sends
`monuser`/`secret`) is never refused, since the check happens at the write.
To send a command:

    upscmd -u admin -p yourpassword ups@ups-esp32-XXXX.local load.off

In Home Assistant's NUT integration the username and password fields are only
needed for its command buttons.

**Stored hashed** (salted, 4096 rounds of SHA-256, in hardware on the S3), so a
flash dump does not hand over a password someone uses elsewhere. **Five wrong
passwords in a row** from one address lock that address out for a minute, the
web login and NUT together; 25 wrong passwords in a row from anywhere lock the
board for a minute. A web login lasts until 12 hours idle, the board restarts,
or the login changes.

**Setting a password** asks for it twice and shows a rough strength bar (Too
weak … Strong: length counts most, mixing kinds of character helps). Under 8
characters, one character repeated, or one of the first passwords scanners try
(`admin`, `password1`, `12345678` …) is refused on the page before anything is
sent. The bar is a guide, not zxcvbn: that library alone is several times the
size of the whole page.

**Forgot it?** Plug a computer into the board's USB-C port, open the serial
console at 115200 and type `clear-login`. That removes the login and nothing
else (Wi-Fi, address and settings stay) and the page then asks for a new one.
A whole line must match, so log noise or a terminal's echo cannot trip it. It
is the same trust as the BOOT button, someone holding the board's cable,
without opening a case. Pressing BOOT five times still works too, and clears
Wi-Fi with the login.

**What it is not:** encryption of the plain HTTP page or of a NUT connection
that never asks for `STARTTLS`; over those, the password crosses the LAN
readable. The login keeps people and stray scripts from changing things; it
does not stop someone who can capture your network traffic.

**Slow clients.** The page is served by one task, so a client that connects
and sends nothing, or sends a request a byte at a time, can occupy it. Receive
timeouts are 2 s, request headers are capped at 512 bytes, a request body must
arrive within 4 s, and a TLS handshake within 3 s. A single client trickling
bytes can still hold port 80 for several minutes, one connection at a time;
the NUT server and the update check run on their own tasks and are not
affected. NUT connections get 20 s to send a first command, and one address
may hold two.

## Commands, and why the UI is built from the UPS

`/api/cmd` takes NUT's own instcmd names (`test.battery.start.quick`,
`test.battery.start.deep`, `test.battery.stop`, `beeper.disable`,
`beeper.enable`, `beeper.mute`, `load.off` with arg = seconds) plus
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
and the web UI never draws its button. A UPS with none gets a page that says
so, and that is the same code path.

**Writes are read-modify-write.** A report can carry several fields (on the
APC, report 21 holds `DelayBeforeShutdown` next to its neighbours), so writing
a freshly zeroed buffer would clobber them. Reading first costs one control
transfer and means a beeper command cannot set a shutdown timer by accident.

**Commands run on the polling task.** `ctrl_in` and `ctrl_out` both pump
`usb_host_client_handle_events`, and that pump belongs to one task; a command
arriving on the web server's task would be a second pumper on the same client.
Requests are queued, executed between polls, and the caller waits for a result.

**The `Test` usage is written with one table and read with another.** NUT's
`usbhid-ups` writes 1/2/3 for quick/deep/abort, but reads 0 "No test",
1 "Done and passed", 2 "Done and warning", 3 "Done and error", 4 "Aborted",
5 "In progress", 6 "No test initiated". The two tables are not the same: an
idle APC returns 6, which read through the write table would mean "error". A
status page that calls a healthy battery FAILED is worse than one that says
nothing. The tables match the shipped `usbhid-ups`.

**What "deep" does is the UPS's choice.** The HID usage tables define the
values and nothing about what a deep test is; "calibration" appears nowhere in
them, and no NUT HID subdriver implements `calibrate.start`. The APC RS 1000G
runs a real run-down; the CyberPower EC850LCD runs its 4-second quick test
(`docs/tested-ups.md`). Both report success either way, so "the UPS accepted
it" is all a caller learns.

**A UPS's reports can freeze during a test.** An APC deep test can leave every
fresh GET_REPORT saying "on mains, 100 %, No test initiated" while the battery
drains; power-cycling the UPS's USB (the VBUS switch) makes it re-enumerate and
report the truth. A real outage during such a freeze would be reported as
mains. A UPS can also report no status bits for a couple of minutes after a
test and a USB power cycle, which the NUT server shows as `OFF`: the class of
glitch that made `upsmon` shut hosts down on APC RS units (NUT #2104).

**The page warns before and after.** `DEEP_KNOWN` in `index.html` lists what
each known model does: a model whose deep test is its quick test gets a
disabled button and the reason; one that runs it but may hide it gets that in
the confirm box. Any other model is watched: done inside three minutes is
reported as a short self-test, no sign of starting within 75 s as possibly
hidden, with the Power-cycle UPS link as the check. NUT clients are not told:
`test.battery.start.deep` stays in `LIST CMD`, as `usbhid-ups` offers it.

The last status is held through a brief all-zero status while the UPS is
attached and live, and the UPS's USB is power-cycled if it persists past five
minutes (`status_hold.h`). A deep test is refused unless the battery reads
100 % (`test_guard.h`; the web page says why, NUT clients get
`ERR INSTCMD-FAILED`).

**`load.off` is not like the others.** It cuts power to everything the UPS
feeds, and if that includes this board, it goes dark and cannot report what
happened. The APC has `DelayBeforeShutdown` and no `DelayBeforeStartup`, so
there is no matching "on": the outlets stay dead until mains returns. The web
UI keeps the button disabled until someone types `OFF`.

Three details in the network path are decisions rather than defaults:

**The setup AP is open.** A passphrase on a setup network has to be printed on
the board or in the manual, so it is public anyway, and it stops a phone
joining by itself. The only thing the AP accepts is the credentials for the
network the person is standing in.

Because the setup page is plain HTTP on an open network, the Wi-Fi password
and the new login cross the air unencrypted while they are entered, and anyone
in radio range with a capture tool can read them. Set a board up once, at
home, with no one unexpected nearby. After setup the board's own network is
off, and it comes back only if the board cannot reach its Wi-Fi for a long
time. While it is up, NUT and `/metrics` do not answer on it, and a board with
no login accepts only the Wi-Fi form and the first login there.

**Provisioning reboots rather than switching mode in place.** A live
APSTA-to-STA switch has to tear down the access point the browser is talking to
from inside the request that browser is waiting on, then bring up a station and
get a lease. The reboot does the same work in a known order with no
half-states. `netmgr_provision` defers it by 1.2 s so the HTTP reply leaves
first; otherwise the page reports a failure for something that worked.

**The captive-portal DNS answers every question with 192.168.4.1.** That is
what makes a phone show the setup page by itself: it fetches a known URL after
joining and displays whatever comes back. Without a resolver on an isolated AP
the lookup fails and no sheet appears. The reply is assembled by hand (echo
the question, append one answer record) because that is a dozen lines against
a DNS library for one probe.

The same wildcard route is an ordinary 404 once the device is on a real
network. Redirecting every stray request there would break a browser rather
than help it.

## Prometheus metrics

`GET /metrics` is a Prometheus scrape target in the text exposition format,
rendered by `components/webui/metrics.c` and streamed line by line, so its size
never has to fit a buffer.

UPS readings use **nut_exporter's names**: every NUT variable as
`network_ups_tools_<name, dots as underscores>` with a `ups` label, the status
as `network_ups_tools_ups_status{flag="OL"}` with each flag present as 0 or 1,
and `network_ups_tools_device_info` carrying mfr, model, serial and USB IDs.
Dashboards and alerts written against nut_exporter work with Prometheus
scraping the board directly. Only the seven flags this firmware can set are
exported (OL, OB, LB, RB, CHRG, DISCHRG, OVER); nut_exporter can list others,
which would be permanently zero here.

The board's own health is under `ups_esp32_`: `ups_attached`, `build_info`,
`uptime_seconds`, `crashes_total`, `heap_free_bytes`, `heap_min_free_bytes`,
`wifi_rssi_dbm`, `nut_clients`.

A value the UPS does not report is **absent**, never 0: the same rule as NUT
and `/api/status`. A missing series is honest; a battery charge of zero pages
someone.

```yaml
scrape_configs:
  - job_name: ups
    static_configs:
      - targets: ["ups-esp32-XXXX.local:80"]
```

## Discovery

`netmgr` advertises `ups-esp32-XXXX.local` over mDNS once an address is
acquired: the same `XXXX` as the setup access point, from the last two bytes of
the AP MAC, so two boards on one network never claim the same name. The same
name is sent as the DHCP hostname, so it is also what a router's client list
shows. It also advertises `_http._tcp` on 80 and `_nut._tcp` on 3493. This is
the normal way to reach the web UI, and it works on boards with no OLED
fitted. Setup is idempotent and re-runs harmlessly after a reconnect. An mDNS
failure is logged and ignored: the IP address still works, and the device must
keep serving NUT regardless.

## Status indication

Three layers, in decreasing order of detail: the **web UI** (everything), the
**optional OLED** (UPS power state, readings, IP, NUT clients, Wi-Fi,
firmware, and the LED state), and **two plain LEDs**. The OLED is a superset of
the LEDs by rule (design rule 5).

The LEDs are deliberately coarse: a green/red 0603 pair rather than an
addressable RGB part. A WS2812B is an extended component at JLC, so a $0.06
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
`board_status_get`, so the screen and the LEDs cannot disagree.

## Optional OLED

Gated on `CONFIG_UPSA_HAVE_OLED`, default off, and offered only on boards whose
header takes a standard module as seated (`UPSA_BOARD_OLED_OK`). Plain Rev A's
does not (it is wired in reverse), so plain Rev A firmware never drives a
display. The `reva-oled` board target is the released path for a module
labelled VCC, GND, SCL, SDA: its power pins match J3, and selecting that board
in Kconfig pulls in `UPSA_REVA_OLED_VCC_FIRST`, which swaps the display's
SDA/SCL to suit (`BOARD_OLED_SDA`/`_SCL` in `board.h`) and turns the display on,
both automatically. Neither is set independently, so the pin swap and the
board identity that advertises it cannot drift apart. `release.yml` refuses a
plain Rev A or Rev B image with the swap set, but builds and ships `reva-oled`
as its own target, with its own `BOARD_OTA_TARGET` so a plain Rev A board can
never be offered it. Rev B takes a standard module and enables the display
normally, under its own board choice. When disabled the whole component is
`#if`'d out to no-op stubs, so `main.c` calls it unconditionally with no
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
| 1 | What the UPS is doing: `ON LINE`, `ON LINE  CHARGING`, `ON BATTERY`, `ON BATTERY  LOW`, `OVERLOAD`, `REPLACE BATTERY`, `LOW BATTERY`, or `UPS NOT FOUND`. Everything but on line is reversed |
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

**BOOT at runtime.** A press wakes a dimmed screen, and on a lit screen flips
to the other page. Five presses within four seconds also clear Wi-Fi settings.

## Wi-Fi reliability

`netmgr` reconnects indefinitely with backoff capped at 30s, never
exponential-forever, because a bridge offline for an hour must still rejoin
within 30s of the AP returning. Radio power save is disabled: a NUT client may
connect at any moment and the saving is irrelevant on a mains-powered device.

The backoff waits on an `esp_timer` one-shot, not inside the disconnect handler.
Wi-Fi and IP events are delivered one at a time by a single default event loop
task, so sleeping in a handler stalls every other event behind it, including
`GOT_IP` and the mDNS bring-up that hangs off it. The handler arms the timer and
returns; the timer's own task calls `esp_wifi_connect`. Re-arming an already
armed timer is harmless, which collapses a burst of disconnect events into one
pending attempt, and a successful connection cancels it.

## Flash layout

Both boards hold two app slots. An OTA update is written to whichever slot is
not running and then booted from, so a bad image rolls back rather than
bricking a device in a closet. The slots are generous because the web UI is
embedded in the binary rather than living on a filesystem.

**Rev A** (N4, `partitions.csv`): 4MB, two 1.94MB slots. The two slots consume
the part exactly, so there is no spare region and no on-board event log.

**Rev B** (N16R8, `partitions-revb.csv`): 16MB, two 3MB slots, and the rest of
the part (9.9MB from 0x620000) as the `outagelog` data partition, the event log
of transfers to battery.

## Firmware update

Images come from this repository's GitHub **releases**, not from a branch and
not from a file the owner uploads. A release is a deliberate act with a tag on
it, so publishing one is the only thing that can move a device.

Two separate consents on the production channel, and only one of them is
automatic:

| | Automatic? | Why |
|---|---|---|
| Noticing | only if enabled, then every 12h | a device on a private network reaching the internet on a timer is the owner's decision |
| Installing | **never** | it writes flash and reboots something the house may depend on |

The other channels, and what a release contains, are in `docs/updates.md`.

**Which asset.** `ups-adaptor-<target>.bin`, where target is
`BOARD_OTA_TARGET`. A release with no asset for this board is not an update.

**The filename is not trusted.** Every image declares its board in its app
descriptor's project name (`ups-adaptor-reva`, `ups-adaptor-revb`, and so on),
and the client reads the incoming descriptor and refuses anything that is not
its own name *before writing a byte* to the other slot:

```
E (53492) ota: REFUSED: image is "ups-adaptor-reva", this board runs "ups-adaptor-revb"
```

A name is a label anyone can get wrong; a descriptor is what the image says
about itself. It is also what keeps board revisions apart: a unit cannot take
another board's firmware however the file is named.

A refused tag is **remembered for the rest of the boot** (`OTA_INCOMPATIBLE`,
`ota_status_t.refused`), so the periodic check stops putting an Install button
in front of someone for a file that cannot be installed. It is not persisted:
the usual fix is to re-upload the asset under the same tag, and a memory that
survived a reboot would keep refusing the corrected file. An explicit *check
now* clears it, so nobody is ever stuck.

**Rollback is earned, not assumed.** A freshly installed image boots on trial
and reverts unless it confirms itself. Confirming on Wi-Fi association is not
enough: a device can boot, associate and die on its first request. Confirming
takes five minutes of uptime *and* three served HTTP requests.

### Signed updates

The name check above stops a wrong board's image. It does not stop someone
who can publish a release, for example with a stolen GitHub token. Release
images therefore carry a signature, and a board checks it before it installs
an update.

This is ESP-IDF's "signed app verification without hardware secure boot",
turned on by `firmware/sdkconfig.defaults.signed`:

| Option | Value |
|---|---|
| `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT` | on |
| `CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME` | RSA-3072, the Secure Boot v2 block format |
| `CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT` | on: `esp_ota_end()` checks the signature |
| `CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES` | off: the build never sees the key |

How the check works. A signed image ends with a 4 KB signature sector after
the image, on a 4 KB boundary. Its first block holds an RSA-PSS signature over
the SHA-256 of the whole image, and the public key that made it. When an
update finishes downloading, `esp_https_ota_finish()` calls `esp_ota_end()`,
which reads the signature block of the app that is RUNNING and trusts the key
in its first block. The new image's first block must carry that same key, and
the signature must verify. If either fails, the result is
`ESP_ERR_OTA_VALIDATE_FAILED`, the boot slot is not changed, the page says
"refused: not signed with the project's key, or damaged", and the board keeps
running.

The key a board trusts is therefore the key its own image was signed with.
There is no key in the chip. Nothing is burned into an eFuse, and the
bootloader does not check signatures, so any image can still be written over
the USB cable.

At start-up, an app built with the overlay looks for its own signature block.
It does not check the signature, only that a well-formed block is there. With
none, it stops with `abort()` and the message "This app is not signed, but
check signature on update is enabled in config", because it could never
accept an update. The check is in `esp_secure_boot_init_checks()`.
Every image built with the overlay must be signed before it reaches a board.

What it costs on Rev A: about 5 KB of code, 288 bytes of static RAM, and a
few KB of heap while an update is checked. The image also grows by up to 64 KB,
because the build pads it to a 64 KB boundary before the signature sector.

Builds without the overlay, which is any build from source that does not name
it (see `firmware/README.md`), have no check and no `abort()`, and accept
updates by name alone. Installing a signed release on such a board turns the
check on from then on, because the board is then running an app that has it.

Where the key lives. It is an RSA-3072 key on the project owner's computer.
It is not in the repository and not in any CI system. The release workflow
builds unsigned images and makes a draft release, which no board can see. The
owner signs the draft's images, replaces them, and publishes the draft.

Limits. There is one key, and only the first signature block counts. A board
running a signed image accepts only images whose first block carries that same
key. Moving to a new key over the air would need a bridging release, signed
with the old key, whose own code checks updates against the new key: ESP-IDF
has no way to name a second key in this mode. If the key is lost, boards that
run signed images can only be updated by cable.

## Prior art

The licence is GPL-3.0, compatible with reusing logic from existing ESP32 NUT
server projects; where code or protocol detail is taken from one, it is
attributed in the file header. NUT's own `usbhid-ups` and `cps-hid` subdriver
(GPL-2.0-or-later) are the reference for HID interpretation.
