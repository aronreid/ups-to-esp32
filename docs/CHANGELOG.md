# Changelog

Every user-facing change lands here, under "Unreleased", in the same commit
that makes the change. Not afterward: `v0.10` shipped release notes
describing only its final `-rc`'s fix, because whoever wrote the notes
looked at the last candidate's diff instead of the whole interval since
`v0.09`. The login system, the fixed address and a USB crash fix had all
landed too and were simply left out. Writing the bullet at the same time as
the change means there is nothing left to reconstruct at release time:
`tools/release.sh` requires this section to be non-empty, and read, before it
accepts a production tag.

When a production tag ships, "Unreleased" is renamed to that version and a
fresh empty "Unreleased" goes above it. An `-rc` tag does not touch this file
beyond whatever the commits it contains have already added: its own notes are
just its build's delta, not the whole story, the same way `docs/RELEASING.md`
already treats prereleases.

## Unreleased

- New test target: the Waveshare ESP32-P4-ETH board on PoE, a wired board with
  no Wi-Fi. Release asset `ups-adaptor-p4poe-full.bin`. It has only been run
  on the bench with no UPS attached so far.
- On a wired board the web page leaves out the Wi-Fi section and shows
  "Ethernet" in the debug panel.

## v0.14.4 -- 2026-09-30

- The board now also listens for the reports a UPS sends the moment
  something changes, as NUT does, so a switch to battery shows up straight
  away rather than at the next poll.
- Liebert UPSes now get ups.status from NUT's own logic too: on mains, on
  battery, charging, low battery, replace battery and shutdown imminent.

## v0.14.3 -- 2026-09-30

- CyberPower UPSes now report battery voltage, with NUT's own corrections:
  some CP-series units (USB ID 0764:0501) report battery voltage 1.5 times
  too high and frequency in tenths of a hertz, and the board now fixes both
  as NUT does. Battery charge is capped at 100%.
- ups.status is now built exactly the way NUT's own driver builds it, from
  NUT's per-make status tables, so it can report TRIM, BOOST, BYPASS and CAL
  as well as on mains, on battery, low battery, overload and replace battery.
  NUT's ups.alarm messages (for example "Replace battery!") and
  input.transfer.reason are published too. A UPS whose table does not give
  its power state keeps the board's previous status logic.
- UPSes that present more than one USB HID interface are now found on the
  right one. Some (the Liebert PSI5, for one) put a different device first,
  and the board used to talk to that instead of the UPS.
- NUT clients now get each value the way NUT itself reads it for your make of
  UPS, where the two differ: for example dates as 2014/02/21.
- A battery voltage far from the battery's own rated voltage (under half, or
  over 1.6 times) is dropped rather than published.

## v0.14.2 -- 2026-09-30

- Many more readings from your UPS, over NUT and on the page ("More from
  this UPS"). The board now uses the same per-make tables NUT itself uses, and
  publishes everything your UPS actually reports: nominal voltages and power,
  battery type and dates, low-battery and runtime warnings, transfer
  thresholds, timers, and on Eaton units efficiency, energy saving and outlet
  status. An APC Back-UPS now gives 27 variables to a NUT client, up from 21.
  Checked against 2,952 values NUT itself decoded from 48 real UPSes: every one
  matches.
- Fixed: on APC UPSes, battery.mfr.date showed the UPS's own manufacture date,
  not the battery's, and "set battery date" wrote over the UPS's date.
- Fixed: 32-bit timers that mean "no countdown" (-1) showed as 4294967295 on
  some Eaton, APC Smart-UPS and PowerWalker units.
- Values no working UPS reports (a 41216 V output, a 120% charge) are dropped
  rather than published.
- The Liebert GXT5 now reports runtime and on-mains/on-battery status. Its
  USB description switches usage tables mid-way, and the board read a status
  flag as the UPS's power rating. The board now follows the same rule as NUT.

## v0.14.1 -- 2026-09-30

- Fixed: on CyberPower UPSes, "turn the load off" with a delay waited 60 times
  too long. The page's default of 60 seconds meant an hour. These UPSes count
  the delay in minutes, and the board now converts it, rounding up so the
  outlets never go off sooner than asked. Readings are unaffected: in 65 real
  UPS descriptors, only the shutdown and startup delays use this conversion.

## v0.14 -- 2026-09-30

- Many more UPSes can now be read, including every Eaton tested, APC
  Smart-UPS models on USB ID 051d:0003 (Smart-UPS, SRT, Smart-UPS C),
  several CyberPower CP, OR and PR models, PowerWalker, IBM and Salicru.
  These UPSes mark their readings as fixed values in their USB description,
  and the board wrongly skipped them as padding, so it found no charge,
  runtime or status. Checked against 65 real UPS descriptors published on the
  NUT project's issue tracker: 55 now give charge, runtime and status, up
  from 29. Most of the rest are UPSes that do not speak USB HID at all.

## v0.13 -- 2026-09-30

- A UPS that keeps crashing the board can no longer knock it off the network.
  After three restarts in a row caused by the UPS, the board still identifies
  it but stops reading it; after five, it turns USB off. Either way it stays
  online, can be updated, and says so on its page, with a Try again button.
- When the board has no UPS readings (none attached, not read yet, or one it
  cannot read), NUT now answers "data stale", as a real NUT driver does,
  instead of reporting the UPS as OFF. Some NUT clients treat a UPS that stays
  OFF as critical and could shut a computer down.
- UPSes whose USB descriptor uses no report IDs can now be read. Before, the
  board rejected almost every reply from them and waited for a status forever.
- Values that sit past the 32nd byte of a report are now read, up to 63.
- A UPS the board has nothing to read from is no longer power-cycled over and
  over as if it had stopped answering.

## v0.12 -- 2026-09-30

- Fixed: some UPSes, including the APC Back-UPS BE600M1, made the board
  restart every couple of seconds and drop off the network as soon as they
  were plugged in. They answer a request with more data than was asked for,
  and the board now makes room for that instead of crashing. A board already
  stuck this way cannot update itself while the UPS is connected: unplug the
  UPS's USB cable, let the board update, then plug it back in.
- A UPS the board can't read now gets fixed faster. If a UPS is attached but
  gives no readings for five minutes, and the check-in is on, the board sends
  that UPS's USB report descriptor, make, model and USB ID to the project once.
  No readings, no serial number, nothing about your network.
- A "send my UPS info to the developer" button on the page sends the same
  thing on request, even with the check-in off.

## v0.11 -- 2026-09-29

- A real firmware target for a Rev A board with a VCC-first OLED module
  fitted (`reva-oled`), released alongside the plain build. Previously this
  needed a hand-built bench flag; now it is a normal board choice with its
  own OTA identity, so a board flashed with it can update itself instead of
  needing a cable every time. A plain Rev A board can never be offered it.

## v0.10 -- 2026-09-28

- A login is now required for every change, web and NUT. Set during Wi-Fi
  setup, or on the page's Login card if updating from an older version.
  Every write needs it: UPS commands, Wi-Fi, updates, reboot, and NUT's
  INSTCMD and SET VAR. Watching the UPS never does.
- A fixed IPv4 address, in Wi-Fi setup or a new Address card, for a NAS or
  Home Assistant pointed at the board's IP. Checked before it's saved: the
  router has to answer a ping from it.
- The page now warns when a UPS's "deep test" isn't a real one. Some models
  just rerun their short test; the page says so and shows the real duration
  for ones that do run a long one.
- Fixed a crash: a UPS control request that took over a second could free
  memory the USB stack still held, crashing the board later on an unrelated
  allocation.
- The page ran stale against new firmware if a tab was left open across an
  update. It now reloads itself when the firmware version changes, and is
  never cached.
- Failures on the page all read as one generic message. Now: "can't reach
  the board" versus a reply it can't parse versus a locked-out login, each
  said plainly.
- Fixed a false OFF over NUT: some UPSes read back all-zero status flags for
  a minute or two after a self-test, which used to map straight to OFF.
- A UPS's report cache has been seen to wedge shortly after a test starts.
  The board now power-cycles the USB link and re-reads right after a test
  command.
- Fixed a status-page header that shifted every second as its text changed
  length.
