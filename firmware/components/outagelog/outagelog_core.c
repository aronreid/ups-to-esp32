/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The outage log's pure part. See outagelog_core.h; host-tested by
 * tools/test/test_outagelog.py.
 */
#include "outagelog_core.h"
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR        /* host-tested: see above */
#endif

/* Table-driven: a boot walks every record on the partition, up to 320,000.
 * In PSRAM where there is some: never used with the cache off (records are
 * sealed before esp_partition_write, and checked after). */
static EXT_RAM_BSS_ATTR uint16_t s_crc_tab[256];

uint16_t olog_crc16(const void *data, size_t len)
{
    if (s_crc_tab[1] == 0) {
        for (int i = 0; i < 256; i++) {
            uint16_t c = (uint16_t)(i << 8);
            for (int b = 0; b < 8; b++) c = (c & 0x8000) ? (uint16_t)(c << 1) ^ 0x1021 : (uint16_t)(c << 1);
            s_crc_tab[i] = c;
        }
    }
    const uint8_t *p = data;
    uint16_t crc = 0xFFFF;
    while (len--) crc = (uint16_t)(crc << 8) ^ s_crc_tab[(uint8_t)(crc >> 8) ^ *p++];
    return crc;
}

void olog_seal(olog_rec_t *r)
{
    r->magic = OLOG_MAGIC;
    r->crc = olog_crc16(r, offsetof(olog_rec_t, crc));
}

bool olog_valid(const olog_rec_t *r)
{
    return r->magic == OLOG_MAGIC &&
           r->type >= OLOG_START && r->type <= OLOG_END &&
           r->crc == olog_crc16(r, offsetof(olog_rec_t, crc));
}

bool olog_erased(const void *p, size_t len)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < len; i++) {
        if (b[i] != 0xFF) return false;
    }
    return true;
}

uint8_t olog_u8(float v)
{
    if (v < 0) return OLOG_NONE8;
    if (v > 100) return 100;
    return (uint8_t)(v + 0.5f);
}

static uint16_t scaled(float v, float k)
{
    if (v < 0) return OLOG_NONE16;
    float s = v * k + 0.5f;
    return s >= (float)(OLOG_NONE16 - 1) ? OLOG_NONE16 - 1 : (uint16_t)s;
}

uint16_t olog_x10(float v)  { return scaled(v, 10.0f); }
uint16_t olog_x100(float v) { return scaled(v, 100.0f); }
uint16_t olog_secs(float v) { return scaled(v, 1.0f); }

/* ---- the ring ---- */

static const olog_rec_t *slot_rec(const uint8_t *base, uint32_t slot)
{
    return (const olog_rec_t *)(base + (size_t)slot * OLOG_REC_SIZE);
}

/* Records go into a sector in order from its first slot, so a sector whose
 * first slot is erased is unused, and the newest record in a used one is the
 * last valid slot. A torn record still took its place, so the end of the log
 * is just past the last slot that is not erased. */
void olog_scan(const uint8_t *base, uint32_t nslots, olog_scan_t *out)
{
    uint32_t nsec = nslots / OLOG_PER_SECTOR;
    int64_t best = -1;
    uint32_t best_sec = 0;
    memset(out, 0, sizeof(*out));
    out->next_seq = 1;

    for (uint32_t s = 0; s < nsec; s++) {
        if (olog_erased(slot_rec(base, s * OLOG_PER_SECTOR), OLOG_REC_SIZE)) continue;
        for (int k = OLOG_PER_SECTOR - 1; k >= 0; k--) {
            const olog_rec_t *r = slot_rec(base, s * OLOG_PER_SECTOR + (uint32_t)k);
            if (!olog_valid(r)) continue;
            if ((int64_t)r->seq > best) {
                best = r->seq;
                best_sec = s;
            }
            break;
        }
    }
    if (best < 0) return;                       /* empty: start at slot 0 */

    uint32_t last = 0;
    for (uint32_t k = 0; k < OLOG_PER_SECTOR; k++) {
        if (!olog_erased(slot_rec(base, best_sec * OLOG_PER_SECTOR + k), OLOG_REC_SIZE)) last = k;
    }
    out->next_slot = (best_sec * OLOG_PER_SECTOR + last + 1) % nslots;
    out->next_seq = (uint32_t)best + 1;
    out->first_sector = (best_sec + 1) % nsec;
}

/* ---- the state machine ---- */

static void fill(olog_rec_t *r, olog_type_t type, uint8_t flags, uint32_t outage,
                 const olog_in_t *in, int64_t now_us)
{
    memset(r, 0, sizeof(*r));
    r->type = (uint8_t)type;
    r->flags = flags;
    r->outage = outage;
    r->uptime_s = (uint32_t)(now_us / 1000000);
    r->charge = olog_u8(in->charge);
    r->load = olog_u8(in->load);
    r->runtime_s = olog_secs(in->runtime);
    r->input_dv = olog_x10(in->vin);
    r->batt_cv = olog_x100(in->vbat);
}

void olog_resume(olog_sm_t *sm, const olog_rec_t *last, bool power_lost)
{
    sm->active = true;
    sm->outage = last->outage;
    sm->last = *last;
    sm->flags = last->flags & (OLOG_F_LOWBATT | OLOG_F_TEST);
    sm->resume = true;
    sm->resume_reason = power_lost ? OLOG_END_POWER_LOST : OLOG_END_RESTART;
    sm->last_sample_us = 0;
    sm->lost_since_us = 0;
}

/* The END for an outage whose last moments were not seen: the time and
 * readings of the last record written for it, so its length is what was
 * observed and not a guess. */
static void copied_end(olog_sm_t *sm, olog_rec_t *r, uint8_t reason)
{
    *r = sm->last;
    r->type = OLOG_END;
    r->flags = (uint8_t)((sm->flags & ~OLOG_F_REASON) | OLOG_F_COPIED | reason);
    sm->active = false;
    sm->resume = false;
}

int olog_step(olog_sm_t *sm, const olog_in_t *in, int64_t now_us, olog_rec_t out[2])
{
    int n = 0;

    if (sm->resume) {
        /* An outage was open when this board went down. Give the UPS time to
         * enumerate, then see whether it is still on battery. */
        if (in->attached && in->onbatt) {
            sm->resume = false;
            sm->lost_since_us = 0;
            if (in->lowbatt) sm->flags |= OLOG_F_LOWBATT;
            fill(&out[n], OLOG_SAMPLE, sm->flags | OLOG_F_RESUMED, sm->outage, in, now_us);
            sm->last = out[n++];
            sm->last_sample_us = now_us;
            return n;
        }
        if (in->attached || now_us >= OLOG_RESUME_US) {
            copied_end(sm, &out[n++], sm->resume_reason);
        }
        return n;
    }

    if (!sm->active) {
        if (in->attached && in->onbatt) {
            sm->active = true;
            sm->outage++;
            sm->flags = (in->testing ? OLOG_F_TEST : 0) | (in->lowbatt ? OLOG_F_LOWBATT : 0);
            sm->lost_since_us = 0;
            fill(&out[n], OLOG_START, sm->flags, sm->outage, in, now_us);
            sm->last = out[n++];
            sm->last_sample_us = now_us;
        }
        return n;
    }

    /* An outage is open. */
    if (!in->attached) {
        /* The UPS left the bus. A re-enumeration takes seconds; give it a
         * while before calling the outage over at the last time it was seen. */
        if (sm->lost_since_us == 0) sm->lost_since_us = now_us;
        if (now_us - sm->lost_since_us >= OLOG_LOST_US) {
            olog_rec_t *r = &out[n++];
            *r = sm->last;
            r->type = OLOG_END;
            r->flags = (uint8_t)((sm->flags & ~OLOG_F_REASON) | OLOG_END_UPS_LOST);
            sm->active = false;
        }
        return n;
    }
    sm->lost_since_us = 0;

    if (!in->onbatt) {
        fill(&out[n], OLOG_END, (uint8_t)(sm->flags | OLOG_END_MAINS), sm->outage, in, now_us);
        sm->last = out[n++];
        sm->active = false;
        return n;
    }

    bool lb_now = in->lowbatt && !(sm->flags & OLOG_F_LOWBATT);
    if (in->lowbatt) sm->flags |= OLOG_F_LOWBATT;
    if (lb_now || now_us - sm->last_sample_us >= OLOG_SAMPLE_US) {
        fill(&out[n], OLOG_SAMPLE, sm->flags, sm->outage, in, now_us);
        sm->last = out[n++];
        sm->last_sample_us = now_us;
    }
    return n;
}

/* ---- the index ---- */

void olog_index_init(olog_index_t *ix, olog_outage_t *store, size_t cap, uint32_t min_seq)
{
    memset(ix, 0, sizeof(*ix));
    ix->v = store;
    ix->cap = cap;
    ix->min_seq = min_seq;
}

static olog_outage_t *at(const olog_index_t *ix, size_t i_from_oldest)
{
    return &ix->v[(ix->head + i_from_oldest) % ix->cap];
}

const olog_outage_t *olog_index_get(const olog_index_t *ix, size_t i)
{
    if (i >= ix->n) return NULL;
    return at(ix, ix->n - 1 - i);
}

const olog_outage_t *olog_index_find(const olog_index_t *ix, uint32_t outage)
{
    for (size_t i = 0; i < ix->n; i++) {
        const olog_outage_t *o = olog_index_get(ix, i);
        if (o->outage == outage) return o;
    }
    return NULL;
}

static uint8_t min8(uint8_t a, uint8_t b)
{
    if (a == OLOG_NONE8) return b;
    if (b == OLOG_NONE8) return a;
    return a < b ? a : b;
}

static uint8_t max8(uint8_t a, uint8_t b)
{
    if (a == OLOG_NONE8) return b;
    if (b == OLOG_NONE8) return a;
    return a > b ? a : b;
}

void olog_index_add(olog_index_t *ix, const olog_rec_t *r, uint32_t slot)
{
    if (ix->cap == 0 || r->seq < ix->min_seq) return;

    olog_outage_t *o = ix->n ? at(ix, ix->n - 1) : NULL;
    if (!o || o->outage != r->outage) {
        /* A new outage: its START, or the first surviving record of one whose
         * START the ring has already overwritten. */
        if (ix->n == ix->cap) {
            ix->head = (ix->head + 1) % ix->cap;
            ix->n--;
        }
        o = at(ix, ix->n++);
        memset(o, 0, sizeof(*o));
        o->outage = r->outage;
        o->first_slot = slot;
        o->start_epoch = o->last_epoch = r->epoch;
        o->start_up = o->last_up = r->uptime_s;
        o->start_boot = o->last_boot = r->boot;
        o->charge0 = r->charge;
        o->charge_min = OLOG_NONE8;
        o->load_max = OLOG_NONE8;
        o->rt0 = r->runtime_s;
        o->vin0 = r->input_dv;
        ix->total++;
    } else if (r->boot == o->last_boot) {
        if (r->uptime_s > o->last_up) o->dur_s += r->uptime_s - o->last_up;
    } else if (r->epoch && o->last_epoch && r->epoch > o->last_epoch) {
        o->dur_s += r->epoch - o->last_epoch;   /* across a restart, by the clock */
    } else {
        o->gap = true;                          /* a restart of unknown length */
    }

    o->end_epoch = o->last_epoch = r->epoch;
    o->end_up = o->last_up = r->uptime_s;
    o->end_boot = o->last_boot = r->boot;
    o->charge1 = r->charge;
    o->rt1 = r->runtime_s;
    o->charge_min = min8(o->charge_min, r->charge);
    o->load_max = max8(o->load_max, r->load);
    o->flags |= r->flags & ~OLOG_F_REASON;
    o->records++;
    if (r->type == OLOG_END) o->reason = r->flags & OLOG_F_REASON;
}
