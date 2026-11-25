/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
/* The UPS's identity, kept across re-attachments (GitHub #5).
 *
 * Home Assistant keys its NUT device on manufacturer, model and serial
 * (device.* first, then ups.*), read when its integration sets up. A UPS that
 * goes quiet is port-cycled and enumerated again, and one that is flaky on
 * EP0 can miss a string read then: its serial or model came back empty, the
 * NUT variables changed, and Home Assistant made a new device with the
 * default name. The strings also choose NUT's table (bind_vars) and light
 * polling (link_recovery.h), so an empty model changed what was read at all.
 *
 * So a string that comes back empty is taken from the last attachment of the
 * same VID:PID, provided nothing that WAS read disagrees with it: a different
 * unit of the same model, swapped in, reads a different serial. RAM only: a
 * restart reads everything fresh.
 *
 * No ESP-IDF here, so tools/test/test_ups_identity.py runs it on the host. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define UPS_ID_STR 32

typedef struct {
    uint16_t vid, pid;
    char mfr[UPS_ID_STR], model[UPS_ID_STR], serial[UPS_ID_STR];
} ups_identity_t;

static inline bool ups_id_clash(const char *now, const char *was)
{
    return now[0] && was[0] && strcmp(now, was) != 0;
}

static inline bool ups_id_fill(char *now, size_t cap, const char *was)
{
    if (now[0] || !was[0]) return false;
    snprintf(now, cap, "%s", was);
    return true;
}

static inline void ups_id_note(char *last, const char *now)
{
    if (now[0]) snprintf(last, UPS_ID_STR, "%s", now);
}

/* `now` is what this attachment read (empty strings: not read). Returns true
 * when it is the last attachment's UPS, with its empty strings filled from
 * `last` (*filled says whether any were); false for another UPS, and `last`
 * starts over. `last` then holds the best known identity for next time. */
static inline bool ups_identity_keep(ups_identity_t *last, ups_identity_t *now, bool *filled)
{
    bool had = last->mfr[0] || last->model[0] || last->serial[0];
    bool same = had && now->vid == last->vid && now->pid == last->pid &&
                !ups_id_clash(now->mfr, last->mfr) && !ups_id_clash(now->model, last->model) &&
                !ups_id_clash(now->serial, last->serial);
    bool f = false;
    if (same) {
        f |= ups_id_fill(now->mfr, sizeof(now->mfr), last->mfr);
        f |= ups_id_fill(now->model, sizeof(now->model), last->model);
        f |= ups_id_fill(now->serial, sizeof(now->serial), last->serial);
    } else {
        memset(last, 0, sizeof(*last));
    }
    last->vid = now->vid;
    last->pid = now->pid;
    ups_id_note(last->mfr, now->mfr);
    ups_id_note(last->model, now->model);
    ups_id_note(last->serial, now->serial);
    if (filled) *filled = f;
    return same;
}
