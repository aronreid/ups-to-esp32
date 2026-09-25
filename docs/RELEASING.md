# Releasing firmware

Every rule here was written after something broke. The reasoning is kept with
each one, because a rule whose reason is forgotten gets removed by the next
person in a hurry.

## The one that matters

**A device in a closet has no cable.** Everything below follows from that. A
bad release does not mean a bad afternoon; it means someone drives to a site,
or a UPS goes unmonitored until they do. The cost of an extra check before
publishing is minutes. The cost of skipping one is measured in journeys.

## What already shipped, and would have been a field incident

| Release | Fault | Why nothing caught it |
|---|---|---|
| `v0.01-beta` | Boot loop: twelve URI handlers registered into a table sized for eight, `ESP_ERROR_CHECK` turned the ninth into `abort()` | Compiled clean, CI green. Nothing ran it. |
| `v0.02-beta` | Reset on any HTTP request: `get_status` outgrew the web server's 4 KB task stack | Same. It surfaced only when a UPS was attached and the response got longer. |
| `v0.02*` | `ups-adaptor-freenove.bin` was a **Rev A build**: the release job reused the first target's `sdkconfig` | Both assets differed by timestamp, so a byte comparison passed. Only reading the image's own identity catches it. |
| `v0.02b` | Boots and serves, but **cannot fetch its own updates**: a 512-byte header buffer could not hold GitHub's redirect | The failure is in the update path itself, so the fix cannot be delivered the usual way. |
| `v0.06` (first tag) | Both images said **`v0.06-rc2`**: tagged on the commit the candidate was already on, and `git describe` picked the rc tag. Pulled back to a prerelease within minutes, before any download | The version gate had never worked: it grepped esptool for the wrong label in a file that no longer existed, read nothing, and treated nothing as a match. |

The third is the worst class: a **one-way door**. It works well enough to look
fine and quietly removes the ability to repair it remotely.

## Rules

**1. No production tag without a release candidate that ran on hardware.**
Tag `vX.Y-rcN` first. The workflow publishes it as a *prerelease*, and
GitHub's `/releases/latest` — the endpoint every device asks — excludes
prereleases. A candidate is therefore invisible to the fleet by construction.
Flash it to a bench board, work through the smoke list below, and only then
tag the production version.

**2. The candidate must be able to update itself.** Before promoting, make the
bench board install *another* release over the air. A build that cannot do this
is a one-way door, and `v0.02b` proved the failure is not hypothetical. This is
the single most important check here and the easiest to skip.

**3. Rollback must be verified, never assumed.** CI fails the release if
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` is absent from the generated
`sdkconfig`. It was in `sdkconfig.defaults` for days while every local build
lacked it, because ESP-IDF reads that file only when generating `sdkconfig` and
the file already existed. Everything claimed about self-recovery was false in
that window.

**4. Confirmation must be earned.** A new image cancels its own rollback only
after **five minutes of uptime and three HTTP requests served**. Associating
with Wi-Fi proves the radio works and nothing else; both shipped bugs above let
a device boot, associate, and mark itself healthy seconds before dying.

**5. Never delete a release a device might still be running.** Supersede it.
Deleting one that a device has installed leaves that device reporting a version
no release matches, and being offered the "new" lower version forever.

Deleting a *superseded* one is a different act and is allowed while the fleet is
small enough to know. On 2026-09-21 everything before `v0.04` was removed --
`v0.02b`, `v0.02c`, `v0.03-rc1`, `v0.03-rc2`, `v0.03` -- with one bench board in
existence and that board on `v0.04`. Four of the five were known-broken
(`v0.02b` could not fetch updates, `v0.02c` shipped a Rev A image under the
devkit's name, `v0.03` never cancelled its rollback), and leaving a broken
release downloadable is its own hazard.

What the deletion costs: the asset URLs stop resolving, so a device cannot
re-fetch an old image. Flash slots are untouched, so a rollback that is already
written still works. The record of what each version contained lives in the
commit history, not in the releases page.

Once more than one device exists, this rule hardens back to its title: check
what is deployed before deleting anything.

**6. The image version must equal the tag.** CI fails otherwise -- including
when it cannot read a version at all, which it used to wave through. The
release builds stamp the version from the pushed tag (`-DPROJECT_VER`), never
from `git describe`: promoting a candidate means a second tag on the same
commit, and describe is free to pick either. They are
compared on-device, so a mismatch makes every board install a release and still
believe it needs it.

**7. Both target images must exist and differ.** Identical binaries mean the
board configuration did not apply, so one target has the wrong pin map — silent
until hardware. CI fails on it.

**8. An image must BE the board it is named for.** Every image declares its
board in its app descriptor's project name -- `ups-adaptor-reva`,
`ups-adaptor-freenove` -- and **the OTA client refuses to install one that does
not match its own**. The filename is only a label; a correctly named asset
containing the wrong build has already shipped, and a devkit installed it and
came up believing it had a load switch on GPIO4. CI checks the same thing from
two independent places, and both workflows now delete `firmware/sdkconfig`
between targets, because ESP-IDF reads `SDKCONFIG_DEFAULTS` only when
generating it and a leftover file makes the second build inherit the first
one's board.

**When a new board revision arrives**, it needs three things and nothing else:
a `BOARD_OTA_TARGET` and `BOARD_IMAGE_NAME` in `board.h`, a build step in the
release workflow with its own `-DUPSA_IMAGE_NAME`, and an entry in the identity
gate. A device only ever accepts firmware naming its own board, so an older
unit cannot take a Rev B image no matter what is published or how it is named.

**9. An image may not exceed 80% of its slot.** It installs to the *other*
slot, so a near-full image still works today; the gate is about leaving room
for the growth that got it there.

**10. A refusal has to reach the person, not the log.** Anything the firmware
declines to do for someone's protection must say so where they are looking. The
identity guard in rule 8 worked perfectly on the first release it caught and was
still reported as a broken page, because the only place it explained itself was
a serial console, while the status page went on offering the same Install button
every twelve hours. A protection nobody can see is indistinguishable from a bug,
and the person who cannot tell the difference is the one who decides whether to
trust the next update.

## Smoke list for a release candidate

On a board, with a UPS attached, before promoting:

- [ ] Boots once. `tools/serial-check.py` reports one boot and no panics.
- [ ] Stays up. Ten minutes, no resets, and the status page agrees.
- [ ] Serves. The page loads and `/api/status` answers repeatedly — the
      stack-overflow bug needed only one request to show.
- [ ] Reads the UPS. Values match what the front panel says.
- [ ] Speaks NUT. `LIST VAR ups` returns readings.
- [ ] **Updates itself.** Install another release over the air end to end.
- [ ] Recovers. Unplug and replug the UPS; readings come back.
- [ ] Reports honestly. `last_reset` and uptime are right after a deliberate
      reset.
- [ ] Refuses honestly. Point it at a release built for the other board; the
      page must say so in words and stop offering it, not just log it.

## Publishing

```sh
tools/publish.sh --push                                       # rebuild the public tree
git tag -a v0.03-rc1 -m "..." && git push github v0.03-rc1   # invisible to devices
#   ... flash it, work the smoke list, including rule 2 ...
git tag -a v0.03 -m "..."     && git push github v0.03       # every device is offered this
```
