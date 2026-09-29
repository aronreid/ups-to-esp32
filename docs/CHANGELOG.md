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
