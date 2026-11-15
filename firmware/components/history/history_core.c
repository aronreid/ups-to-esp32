/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Long history's pure part. See history_core.h; host-tested by
 * tools/test/test_history.py.
 */
#include "history_core.h"
#include <string.h>

uint16_t hist_crc16(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint16_t crc = 0xFFFF;
    while (len--) {
        crc ^= (uint16_t)*p++ << 8;
        for (int i = 0; i < 8; i++) {
            crc = (crc & 0x8000) ? (uint16_t)(crc << 1) ^ 0x1021 : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

void hist_seal(hist_hdr_t *h)
{
    h->magic = HIST_MAGIC;
    h->crc = hist_crc16(h, offsetof(hist_hdr_t, crc));
}

bool hist_hdr_valid(const hist_hdr_t *h)
{
    return h->magic == HIST_MAGIC && h->period_s != 0 &&
           h->crc == hist_crc16(h, offsetof(hist_hdr_t, crc));
}

bool hist_smp_written(const hist_smp_t *s)
{
    return !(s->flags & HIST_F_UNWRITTEN);
}

static uint8_t pct(float v)
{
    if (v < 0) return HIST_NONE8;
    if (v > 100) return 100;
    return (uint8_t)(v + 0.5f);
}

static uint16_t u16(float v, float k)
{
    if (v < 0) return HIST_NONE16;
    float s = v * k + 0.5f;
    return s >= (float)(HIST_NONE16 - 1) ? HIST_NONE16 - 1 : (uint16_t)s;
}

hist_smp_t hist_encode(const hist_in_t *in)
{
    hist_smp_t s;
    s.flags = (uint8_t)((in->present ? HIST_F_PRESENT : 0) | (in->ol ? HIST_F_OL : 0) |
                        (in->ob ? HIST_F_OB : 0) | (in->lb ? HIST_F_LB : 0) |
                        (in->chrg ? HIST_F_CHRG : 0));
    if (!in->present) {
        s.charge = s.load = s.watts_10 = HIST_NONE8;
        s.vin_dv = s.runtime_10s = HIST_NONE16;
        return s;
    }
    s.charge = pct(in->charge);
    s.load = pct(in->load);
    s.vin_dv = u16(in->vin, 10.0f);
    s.runtime_10s = u16(in->runtime, 0.1f);
    if (in->watts < 0) s.watts_10 = HIST_NONE8;
    else {
        float w = in->watts / 10.0f + 0.5f;
        s.watts_10 = w >= 254.0f ? 254 : (uint8_t)w;
    }
    return s;
}

uint32_t hist_count(const uint8_t *sector)
{
    const hist_smp_t *s = (const hist_smp_t *)(sector + sizeof(hist_hdr_t));
    uint32_t lo = 0, hi = HIST_PER_SECTOR;
    /* Written readings are a prefix: binary search for its end. */
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (hist_smp_written(&s[mid])) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

int32_t hist_newest(const uint8_t *base, uint32_t nsec, uint32_t *seq)
{
    int32_t best = -1;
    uint32_t bseq = 0;
    for (uint32_t i = 0; i < nsec; i++) {
        const hist_hdr_t *h = (const hist_hdr_t *)(base + (size_t)i * HIST_SECTOR);
        if (!hist_hdr_valid(h)) continue;
        if (best < 0 || h->seq > bseq) {
            best = (int32_t)i;
            bseq = h->seq;
        }
    }
    if (seq) *seq = bseq;
    return best;
}

void hist_agg_init(hist_agg_t *a, hist_bucket_t *store, uint32_t nb, uint32_t t0, uint32_t step)
{
    a->t0 = t0;
    a->step = step ? step : 1;
    a->nb = nb;
    a->b = store;
    memset(store, 0, nb * sizeof(*store));
    for (uint32_t i = 0; i < nb; i++) store[i].vin_min = store[i].vin_max = HIST_NONE16;
}

void hist_agg_add(hist_agg_t *a, uint32_t epoch, const hist_smp_t *s)
{
    if (epoch < a->t0) return;
    uint32_t i = (epoch - a->t0) / a->step;
    if (i >= a->nb) return;
    hist_bucket_t *b = &a->b[i];
    b->n++;
    b->flags_or |= s->flags;
    if (!(s->flags & HIST_F_PRESENT)) return;
    if (s->charge != HIST_NONE8) { b->charge_sum += s->charge; b->charge_n++; }
    if (s->load != HIST_NONE8)   { b->load_sum += s->load;     b->load_n++; }
    if (s->watts_10 != HIST_NONE8) { b->watts_sum += s->watts_10; b->watts_n++; }
    if (s->vin_dv != HIST_NONE16) {
        if (b->vin_min == HIST_NONE16 || s->vin_dv < b->vin_min) b->vin_min = s->vin_dv;
        if (b->vin_max == HIST_NONE16 || s->vin_dv > b->vin_max) b->vin_max = s->vin_dv;
    }
}

/* A sector with no UTC time can only be placed if it is this boot's, by its
 * uptime. Which boot it is came from its boot number, but that is 0 for
 * every boot of a board whose NVS would not open, so sectors from earlier
 * boots were placed as this one's, hours or days out. Sector numbers only
 * grow, and every sector this boot begins is numbered past the newest one
 * found at its start: that says "this boot" whatever NVS did. */
uint32_t hist_sector_time(const hist_hdr_t *h, uint32_t seq0, bool clock,
                          uint32_t now_t, uint32_t up)
{
    bool mine = h->seq > seq0;
    if (clock) {
        if (h->epoch) return h->epoch;
        if (!mine) return 0;
        return h->uptime_s <= up ? now_t - (up - h->uptime_s) : 0;
    }
    return mine ? h->uptime_s + 1 : 0;              /* +1: uptime 0 is a real time */
}
