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
