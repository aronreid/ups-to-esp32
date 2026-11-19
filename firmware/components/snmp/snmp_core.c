/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * SNMP v1/v2c agent and trap encoder, no ESP-IDF (snmp_core.h).
 *
 * Every length read off the wire is checked against what is left of the
 * datagram before it is used, and nothing is allocated: a request is parsed
 * in place, and the answer is encoded twice, once to measure it and once to
 * write it, from a list of at most SNMP_RESP_VB_MAX small records.
 */
#include "snmp_core.h"
#include "ups_nutstatus.h"
#include <stdio.h>
#include <string.h>

/* ---- BER tags ----------------------------------------------------------- */

#define T_INT       0x02
#define T_OCTETS    0x04
#define T_NULL      0x05
#define T_OID       0x06
#define T_SEQ       0x30
#define T_COUNTER   0x41
#define T_GAUGE     0x42
#define T_TICKS     0x43
#define X_NOOBJ     0x80        /* noSuchObject   */
#define X_NOINST    0x81        /* noSuchInstance */
#define X_END       0x82        /* endOfMibView   */

#define PDU_GET     0xA0
#define PDU_NEXT    0xA1
#define PDU_RESP    0xA2
#define PDU_SET     0xA3
#define PDU_BULK    0xA5
#define PDU_TRAP2   0xA7

#define E_TOOBIG       1
#define E_NOSUCHNAME   2
#define E_NOTWRITABLE  17

/* ---- reading ------------------------------------------------------------ */

typedef struct { const uint8_t *p, *end; } rd_t;

/* One tag and length. Single-byte tags only (all SNMP uses), and lengths of
 * at most two bytes: nothing larger fits a datagram this board accepts. */
static bool rd_tl(rd_t *r, uint8_t *tag, size_t *len)
{
    if (r->end - r->p < 2) return false;
    *tag = *r->p++;
    if ((*tag & 0x1F) == 0x1F) return false;
    uint8_t b = *r->p++;
    size_t l = b;
    if (b & 0x80) {
        int n = b & 0x7F;
        if (n < 1 || n > 2 || r->end - r->p < n) return false;
        l = 0;
        while (n--) l = (l << 8) | *r->p++;
    }
    if (l > (size_t)(r->end - r->p)) return false;
    *len = l;
    return true;
}

/* The next element, which must have tag `want`; `in` is its contents. */
static bool rd_want(rd_t *r, uint8_t want, rd_t *in)
{
    uint8_t tag;
    size_t len;
    if (!rd_tl(r, &tag, &len) || tag != want) return false;
    in->p = r->p;
    in->end = r->p + len;
    r->p += len;
    return true;
}

static bool rd_int(rd_t *r, int32_t *v, rd_t *raw)
{
    rd_t in;
    if (!rd_want(r, T_INT, &in)) return false;
    size_t n = (size_t)(in.end - in.p);
    if (n < 1 || n > 4) return false;
    uint32_t u = (in.p[0] & 0x80) ? 0xFFFFFFFFu : 0;
    for (size_t i = 0; i < n; i++) u = (u << 8) | in.p[i];
    *v = (int32_t)u;
    if (raw) *raw = in;
    return true;
}

/* OID contents to sub-identifiers. False if malformed or too long. */
static bool oid_decode(const uint8_t *p, size_t n, uint32_t *sub, size_t *nsub)
{
    size_t k = 0;
    uint32_t acc = 0;
    int bytes = 0;
    if (n == 0) return false;
    for (size_t i = 0; i < n; i++) {
        if (bytes == 0 && p[i] == 0x80) return false;        /* non-minimal */
        if (++bytes > 5 || (acc >> 25)) return false;         /* > 32 bits   */
        acc = (acc << 7) | (p[i] & 0x7F);
        if (p[i] & 0x80) continue;
        if (k == 0) {
            if (SNMP_OID_MAX < 2) return false;
            sub[0] = acc < 40 ? 0 : acc < 80 ? 1 : 2;
            sub[1] = acc - sub[0] * 40;
            k = 2;
        } else {
            if (k >= SNMP_OID_MAX) return false;
            sub[k++] = acc;
        }
        acc = 0;
        bytes = 0;
    }
    if (bytes) return false;                                  /* cut off */
    *nsub = k;
    return true;
}

/* ---- writing ------------------------------------------------------------ */

/* With b NULL it only counts, so the same code measures and then writes. */
typedef struct { uint8_t *b; size_t cap, n; } wr_t;

static void w_byte(wr_t *w, uint8_t c)
{
    if (w->b && w->n < w->cap) w->b[w->n] = c;
    w->n++;
}

static void w_bytes(wr_t *w, const void *p, size_t n)
{
    for (size_t i = 0; i < n; i++) w_byte(w, ((const uint8_t *)p)[i]);
}

static size_t len_size(size_t l) { return l < 0x80 ? 1 : l < 0x100 ? 2 : 3; }
static size_t tlv_size(size_t l) { return 1 + len_size(l) + l; }

static void w_tl(wr_t *w, uint8_t tag, size_t l)
{
    w_byte(w, tag);
    if (l < 0x80) {
        w_byte(w, (uint8_t)l);
    } else if (l < 0x100) {
        w_byte(w, 0x81);
        w_byte(w, (uint8_t)l);
    } else {
        w_byte(w, 0x82);
        w_byte(w, (uint8_t)(l >> 8));
        w_byte(w, (uint8_t)l);
    }
}

static size_t int_clen(int32_t v)
{
    size_t n = 4;
    while (n > 1) {
        int32_t top9 = v >> (8 * n - 9);          /* the top byte and the next bit */
        if (top9 != 0 && top9 != -1) break;
        n--;
    }
    return n;
}

static void w_int(wr_t *w, uint8_t tag, int32_t v)
{
    size_t n = int_clen(v);
    w_tl(w, tag, n);
    for (size_t i = n; i-- > 0; ) w_byte(w, (uint8_t)((uint32_t)v >> (8 * i)));
}

static size_t uint_clen(uint32_t v)
{
    size_t n = 1;
    while (n < 4 && (v >> (8 * n))) n++;
    if ((v >> (8 * n - 1)) & 1) n++;              /* a leading 0 keeps it positive */
    return n;
}

static void w_uint(wr_t *w, uint8_t tag, uint32_t v)
{
    size_t n = uint_clen(v);
    w_tl(w, tag, n);
    for (size_t i = n; i-- > 0; ) w_byte(w, i >= 4 ? 0 : (uint8_t)(v >> (8 * i)));
}

/* The OIDs this board names are all 1.3.x with every arc under 128, so
 * their encoding is one byte each after the first two arcs, which share one. */
static size_t oid8_clen(const uint8_t *sub, size_t n) { (void)sub; return n - 1; }

static void w_oid8(wr_t *w, const uint8_t *sub, size_t n)
{
    w_tl(w, T_OID, oid8_clen(sub, n));
    w_byte(w, (uint8_t)(sub[0] * 40 + sub[1]));
    for (size_t i = 2; i < n; i++) w_byte(w, sub[i]);
}

/* ---- values and varbinds ------------------------------------------------ */

#define OID8_MAX 14

typedef struct {
    uint8_t     tag;
    int32_t     i;              /* T_INT */
    uint32_t    u;              /* counter, gauge, ticks */
    const char *s;              /* T_OCTETS */
    uint8_t     oid[OID8_MAX];  /* T_OID */
    uint8_t     olen;
} val_t;

static size_t val_clen(const val_t *v)
{
    switch (v->tag) {
    case T_INT:     return int_clen(v->i);
    case T_COUNTER: case T_GAUGE: case T_TICKS: return uint_clen(v->u);
    case T_OCTETS:  return strlen(v->s);
    case T_OID:     return oid8_clen(v->oid, v->olen);
    default:        return 0;   /* NULL and the three exceptions */
    }
}

static void w_val(wr_t *w, const val_t *v)
{
    switch (v->tag) {
    case T_INT:     w_int(w, T_INT, v->i); break;
    case T_COUNTER: case T_GAUGE: case T_TICKS: w_uint(w, v->tag, v->u); break;
    case T_OCTETS:  w_tl(w, T_OCTETS, strlen(v->s)); w_bytes(w, v->s, strlen(v->s)); break;
    case T_OID:     w_oid8(w, v->oid, v->olen); break;
    default:        w_tl(w, v->tag, 0); break;
    }
}

/* A varbind's name is either one of ours (oid8) or a request's own bytes,
 * echoed back as they came (raw). */
typedef struct {
    const uint8_t *raw;
    size_t         rawlen;
    uint8_t        oid[OID8_MAX];
    uint8_t        olen;
    val_t          v;
} vb_t;

static size_t vb_clen(const vb_t *b)
{
    size_t name = b->raw ? b->rawlen : oid8_clen(b->oid, b->olen);
    return tlv_size(name) + tlv_size(val_clen(&b->v));
}

static void w_vb(wr_t *w, const vb_t *b)
{
    w_tl(w, T_SEQ, vb_clen(b));
    if (b->raw) {
        w_tl(w, T_OID, b->rawlen);
        w_bytes(w, b->raw, b->rawlen);
    } else {
        w_oid8(w, b->oid, b->olen);
    }
    w_val(w, &b->v);
}

/* ---- the MIB ------------------------------------------------------------ */

enum {
    K_SYS_DESCR, K_SYS_OBJID, K_SYS_UPTIME, K_SYS_CONTACT, K_SYS_NAME, K_SYS_LOCATION,
    K_SYS_SERVICES,
    K_ID_MFR, K_ID_MODEL, K_ID_UPSFW, K_ID_AGENTFW, K_ID_NAME, K_ID_ATTACHED,
    K_B_STATUS, K_B_SECS, K_B_MINUTES, K_B_CHARGE, K_B_VOLTS, K_B_TEMP,
    K_IN_BADS, K_IN_LINES, K_IN_FREQ, K_IN_VOLTS, K_IN_AMPS,
    K_OUT_SOURCE, K_OUT_FREQ, K_OUT_LINES, K_OUT_VOLTS, K_OUT_AMPS, K_OUT_WATTS, K_OUT_LOAD,
    K_AL_PRESENT, K_AL_DESCR, K_AL_TIME,
    K_TEST_ID, K_TEST_SPINLOCK, K_TEST_SUMMARY, K_TEST_DETAIL, K_TEST_START, K_TEST_ELAPSED,
    K_CFG_IN_V, K_CFG_IN_FREQ, K_CFG_OUT_V, K_CFG_OUT_FREQ, K_CFG_OUT_VA, K_CFG_OUT_W,
    K_CFG_LOW_BATT, K_CFG_AUDIBLE, K_CFG_XFER_LO, K_CFG_XFER_HI,
};

typedef struct {
    uint8_t len;
    uint8_t sub[OID8_MAX];
    uint8_t kind;
    uint8_t arg;
} mib_t;

#define SYS(n, k)          { 9,  { 1,3,6,1,2,1,1,n,0 }, k, 0 }
#define UPS(g, n, k)       { 11, { 1,3,6,1,2,1,33,1,g,n,0 }, k, 0 }
/* upsInputTable is upsInput.3 and upsOutputTable upsOutput.4: the table's
 * arc equals its group's. One line each, index 1. */
#define UPS_COL(g, c, k)   { 13, { 1,3,6,1,2,1,33,1,g,g,1,c,1 }, k, 0 }
#define ALARM(c, id, k)    { 13, { 1,3,6,1,2,1,33,1,6,2,1,c,id }, k, id }

/* In OID order: a walk is this list from top to bottom, less what the UPS
 * does not report. tools/test/test_snmp.py checks the order. */
static const mib_t MIB[] = {
    SYS(1, K_SYS_DESCR), SYS(2, K_SYS_OBJID), SYS(3, K_SYS_UPTIME), SYS(4, K_SYS_CONTACT),
    SYS(5, K_SYS_NAME), SYS(6, K_SYS_LOCATION), SYS(7, K_SYS_SERVICES),

    UPS(1, 1, K_ID_MFR), UPS(1, 2, K_ID_MODEL), UPS(1, 3, K_ID_UPSFW), UPS(1, 4, K_ID_AGENTFW),
    UPS(1, 5, K_ID_NAME), UPS(1, 6, K_ID_ATTACHED),

    UPS(2, 1, K_B_STATUS), UPS(2, 2, K_B_SECS), UPS(2, 3, K_B_MINUTES), UPS(2, 4, K_B_CHARGE),
    UPS(2, 5, K_B_VOLTS), UPS(2, 7, K_B_TEMP),

    UPS(3, 1, K_IN_BADS), UPS(3, 2, K_IN_LINES),
    UPS_COL(3, 2, K_IN_FREQ), UPS_COL(3, 3, K_IN_VOLTS), UPS_COL(3, 4, K_IN_AMPS),

    UPS(4, 1, K_OUT_SOURCE), UPS(4, 2, K_OUT_FREQ), UPS(4, 3, K_OUT_LINES),
    UPS_COL(4, 2, K_OUT_VOLTS), UPS_COL(4, 3, K_OUT_AMPS), UPS_COL(4, 4, K_OUT_WATTS),
    UPS_COL(4, 5, K_OUT_LOAD),

    UPS(6, 1, K_AL_PRESENT),
    /* One row per alarm this board can raise, ascending; column 2, then 3. */
#define ALARM_ROWS(c, k) \
    ALARM(c, SNMP_ALARM_BATTERY_BAD, k),      ALARM(c, SNMP_ALARM_ON_BATTERY, k), \
    ALARM(c, SNMP_ALARM_LOW_BATTERY, k),      ALARM(c, SNMP_ALARM_DEPLETED_BATTERY, k), \
    ALARM(c, SNMP_ALARM_TEMP_BAD, k),         ALARM(c, SNMP_ALARM_OUTPUT_OVERLOAD, k), \
    ALARM(c, SNMP_ALARM_ON_BYPASS, k),        ALARM(c, SNMP_ALARM_CHARGER_FAILED, k), \
    ALARM(c, SNMP_ALARM_UPS_OUTPUT_OFF, k),   ALARM(c, SNMP_ALARM_FAN_FAILURE, k), \
    ALARM(c, SNMP_ALARM_GENERAL_FAULT, k),    ALARM(c, SNMP_ALARM_TEST_FAILED, k), \
    ALARM(c, SNMP_ALARM_COMMS_LOST, k),       ALARM(c, SNMP_ALARM_AWAITING_POWER, k), \
    ALARM(c, SNMP_ALARM_SHUTDOWN_IMMINENT, k), ALARM(c, SNMP_ALARM_TEST_IN_PROGRESS, k)
    ALARM_ROWS(2, K_AL_DESCR),
    ALARM_ROWS(3, K_AL_TIME),

    UPS(7, 1, K_TEST_ID), UPS(7, 2, K_TEST_SPINLOCK), UPS(7, 3, K_TEST_SUMMARY),
    UPS(7, 4, K_TEST_DETAIL), UPS(7, 5, K_TEST_START), UPS(7, 6, K_TEST_ELAPSED),

    UPS(9, 1, K_CFG_IN_V), UPS(9, 2, K_CFG_IN_FREQ), UPS(9, 3, K_CFG_OUT_V),
    UPS(9, 4, K_CFG_OUT_FREQ), UPS(9, 5, K_CFG_OUT_VA), UPS(9, 6, K_CFG_OUT_W),
    UPS(9, 7, K_CFG_LOW_BATT), UPS(9, 8, K_CFG_AUDIBLE), UPS(9, 9, K_CFG_XFER_LO),
    UPS(9, 10, K_CFG_XFER_HI),
};
#define MIB_N (sizeof(MIB) / sizeof(MIB[0]))

static const uint8_t OID_UPS_MIB[]      = { 1,3,6,1,2,1,33 };
static const uint8_t OID_WELL_KNOWN[]   = { 1,3,6,1,2,1,33,1,6,3 };   /* + alarm id */
static const uint8_t OID_UPS_TRAPS[]    = { 1,3,6,1,2,1,33,2 };       /* + trap number */
static const uint8_t OID_SNMP_TRAPS[]   = { 1,3,6,1,6,3,1,1,5 };      /* + 1 cold, 2 warm */
static const uint8_t OID_SYSUPTIME0[]   = { 1,3,6,1,2,1,1,3,0 };
static const uint8_t OID_TRAPOID0[]     = { 1,3,6,1,6,3,1,1,4,1,0 };
static const uint8_t OID_ALARM_ID[]     = { 1,3,6,1,2,1,33,1,6,2,1,1 }; /* + id */
static const uint8_t OID_NO_TESTS[]     = { 1,3,6,1,2,1,33,1,7,7,1 };  /* upsTestNoTestsInitiated */

static void set_oid(uint8_t *dst, uint8_t *dlen, const uint8_t *pre, size_t n, int last)
{
    memcpy(dst, pre, n);
    if (last >= 0) dst[n++] = (uint8_t)last;
    *dlen = (uint8_t)n;
}

static bool v_int(val_t *o, int32_t x)
{
    if (x == SNMP_ABSENT) return false;
    o->tag = T_INT;
    o->i = x;
    return true;
}

static bool v_str(val_t *o, const char *s)
{
    o->tag = T_OCTETS;
    o->s = s ? s : "";
    return true;
}

/* The value of one MIB entry now, or false if the UPS does not report it. */
static bool mib_get(const mib_t *e, const snmp_view_t *v, val_t *o)
{
    memset(o, 0, sizeof(*o));
    switch (e->kind) {
    case K_SYS_DESCR:    return v_str(o, v->sys_descr);
    case K_SYS_OBJID:    o->tag = T_OID; set_oid(o->oid, &o->olen, OID_UPS_MIB, sizeof(OID_UPS_MIB), -1); return true;
    case K_SYS_UPTIME:   o->tag = T_TICKS; o->u = v->uptime; return true;
    case K_SYS_CONTACT:  return v_str(o, v->sys_contact);
    case K_SYS_NAME:     return v_str(o, v->sys_name);
    case K_SYS_LOCATION: return v_str(o, v->sys_location);
    case K_SYS_SERVICES: return v_int(o, 72);     /* application + end-to-end */
    case K_ID_MFR:       return v_str(o, v->mfr);
    case K_ID_MODEL:     return v_str(o, v->model);
    case K_ID_UPSFW:     return v_str(o, v->ups_fw);
    case K_ID_AGENTFW:   return v_str(o, v->agent_ver);
    case K_ID_NAME:      return v_str(o, v->sys_name);
    case K_ID_ATTACHED:  return v_str(o, "");
    case K_B_STATUS:     return v_int(o, v->batt_status);
    case K_B_SECS:       return v_int(o, v->secs_on_batt);
    case K_B_MINUTES:    return v_int(o, v->minutes_left);
    case K_B_CHARGE:     return v_int(o, v->charge);
    case K_B_VOLTS:      return v_int(o, v->batt_dv);
    case K_B_TEMP:       return v_int(o, v->batt_temp);
    case K_IN_BADS:      o->tag = T_COUNTER; o->u = v->line_bads; return true;
    case K_IN_LINES:     return v_int(o, 1);
    case K_IN_FREQ:      return v_int(o, v->in_dhz);
    case K_IN_VOLTS:     return v_int(o, v->in_v);
    case K_IN_AMPS:      return v_int(o, v->in_da);
    case K_OUT_SOURCE:   return v_int(o, v->out_source);
    case K_OUT_FREQ:     return v_int(o, v->out_dhz);
    case K_OUT_LINES:    return v_int(o, 1);
    case K_OUT_VOLTS:    return v_int(o, v->out_v);
    case K_OUT_AMPS:     return v_int(o, v->out_da);
    case K_OUT_WATTS:    return v_int(o, v->out_w);
    case K_OUT_LOAD:     return v_int(o, v->out_load);
    case K_AL_PRESENT: {
        uint32_t n = 0;
        for (uint32_t a = v->alarms; a; a &= a - 1) n++;
        o->tag = T_GAUGE;
        o->u = n;
        return true;
    }
    case K_AL_DESCR:
        if (!(v->alarms & SNMP_ALARM_BIT(e->arg))) return false;
        o->tag = T_OID;
        set_oid(o->oid, &o->olen, OID_WELL_KNOWN, sizeof(OID_WELL_KNOWN), e->arg);
        return true;
    case K_AL_TIME:
        if (!(v->alarms & SNMP_ALARM_BIT(e->arg))) return false;
        o->tag = T_TICKS;
        o->u = v->alarm_time[e->arg];
        return true;
    case K_TEST_ID:
        /* Which test ran is not something a UPS says over HID; only "none"
         * is known for sure. */
        if (v->test_summary != 6) return false;
        o->tag = T_OID;
        set_oid(o->oid, &o->olen, OID_NO_TESTS, sizeof(OID_NO_TESTS), -1);
        return true;
    case K_TEST_SPINLOCK: return v->test_summary != SNMP_ABSENT && v_int(o, 0);  /* never SET */
    case K_TEST_SUMMARY: return v_int(o, v->test_summary);
    case K_TEST_DETAIL:  return v->test_detail && v->test_summary != SNMP_ABSENT && v_str(o, v->test_detail);
    case K_TEST_START:
        if (v->test_start == SNMP_ABSENT) return false;
        o->tag = T_TICKS;
        o->u = (uint32_t)v->test_start;
        return true;
    case K_TEST_ELAPSED: return v_int(o, v->test_elapsed);
    case K_CFG_IN_V:     return v_int(o, v->cfg_in_v);
    case K_CFG_IN_FREQ:  return v_int(o, v->cfg_in_dhz);
    case K_CFG_OUT_V:    return v_int(o, v->cfg_out_v);
    case K_CFG_OUT_FREQ: return v_int(o, v->cfg_out_dhz);
    case K_CFG_OUT_VA:   return v_int(o, v->cfg_out_va);
    case K_CFG_OUT_W:    return v_int(o, v->cfg_out_w);
    case K_CFG_LOW_BATT: return v_int(o, v->cfg_low_batt_min);
    case K_CFG_AUDIBLE:  return v_int(o, v->audible);
    case K_CFG_XFER_LO:  return v_int(o, v->cfg_xfer_lo);
    case K_CFG_XFER_HI:  return v_int(o, v->cfg_xfer_hi);
    }
    return false;
}

static int oid_cmp(const uint32_t *a, size_t na, const uint8_t *b, size_t nb)
{
    size_t n = na < nb ? na : nb;
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return na < nb ? -1 : na > nb ? 1 : 0;
}

/* Index of the entry named exactly `sub`, or -1. */
static int mib_exact(const uint32_t *sub, size_t n)
{
    for (size_t i = 0; i < MIB_N; i++) {
        if (oid_cmp(sub, n, MIB[i].sub, MIB[i].len) == 0) return (int)i;
    }
    return -1;
}

/* Whether `sub` names an object we have, with an instance we do not: every
 * entry is an object plus one index arc (.0, or a table row). RFC 3416
 * 4.2.1 answers that noSuchInstance, not noSuchObject. */
static bool mib_object_known(const uint32_t *sub, size_t n)
{
    for (size_t i = 0; i < MIB_N; i++) {
        size_t ol = (size_t)MIB[i].len - 1;
        if (n >= ol && oid_cmp(sub, ol, MIB[i].sub, ol) == 0) return true;
    }
    return false;
}

/* The first entry after index `from` (exclusive) that has a value, or -1. */
static int mib_next_from(int from, const snmp_view_t *v)
{
    val_t o;
    for (size_t i = (size_t)(from + 1); i < MIB_N; i++) {
        if (mib_get(&MIB[i], v, &o)) return (int)i;
    }
    return -1;
}

/* The first entry after OID `sub` that has a value, or -1. */
static int mib_next(const uint32_t *sub, size_t n, const snmp_view_t *v)
{
    for (size_t i = 0; i < MIB_N; i++) {
        if (oid_cmp(sub, n, MIB[i].sub, MIB[i].len) < 0) return mib_next_from((int)i - 1, v);
    }
    return -1;
}

bool snmp_mib_sorted(void)
{
    for (size_t i = 1; i < MIB_N; i++) {
        uint32_t a[OID8_MAX];
        for (size_t k = 0; k < MIB[i - 1].len; k++) a[k] = MIB[i - 1].sub[k];
        if (oid_cmp(a, MIB[i - 1].len, MIB[i].sub, MIB[i].len) >= 0) return false;
    }
    return true;
}

/* ---- the view ----------------------------------------------------------- */

static bool has_word(const char *s, const char *w)
{
    size_t wl = strlen(w);
    for (const char *p = s; p && *p; ) {
        while (*p == ' ') p++;
        const char *q = p;
        while (*q && *q != ' ') q++;
        if ((size_t)(q - p) == wl && strncmp(p, w, wl) == 0) return true;
        p = q;
    }
    return false;
}

/* Rounded to a whole number, never negative, SNMP_ABSENT if absent. */
static int32_t nn(float f, float scale)
{
    if (!ups_valid(f)) return SNMP_ABSENT;
    float x = f * scale;
    if (!(x >= 0.0f)) return 0;                 /* negative, or NaN */
    if (x > 2.0e9f) return 2000000000;
    return (int32_t)(x + 0.5f);
}

static int32_t clampi(int32_t x, int32_t lo, int32_t hi)
{
    if (x == SNMP_ABSENT) return x;
    return x < lo ? lo : x > hi ? hi : x;
}

static const char *test_text(int32_t s)
{
    switch (s) {
    case 1: return "Passed";
    case 2: return "Passed with a warning";
    case 3: return "Failed";
    case 4: return "Aborted";
    case 5: return "In progress";
    default: return "No test run";
    }
}

void snmp_view_fill(snmp_track_t *t, snmp_view_t *v, const ups_data_t *d,
                    const snmp_extra_t *x, int64_t now_us)
{
    strncpy(v->mfr, d->mfr, sizeof(v->mfr) - 1);
    v->mfr[sizeof(v->mfr) - 1] = '\0';
    strncpy(v->model, d->model, sizeof(v->model) - 1);
    v->model[sizeof(v->model) - 1] = '\0';
    strncpy(v->ups_fw, x->ups_fw ? x->ups_fw : "", sizeof(v->ups_fw) - 1);
    v->ups_fw[sizeof(v->ups_fw) - 1] = '\0';

    const char *st = x->nut_status ? x->nut_status : "";
    bool ob = !x->stale && has_word(st, "OB");
    uint32_t al = 0;
    if (x->stale) {
        /* What the UPS last said still stands; only now it cannot be read.
         * Clearing it would tell a receiver "back on mains" that nobody saw. */
        al = t->alarms & ~SNMP_ALARM_BIT(SNMP_ALARM_COMMS_LOST);
        if (!x->comms_grace) al |= SNMP_ALARM_BIT(SNMP_ALARM_COMMS_LOST);
        ob = (al & SNMP_ALARM_BIT(SNMP_ALARM_ON_BATTERY)) != 0;
    } else {
        if (ob)                        al |= SNMP_ALARM_BIT(SNMP_ALARM_ON_BATTERY);
        if (has_word(st, "LB"))        al |= SNMP_ALARM_BIT(SNMP_ALARM_LOW_BATTERY);
        if (has_word(st, "RB"))        al |= SNMP_ALARM_BIT(SNMP_ALARM_BATTERY_BAD);
        if (has_word(st, "OVER"))      al |= SNMP_ALARM_BIT(SNMP_ALARM_OUTPUT_OVERLOAD);
        if (has_word(st, "BYPASS"))    al |= SNMP_ALARM_BIT(SNMP_ALARM_ON_BYPASS);
        if (has_word(st, "OFF"))       al |= SNMP_ALARM_BIT(SNMP_ALARM_UPS_OUTPUT_OFF);
        if (has_word(st, "CAL") || d->test_result == UPS_TEST_IN_PROGRESS)
                                       al |= SNMP_ALARM_BIT(SNMP_ALARM_TEST_IN_PROGRESS);
        if (d->test_result == UPS_TEST_ERROR)
                                       al |= SNMP_ALARM_BIT(SNMP_ALARM_TEST_FAILED);
        /* Faults NUT reports only in ups.alarm. */
        if (x->nst_valid) {
            static const struct { uint8_t nst, alarm; } F[] = {
                { NST_BATTVOLTLO,    SNMP_ALARM_BATTERY_BAD },
                { NST_BATTVOLTHI,    SNMP_ALARM_BATTERY_BAD },
                { NST_DEPLETED,      SNMP_ALARM_DEPLETED_BATTERY },
                { NST_OVERHEAT,      SNMP_ALARM_TEMP_BAD },
                { NST_CHARGERFAIL,   SNMP_ALARM_CHARGER_FAILED },
                { NST_FANFAIL,       SNMP_ALARM_FAN_FAILURE },
                { NST_COMMFAULT,     SNMP_ALARM_GENERAL_FAULT },
                { NST_AWAITINGPOWER, SNMP_ALARM_AWAITING_POWER },
                { NST_SHUTDOWNIMM,   SNMP_ALARM_SHUTDOWN_IMMINENT },
            };
            for (size_t i = 0; i < sizeof(F) / sizeof(F[0]); i++) {
                if (x->nst_bits & NST(F[i].nst)) al |= SNMP_ALARM_BIT(F[i].alarm);
            }
        }
    }

    for (int id = 1; id <= SNMP_ALARM_MAX; id++) {
        uint32_t b = SNMP_ALARM_BIT(id);
        if ((al & b) && !(t->alarms & b)) t->alarm_time[id] = t->started ? v->uptime : 0;
    }
    uint32_t obb = SNMP_ALARM_BIT(SNMP_ALARM_ON_BATTERY);
    if ((al & obb) && !(t->alarms & obb)) {
        t->ob_since_us = now_us;
        if (t->started) t->line_bads++;
    }
    t->alarms = al;
    /* Not on a fill inside the start-up grace: the UPS has not been read
     * yet, and what it says when it is was "present at start" (alarm time 0,
     * no line bad), not a change. */
    if (!x->stale || !x->comms_grace) t->started = true;
    v->alarms = al;
    memcpy(v->alarm_time, t->alarm_time, sizeof(v->alarm_time));
    v->line_bads = t->line_bads;

    v->cfg_in_v         = nn(x->in_nominal, 1);
    v->cfg_out_v        = nn(x->out_nominal, 1);
    v->cfg_out_w        = nn(d->ups_realpower_nom, 1);
    v->cfg_in_dhz       = nn(x->in_freq_nominal, 10);
    v->cfg_out_dhz      = nn(x->out_freq_nominal, 10);
    v->cfg_out_va       = nn(x->va_nominal, 1);
    v->cfg_low_batt_min = nn(x->low_batt_s, 1.0f / 60.0f);
    v->cfg_xfer_lo      = nn(x->xfer_low, 1);
    v->cfg_xfer_hi      = nn(x->xfer_high, 1);

    if (x->stale) {
        v->batt_status = 1;                       /* unknown */
        /* Seconds on battery follow the held on-battery alarm, so the
         * repeated upsTrapOnBattery still says how long it has been. */
        v->secs_on_batt = ob ? (int32_t)((now_us - t->ob_since_us) / 1000000) : 0;
        v->minutes_left = v->charge = v->batt_dv = v->batt_temp = SNMP_ABSENT;
        v->in_dhz = v->in_v = v->in_da = v->out_source = v->out_dhz = v->out_v = v->out_da = SNMP_ABSENT;
        v->out_w = v->out_load = v->test_summary = v->audible = SNMP_ABSENT;
        v->test_start = v->test_elapsed = SNMP_ABSENT;
        v->test_detail = NULL;
        return;
    }
    bool depleted = x->nst_valid && (x->nst_bits & NST(NST_DEPLETED));
    v->batt_status  = depleted ? 4 : has_word(st, "LB") ? 3 : 2;
    v->secs_on_batt = ob ? (int32_t)((now_us - t->ob_since_us) / 1000000) : 0;
    v->minutes_left = nn(d->battery_runtime, 1.0f / 60.0f);
    if (v->minutes_left != SNMP_ABSENT && v->minutes_left < 1) v->minutes_left = 1;  /* PositiveInteger */
    v->charge       = clampi(nn(d->battery_charge, 1), 0, 100);
    v->batt_dv      = nn(d->battery_voltage, 10);
    v->batt_temp    = ups_valid(x->batt_temp) && x->batt_temp > -1000.0f && x->batt_temp < 1000.0f
                      ? (int32_t)(x->batt_temp + (x->batt_temp < 0 ? -0.5f : 0.5f)) : SNMP_ABSENT;
    v->in_dhz       = nn(d->input_frequency, 10);
    v->in_v         = nn(d->input_voltage, 1);
    v->in_da        = nn(x->in_current, 10);
    v->out_dhz      = nn(x->out_freq, 10);
    v->out_v        = nn(d->output_voltage, 1);
    v->out_da       = nn(x->out_current, 10);
    v->out_w        = nn(d->ups_realpower, 1);  /* measured only, never load x rating */
    v->out_load     = clampi(nn(d->ups_load, 1), 0, 200);

    if (has_word(st, "OFF"))         v->out_source = 2;
    else if (has_word(st, "BYPASS")) v->out_source = 4;
    else if (ob)                     v->out_source = 5;
    else if (has_word(st, "BOOST"))  v->out_source = 6;
    else if (has_word(st, "TRIM"))   v->out_source = 7;
    else if (has_word(st, "OL"))     v->out_source = 3;
    else                             v->out_source = 1;

    /* ups_test_result_t is upsTestResultsSummary's own numbering, 1-6
     * (both come from the HID Power Device Test usage); 0 has no test. */
    v->test_summary = (d->test_result >= 1 && d->test_result <= 6) ? d->test_result : 6;
    v->test_detail  = test_text(v->test_summary);

    /* When a test began and how long it took: known only for one this
     * board saw start. One already running, or already done, at start-up
     * has no start time to give. */
    if (t->test_summary == 0) t->test_start = t->test_end = SNMP_ABSENT;
    if (v->test_summary == 5 && t->test_summary != 5) {
        t->test_start = t->test_summary ? (int32_t)v->uptime : SNMP_ABSENT;
        t->test_end = SNMP_ABSENT;
    } else if (t->test_summary == 5 && v->test_summary != 5) {
        t->test_end = (int32_t)v->uptime;
    }
    t->test_summary = v->test_summary;
    if (v->test_summary == 6) {
        v->test_start = v->test_elapsed = 0;      /* RFC 1628: 0 when no test has run */
    } else {
        v->test_start = t->test_start;
        v->test_elapsed = t->test_start == SNMP_ABSENT ? SNMP_ABSENT
                        : (int32_t)((t->test_end == SNMP_ABSENT ? v->uptime : (uint32_t)t->test_end)
                                    - (uint32_t)t->test_start);
    }
    v->audible      = ((d->caps & UPS_CAP_BEEPER) && d->beeper >= 1 && d->beeper <= 3)
                      ? d->beeper : SNMP_ABSENT;
}

/* ---- the agent ---------------------------------------------------------- */

typedef struct {
    int16_t  entry;             /* MIB index, or -1 for an exception */
    uint8_t  exc;               /* X_NOOBJ / X_NOINST / X_END */
    uint8_t  req;               /* which request varbind named it */
} res_t;

typedef struct {
    const uint8_t *oid;         /* contents of the request's OID */
    size_t         len;
} reqvb_t;

static void res_to_vb(const res_t *r, const reqvb_t *rq, const snmp_view_t *v, vb_t *b)
{
    memset(b, 0, sizeof(*b));
    if (r->entry >= 0) {
        const mib_t *e = &MIB[r->entry];
        memcpy(b->oid, e->sub, e->len);
        b->olen = e->len;
        if (!mib_get(e, v, &b->v)) b->v.tag = X_NOINST;   /* cannot happen: chosen for a value */
    } else {
        b->raw = rq[r->req].oid;
        b->rawlen = rq[r->req].len;
        b->v.tag = r->exc;
    }
}

typedef struct {
    int32_t        version;
    const uint8_t *comm;
    size_t         comm_len;
    rd_t           reqid;       /* raw INTEGER contents, echoed */
} hdr_t;

/* The whole response message. Either `res` (n of them) or, when echo is
 * non-NULL, the request's own varbind list verbatim (for errors). */
static void w_response(wr_t *w, const hdr_t *h, int32_t err, int32_t idx,
                       const res_t *res, size_t n, const reqvb_t *rq,
                       const rd_t *echo, const snmp_view_t *v)
{
    size_t vbl = 0;
    vb_t b;
    if (echo) {
        vbl = (size_t)(echo->end - echo->p);
    } else {
        for (size_t i = 0; i < n; i++) {
            res_to_vb(&res[i], rq, v, &b);
            vbl += tlv_size(vb_clen(&b));
        }
    }
    size_t rid = (size_t)(h->reqid.end - h->reqid.p);
    size_t pdu = tlv_size(rid) + tlv_size(int_clen(err)) + tlv_size(int_clen(idx)) + tlv_size(vbl);
    size_t msg = tlv_size(int_clen(h->version)) + tlv_size(h->comm_len) + tlv_size(pdu);

    w_tl(w, T_SEQ, msg);
    w_int(w, T_INT, h->version);
    w_tl(w, T_OCTETS, h->comm_len);
    w_bytes(w, h->comm, h->comm_len);
    w_tl(w, PDU_RESP, pdu);
    w_tl(w, T_INT, rid);
    w_bytes(w, h->reqid.p, rid);
    w_int(w, T_INT, err);
    w_int(w, T_INT, idx);
    w_tl(w, T_SEQ, vbl);
    if (echo) {
        w_bytes(w, echo->p, vbl);
    } else {
        for (size_t i = 0; i < n; i++) {
            res_to_vb(&res[i], rq, v, &b);
            w_vb(w, &b);
        }
    }
}

static size_t response_size(const hdr_t *h, int32_t err, int32_t idx, const res_t *res,
                            size_t n, const reqvb_t *rq, const rd_t *echo, const snmp_view_t *v)
{
    wr_t c = { NULL, 0, 0 };
    w_response(&c, h, err, idx, res, n, rq, echo, v);
    return c.n;
}

static size_t emit(uint8_t *out, size_t cap, const hdr_t *h, int32_t err, int32_t idx,
                   const res_t *res, size_t n, const reqvb_t *rq, const rd_t *echo,
                   const snmp_view_t *v)
{
    size_t need = response_size(h, err, idx, res, n, rq, echo, v);
    if (need > cap) return 0;
    wr_t w = { out, cap, 0 };
    w_response(&w, h, err, idx, res, n, rq, echo, v);
    return w.n;
}

/* tooBig, with no varbinds, as RFC 3416 4.2 asks; 0 if even that won't fit. */
static size_t too_big(uint8_t *out, size_t cap, const hdr_t *h, const snmp_view_t *v)
{
    return emit(out, cap, h, E_TOOBIG, 0, NULL, 0, NULL, NULL, v);
}

static bool comm_ok(const uint8_t *p, size_t n, const char *want)
{
    size_t wl = strlen(want);
    if (wl == 0) return false;
    uint8_t diff = (uint8_t)(n != wl);
    for (size_t i = 0; i < n; i++) diff |= p[i] ^ (uint8_t)(i < wl ? want[i] : 0);
    return diff == 0;
}

size_t snmp_handle(const uint8_t *req, size_t len, uint8_t *out, size_t cap,
                   const char *community, const snmp_view_t *v, snmp_stats_t *st)
{
    snmp_stats_t dummy;
    if (!st) st = &dummy;
    st->in_pkts++;

    rd_t r = { req, req + len }, msg, comm, pdu, vbl;
    hdr_t h;
    uint8_t ptag;
    size_t plen;
    if (!rd_want(&r, T_SEQ, &msg) || r.p != r.end) goto bad;
    if (!rd_int(&msg, &h.version, NULL)) goto bad;
    if (h.version != 0 && h.version != 1) { st->bad_version++; return 0; }
    if (!rd_want(&msg, T_OCTETS, &comm)) goto bad;
    h.comm = comm.p;
    h.comm_len = (size_t)(comm.end - comm.p);
    if (h.comm_len > SNMP_COMMUNITY_MAX || !comm_ok(comm.p, h.comm_len, community)) {
        st->bad_community++;
        return 0;
    }
    if (!rd_tl(&msg, &ptag, &plen) || plen != (size_t)(msg.end - msg.p)) goto bad;
    pdu.p = msg.p;
    pdu.end = msg.end;
    if (ptag != PDU_GET && ptag != PDU_NEXT && ptag != PDU_SET &&
        !(ptag == PDU_BULK && h.version == 1)) goto bad;

    int32_t a, b;
    if (!rd_int(&pdu, &a, &h.reqid) || !rd_int(&pdu, &a, NULL) || !rd_int(&pdu, &b, NULL)) goto bad;
    if (!rd_want(&pdu, T_SEQ, &vbl) || pdu.p != pdu.end) goto bad;

    reqvb_t rq[SNMP_REQ_VB_MAX];
    size_t nvb = 0;
    bool over = false;
    for (rd_t it = vbl; it.p < it.end; ) {
        rd_t one, name;
        uint8_t vt;
        size_t vl;
        if (!rd_want(&it, T_SEQ, &one) || !rd_want(&one, T_OID, &name)) goto bad;
        if (!rd_tl(&one, &vt, &vl)) goto bad;
        one.p += vl;
        if (one.p != one.end) goto bad;
        if (nvb == SNMP_REQ_VB_MAX) { over = true; continue; }
        rq[nvb].oid = name.p;
        rq[nvb].len = (size_t)(name.end - name.p);
        nvb++;
    }
    st->answered++;

    if (ptag == PDU_SET) {
        return emit(out, cap, &h, h.version == 0 ? E_NOSUCHNAME : E_NOTWRITABLE, nvb ? 1 : 0,
                    NULL, 0, NULL, &vbl, v);
    }
    if (over) return too_big(out, cap, &h, v);

    res_t res[SNMP_RESP_VB_MAX];
    size_t n = 0;
    uint32_t sub[SNMP_OID_MAX];
    size_t ns;

    if (ptag == PDU_GET || ptag == PDU_NEXT) {
        for (size_t i = 0; i < nvb; i++) {
            res_t *x = &res[n++];
            x->req = (uint8_t)i;
            x->entry = -1;
            if (!oid_decode(rq[i].oid, rq[i].len, sub, &ns)) {
                x->exc = ptag == PDU_GET ? X_NOOBJ : X_END;
            } else if (ptag == PDU_GET) {
                int e = mib_exact(sub, ns);
                val_t o;
                if (e >= 0 && mib_get(&MIB[e], v, &o)) x->entry = (int16_t)e;
                else x->exc = e >= 0 || mib_object_known(sub, ns) ? X_NOINST : X_NOOBJ;
            } else {
                int e = mib_next(sub, ns, v);
                if (e >= 0) x->entry = (int16_t)e;
                else x->exc = X_END;
            }
            if (h.version == 0 && x->entry < 0) {
                /* v1 has no exceptions: the whole request fails, naming it. */
                return emit(out, cap, &h, E_NOSUCHNAME, (int32_t)i + 1, NULL, 0, NULL, &vbl, v);
            }
        }
        size_t got = emit(out, cap, &h, 0, 0, res, n, rq, NULL, v);
        return got ? got : too_big(out, cap, &h, v);
    }

    /* GETBULK: `a` non-repeaters, `b` max-repetitions. */
    size_t nonrep = a < 0 ? 0 : (size_t)a > nvb ? nvb : (size_t)a;
    size_t reps = b < 0 ? 0 : b > SNMP_BULK_REPS_MAX ? SNMP_BULK_REPS_MAX : (size_t)b;
    size_t nrep = nvb - nonrep;
    int cur[SNMP_REQ_VB_MAX];
    for (size_t i = 0; i < nvb; i++) {   /* nonrep <= SNMP_REQ_VB_MAX < SNMP_RESP_VB_MAX */
        int e = oid_decode(rq[i].oid, rq[i].len, sub, &ns) ? mib_next(sub, ns, v) : -1;
        if (i < nonrep) {
            res[n++] = (res_t){ (int16_t)e, X_END, (uint8_t)i };
        } else {
            cur[i] = e;
        }
    }
    size_t fixed = n;
    for (size_t r2 = 0; r2 < reps && nrep; r2++) {
        bool any = false;
        for (size_t j = nonrep; j < nvb; j++) {
            if (n == SNMP_RESP_VB_MAX) goto full;
            int e = cur[j];
            res[n++] = (res_t){ (int16_t)e, X_END, (uint8_t)j };
            if (e >= 0) {
                any = true;
                cur[j] = mib_next_from(e, v);
            }
        }
        if (!any) break;        /* every column is past the end */
    }
full:
    /* Fewer repetitions where the whole lot will not fit (RFC 3416 4.2.3). */
    while (n > fixed && response_size(&h, 0, 0, res, n, rq, NULL, v) > cap) n--;
    {
        size_t got = emit(out, cap, &h, 0, 0, res, n, rq, NULL, v);
        return got ? got : too_big(out, cap, &h, v);
    }

bad:
    st->parse_errors++;
    return 0;
}

/* ---- traps -------------------------------------------------------------- */

#define NEVER INT64_MIN

void snmp_ev_reset(snmp_ev_t *ev, bool cold)
{
    memset(ev, 0, sizeof(*ev));
    ev->cold_pending = cold;
    for (int i = 0; i <= SNMP_ALARM_MAX; i++) ev->last_us[i] = NEVER;
    ev->onbatt_us = NEVER;
}

static bool spaced(int64_t last, int64_t now, int64_t gap)
{
    return last == NEVER || now - last >= gap;
}

int snmp_events(snmp_ev_t *ev, const snmp_view_t *v, int64_t now_us,
                snmp_event_t *out, int max)
{
    int n = 0;
    if (ev->cold_pending && n < max) {
        out[n++] = (snmp_event_t){ SNMP_TRAP_COLD_START, 0 };
        ev->cold_pending = false;
    }
    for (int id = 1; id <= SNMP_ALARM_MAX; id++) {
        uint32_t bit = SNMP_ALARM_BIT(id);
        bool want = (v->alarms & bit) != 0, had = (ev->told & bit) != 0;
        if (want == had) continue;
        /* RFC 1628: upsAlarmTestInProgress is in the table but never trapped. */
        if (id == SNMP_ALARM_TEST_IN_PROGRESS) { ev->told ^= bit; continue; }
        if (!spaced(ev->last_us[id], now_us, SNMP_TRAP_SPACING_US)) continue;
        if (n >= max) break;
        if (want && id == SNMP_ALARM_ON_BATTERY) {
            /* ...and on battery has its own trap instead of "alarm added". */
            out[n++] = (snmp_event_t){ SNMP_TRAP_ON_BATTERY, 0 };
            ev->onbatt_us = now_us;
        } else {
            out[n++] = (snmp_event_t){ want ? SNMP_TRAP_ALARM_ADDED : SNMP_TRAP_ALARM_REMOVED, id };
        }
        ev->told ^= bit;
        ev->last_us[id] = now_us;
    }
    uint32_t ob = SNMP_ALARM_BIT(SNMP_ALARM_ON_BATTERY);
    if ((ev->told & ob) && (v->alarms & ob) && n < max &&
        spaced(ev->onbatt_us, now_us, SNMP_ONBATT_REPEAT_US)) {
        out[n++] = (snmp_event_t){ SNMP_TRAP_ON_BATTERY, 0 };
        ev->onbatt_us = now_us;
        ev->last_us[SNMP_ALARM_ON_BATTERY] = now_us;
    }
    if (v->test_summary != SNMP_ABSENT) {
        bool done = ev->test_summary == 5 && v->test_summary >= 1 && v->test_summary <= 4;
        if (done && n >= max) return n;           /* owed: ask again next time */
        if (done) out[n++] = (snmp_event_t){ SNMP_TRAP_TEST_COMPLETED, 0 };
        ev->test_summary = v->test_summary;
    }
    return n;
}

/* A varbind from one of our MIB entries, if it has a value now. */
static bool vb_from_mib(vb_t *b, const uint8_t *sub, size_t len, const snmp_view_t *v)
{
    uint32_t s[OID8_MAX];
    for (size_t i = 0; i < len; i++) s[i] = sub[i];
    int e = mib_exact(s, len);
    memset(b, 0, sizeof(*b));
    if (e < 0 || !mib_get(&MIB[e], v, &b->v)) return false;
    memcpy(b->oid, sub, len);
    b->olen = (uint8_t)len;
    return true;
}

size_t snmp_build_trap(uint8_t *out, size_t cap, const char *community,
                       uint32_t request_id, const snmp_view_t *v, const snmp_event_t *e)
{
    vb_t vbs[8];
    size_t n = 0;

    memset(vbs, 0, sizeof(vbs));
    set_oid(vbs[0].oid, &vbs[0].olen, OID_SYSUPTIME0, sizeof(OID_SYSUPTIME0), -1);
    vbs[0].v.tag = T_TICKS;
    vbs[0].v.u = v->uptime;
    set_oid(vbs[1].oid, &vbs[1].olen, OID_TRAPOID0, sizeof(OID_TRAPOID0), -1);
    vbs[1].v.tag = T_OID;
    n = 2;

    /* The OBJECTS RFC 1628 lists for each trap, in its order. One the UPS
     * does not report is left out rather than made up. */
    static const uint8_t MINUTES0[] = { 1,3,6,1,2,1,33,1,2,3,0 };
    static const uint8_t SECS0[]    = { 1,3,6,1,2,1,33,1,2,2,0 };
    static const uint8_t LOWBATT0[] = { 1,3,6,1,2,1,33,1,9,7,0 };
    static const uint8_t TESTID0[]  = { 1,3,6,1,2,1,33,1,7,1,0 };
    static const uint8_t SPIN0[]    = { 1,3,6,1,2,1,33,1,7,2,0 };
    static const uint8_t SUMMARY0[] = { 1,3,6,1,2,1,33,1,7,3,0 };
    static const uint8_t DETAIL0[]  = { 1,3,6,1,2,1,33,1,7,4,0 };
    static const uint8_t START0[]   = { 1,3,6,1,2,1,33,1,7,5,0 };
    static const uint8_t ELAPSED0[] = { 1,3,6,1,2,1,33,1,7,6,0 };
    static const uint8_t SYSNAME0[] = { 1,3,6,1,2,1,1,5,0 };
    static const uint8_t DESCR_COL[] = { 1,3,6,1,2,1,33,1,6,2,1,2 };

    val_t *trap = &vbs[1].v;
    switch (e->kind) {
    case SNMP_TRAP_COLD_START:
    case SNMP_TRAP_WARM_START:
        set_oid(trap->oid, &trap->olen, OID_SNMP_TRAPS, sizeof(OID_SNMP_TRAPS),
                e->kind == SNMP_TRAP_COLD_START ? 1 : 2);
        if (vb_from_mib(&vbs[n], SYSNAME0, sizeof(SYSNAME0), v)) n++;
        break;
    case SNMP_TRAP_ON_BATTERY:
        set_oid(trap->oid, &trap->olen, OID_UPS_TRAPS, sizeof(OID_UPS_TRAPS), 1);
        if (vb_from_mib(&vbs[n], MINUTES0, sizeof(MINUTES0), v)) n++;
        if (vb_from_mib(&vbs[n], SECS0, sizeof(SECS0), v)) n++;
        if (vb_from_mib(&vbs[n], LOWBATT0, sizeof(LOWBATT0), v)) n++;
        break;
    case SNMP_TRAP_TEST_COMPLETED:
        set_oid(trap->oid, &trap->olen, OID_UPS_TRAPS, sizeof(OID_UPS_TRAPS), 2);
        if (vb_from_mib(&vbs[n], TESTID0, sizeof(TESTID0), v)) n++;
        if (vb_from_mib(&vbs[n], SPIN0, sizeof(SPIN0), v)) n++;
        if (vb_from_mib(&vbs[n], SUMMARY0, sizeof(SUMMARY0), v)) n++;
        if (vb_from_mib(&vbs[n], DETAIL0, sizeof(DETAIL0), v)) n++;
        if (vb_from_mib(&vbs[n], START0, sizeof(START0), v)) n++;
        if (vb_from_mib(&vbs[n], ELAPSED0, sizeof(ELAPSED0), v)) n++;
        break;
    case SNMP_TRAP_ALARM_ADDED:
    case SNMP_TRAP_ALARM_REMOVED:
        if (e->alarm < 1 || e->alarm > SNMP_ALARM_MAX) return 0;
        set_oid(trap->oid, &trap->olen, OID_UPS_TRAPS, sizeof(OID_UPS_TRAPS),
                e->kind == SNMP_TRAP_ALARM_ADDED ? 3 : 4);
        /* Built here, not read from the table: on removal the row is gone. */
        set_oid(vbs[n].oid, &vbs[n].olen, OID_ALARM_ID, sizeof(OID_ALARM_ID), e->alarm);
        vbs[n].v.tag = T_INT;
        vbs[n].v.i = e->alarm;
        n++;
        set_oid(vbs[n].oid, &vbs[n].olen, DESCR_COL, sizeof(DESCR_COL), e->alarm);
        vbs[n].v.tag = T_OID;
        set_oid(vbs[n].v.oid, &vbs[n].v.olen, OID_WELL_KNOWN, sizeof(OID_WELL_KNOWN), e->alarm);
        n++;
        break;
    default:
        return 0;
    }

    size_t comm = strlen(community);
    if (comm == 0 || comm > SNMP_COMMUNITY_MAX) return 0;
    int32_t rid = (int32_t)(request_id & 0x7FFFFFFF);
    size_t vbl = 0;
    for (size_t i = 0; i < n; i++) vbl += tlv_size(vb_clen(&vbs[i]));
    size_t pdu = tlv_size(int_clen(rid)) + 2 * tlv_size(1) + tlv_size(vbl);
    size_t msg = tlv_size(1) + tlv_size(comm) + tlv_size(pdu);
    if (tlv_size(msg) > cap) return 0;

    wr_t w = { out, cap, 0 };
    w_tl(&w, T_SEQ, msg);
    w_int(&w, T_INT, 1);                 /* v2c */
    w_tl(&w, T_OCTETS, comm);
    w_bytes(&w, community, comm);
    w_tl(&w, PDU_TRAP2, pdu);
    w_int(&w, T_INT, rid);
    w_int(&w, T_INT, 0);
    w_int(&w, T_INT, 0);
    w_tl(&w, T_SEQ, vbl);
    for (size_t i = 0; i < n; i++) w_vb(&w, &vbs[i]);
    return w.n;
}

/* ---- settings ----------------------------------------------------------- */

bool snmp_ipv4_ok(const char *s, uint32_t *out)
{
    uint32_t a[4];
    int k = 0;
    const char *p = s;
    if (!p) return false;
    for (k = 0; k < 4; k++) {
        if (*p < '0' || *p > '9') return false;
        uint32_t x = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            if (++digits > 3) return false;
            if (digits == 2 && x == 0) return false;          /* "01": not dotted decimal */
            x = x * 10 + (uint32_t)(*p++ - '0');
        }
        if (x > 255) return false;
        a[k] = x;
        if (k < 3 && *p++ != '.') return false;
    }
    if (*p) return false;
    uint32_t host = a[0] << 24 | a[1] << 16 | a[2] << 8 | a[3];
    /* Not "this network" (0/8), loopback, multicast or reserved (224/3 and up). */
    if (host == 0xFFFFFFFFu || a[0] == 0 || a[0] == 127 || a[0] >= 224) return false;
    if (out) {
        uint8_t b[4] = { (uint8_t)a[0], (uint8_t)a[1], (uint8_t)a[2], (uint8_t)a[3] };
        memcpy(out, b, 4);
    }
    return true;
}

static bool printable(const char *s, size_t max, bool space_ok)
{
    size_t n = strnlen(s, max + 1);
    if (n > max) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch < 0x20 || ch > 0x7E || (!space_ok && ch == ' ')) return false;
    }
    return true;
}

bool snmp_cfg_check(const snmp_cfg_t *c, char *why, size_t why_len)
{
    const char *bad = NULL;
    if (!printable(c->community, SNMP_COMMUNITY_MAX, false))
        bad = "the community must be up to 32 letters, digits or symbols, without spaces";
    else if ((c->agent || c->traps) && !c->community[0])
        bad = "set a community first";
    else if (c->traps && !snmp_ipv4_ok(c->trap_host, NULL))
        bad = "the trap receiver must be an IPv4 address, four numbers with dots";
    else if (c->trap_host[0] && !snmp_ipv4_ok(c->trap_host, NULL))
        bad = "the trap receiver must be an IPv4 address, four numbers with dots";
    else if (c->trap_port == 0)
        bad = "the trap port must be 1 to 65535";
    else if (!printable(c->location, sizeof(c->location) - 1, true) ||
             !printable(c->contact, sizeof(c->contact) - 1, true))
        bad = "location and contact are up to 63 plain characters";
    if (bad && why && why_len) snprintf(why, why_len, "%s", bad);
    return bad == NULL;
}
