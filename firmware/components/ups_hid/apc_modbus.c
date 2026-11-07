/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * APC Modbus over USB, the protocol half. See apc_modbus.h for what this is
 * and where it comes from. Plain C with no ESP-IDF, so the host tests run it.
 */
#include "apc_modbus.h"

#include <stdio.h>
#include <string.h>

#define USAGE_TO_UPS    0xFF8600FCu
#define USAGE_FROM_UPS  0xFF8600FDu

bool apcmb_find_reports(const uint8_t *desc, size_t len, uint8_t *to_ups, uint8_t *from_ups)
{
    uint32_t page = 0, page_stack[4];
    uint8_t  rid = 0, rid_stack[4];
    int      sp = 0;
    uint32_t usages[16];
    size_t   nu = 0;
    bool     have_to = false, have_from = false;

    for (size_t i = 0; i < len; ) {
        uint8_t b = desc[i];
        if (b == 0xFE) {                    /* long item: skip it */
            if (i + 2 >= len) break;
            i += 3 + desc[i + 1];
            continue;
        }
        size_t size = (b & 3) == 3 ? 4 : (b & 3);
        if (i + 1 + size > len) break;
        uint32_t v = 0;
        for (size_t k = 0; k < size; k++) v |= (uint32_t)desc[i + 1 + k] << (8 * k);
        uint8_t type = (b >> 2) & 3, tag = b >> 4;

        if (type == 1) {                    /* global */
            if (tag == 0) page = v;
            else if (tag == 8) rid = (uint8_t)v;
            else if (tag == 10 && sp < 4) { page_stack[sp] = page; rid_stack[sp] = rid; sp++; }
            else if (tag == 11 && sp > 0) { sp--; page = page_stack[sp]; rid = rid_stack[sp]; }
        } else if (type == 2) {             /* local */
            if (tag == 0 && nu < 16) usages[nu++] = size == 4 ? v : (page << 16) | v;
        } else if (type == 0) {             /* main */
            for (size_t k = 0; k < nu; k++) {
                if (tag == 9 && usages[k] == USAGE_TO_UPS)        { *to_ups = rid;   have_to = true; }
                else if (tag == 8 && usages[k] == USAGE_FROM_UPS) { *from_ups = rid; have_from = true; }
            }
            nu = 0;                         /* locals end at every main item */
        }
        i += 1 + size;
    }
    return have_to && have_from;
}

void apcmb_build_read(uint8_t report_id, uint8_t slave, uint16_t addr, uint16_t count,
                      uint8_t out[APCMB_REPORT])
{
    memset(out, 0, APCMB_REPORT);
    out[0] = report_id;
    out[1] = slave;
    out[2] = 0x03;                          /* Read Holding Registers */
    out[3] = (uint8_t)(addr >> 8);
    out[4] = (uint8_t)addr;
    out[5] = (uint8_t)(count >> 8);
    out[6] = (uint8_t)count;
}

void apcmb_rx_init(apcmb_rx_t *rx, uint8_t slave, uint16_t count)
{
    memset(rx, 0, sizeof(*rx));
    rx->slave = slave;
    rx->count = count > APCMB_MAX_REGS ? APCMB_MAX_REGS : count;
}

apcmb_rx_status_t apcmb_rx_feed(apcmb_rx_t *rx, const uint8_t *payload, size_t n,
                                uint16_t *regs)
{
    if (rx->len == 0) {
        /* The start of a frame: is it the reply to this request? A reply to
         * an earlier, abandoned one is still queued on a packetised link
         * (NUT's EMBBADSLAVE / EMBBADDATA retries exist for exactly this),
         * and a read of a different length is one of those. */
        if (n < 3 || payload[0] != rx->slave) return APCMB_FOREIGN;
        if (payload[1] == 0x83) { rx->exception = payload[2]; return APCMB_EXCEPTION; }
        if (payload[1] != 0x03 || payload[2] != 2 * rx->count) return APCMB_FOREIGN;
    }
    size_t want = 3 + 2 * (size_t)rx->count;
    size_t take = want - rx->len;
    if (take > n) take = n;
    memcpy(rx->frame + rx->len, payload, take);
    rx->len += take;
    if (rx->len < want) return APCMB_MORE;
    for (uint16_t k = 0; k < rx->count; k++) {
        regs[k] = (uint16_t)((rx->frame[3 + 2 * k] << 8) | rx->frame[4 + 2 * k]);
    }
    return APCMB_DONE;
}

void apcmb_reading_init(apcmb_reading_t *r)
{
    r->power_nominal = r->realpower_nominal = APCMB_ABSENT;
    r->battery_voltage = r->battery_temperature = APCMB_ABSENT;
    r->load = r->realpower = r->power = APCMB_ABSENT;
    r->output_current = r->output_voltage = r->output_frequency = APCMB_ABSENT;
    r->input_voltage = r->efficiency = APCMB_ABSENT;
    r->efficiency_word = NULL;
}

/* NUT's _apc_modbus_to_double: a register over 2^scale, signed or not. */
static double u(const uint16_t *regs, int addr, int base, int scale)
{
    return (double)regs[addr - base] / (double)(1 << scale);
}

static double s(const uint16_t *regs, int addr, int base, int scale)
{
    return (double)(int16_t)regs[addr - base] / (double)(1 << scale);
}

void apcmb_decode_nominal(const uint16_t regs[APCMB_NOM_COUNT], apcmb_reading_t *r)
{
    r->power_nominal     = u(regs, 588, APCMB_NOM_ADDR, 0);
    r->realpower_nominal = u(regs, 589, APCMB_NOM_ADDR, 0);
}

static const struct { int code; const char *word; } k_efficiency[] = {
    { -1, "NotAvailable" }, { -2, "LoadTooLow" }, { -3, "OutputOff" },
    { -4, "OnBattery" }, { -5, "InBypass" }, { -6, "BatteryCharging" },
    { -7, "PoorACInput" }, { -8, "BatteryDisconnected" },
};

void apcmb_decode_dynamic(const uint16_t regs[APCMB_DYN_COUNT], apcmb_reading_t *r)
{
    const int B = APCMB_DYN_ADDR;
    r->battery_voltage     = s(regs, 131, B, 5);
    r->battery_temperature = s(regs, 135, B, 7);
    r->load                = u(regs, 136, B, 8);
    /* NUT's _apc_modbus_power_to_nut: a percentage of the nameplate. Without
     * the nameplate NUT would publish 0; this publishes nothing. */
    r->realpower = r->realpower_nominal > 0 ? r->load / 100.0 * r->realpower_nominal : APCMB_ABSENT;
    r->power     = r->power_nominal > 0 ? u(regs, 138, B, 8) / 100.0 * r->power_nominal : APCMB_ABSENT;
    r->output_current      = u(regs, 140, B, 5);
    r->output_voltage      = u(regs, 142, B, 6);
    r->output_frequency    = u(regs, 144, B, 7);
    /* 0xFFFF is "not applicable": NUT publishes the text "NA", which no
     * client reading input.voltage as a number can use. Left out instead. */
    r->input_voltage = regs[151 - B] == 0xFFFF ? APCMB_ABSENT : u(regs, 151, B, 6);
    int16_t eff = (int16_t)regs[154 - B];
    r->efficiency_word = NULL;
    for (size_t k = 0; k < sizeof(k_efficiency) / sizeof(k_efficiency[0]); k++) {
        if (k_efficiency[k].code == eff) r->efficiency_word = k_efficiency[k].word;
    }
    r->efficiency = r->efficiency_word ? APCMB_ABSENT : s(regs, 154, B, 7);
}

static size_t add(apcmb_var_t *out, size_t n, const char *name, const char *fmt, double v)
{
    if (v == APCMB_ABSENT || n >= APCMB_MAX_VARS) return n;
    out[n].name = name;
    snprintf(out[n].value, sizeof(out[n].value), fmt, v);
    return n + 1;
}

size_t apcmb_vars(const apcmb_reading_t *r, apcmb_var_t out[APCMB_MAX_VARS])
{
    size_t n = 0;
    n = add(out, n, "input.voltage",         "%.2f", r->input_voltage);
    n = add(out, n, "output.voltage",        "%.2f", r->output_voltage);
    n = add(out, n, "output.current",        "%.2f", r->output_current);
    n = add(out, n, "output.frequency",      "%.2f", r->output_frequency);
    n = add(out, n, "ups.load",              "%.2f", r->load);
    n = add(out, n, "ups.realpower",         "%.2f", r->realpower);
    n = add(out, n, "ups.power",             "%.2f", r->power);
    n = add(out, n, "ups.realpower.nominal", "%.0f", r->realpower_nominal);
    n = add(out, n, "ups.power.nominal",     "%.0f", r->power_nominal);
    n = add(out, n, "battery.voltage",       "%.2f", r->battery_voltage);
    n = add(out, n, "battery.temperature",   "%.2f", r->battery_temperature);
    if (r->efficiency_word && n < APCMB_MAX_VARS) {
        out[n].name = "ups.efficiency";
        snprintf(out[n].value, sizeof(out[n].value), "%s", r->efficiency_word);
        n++;
    } else {
        n = add(out, n, "ups.efficiency", "%.1f", r->efficiency);
    }
    return n;
}

void apcmb_sched_init(apcmb_sched_t *s)
{
    memset(s, 0, sizeof(*s));
}

apcmb_step_t apcmb_sched_step(const apcmb_sched_t *s, int64_t now_us)
{
    if (now_us < s->next_us) return APCMB_IDLE;
    return s->live ? APCMB_READ : APCMB_PROBE;
}

unsigned apcmb_sched_done(apcmb_sched_t *s, apcmb_step_t step, apcmb_result_t r,
                          int64_t now_us)
{
    unsigned ev = 0;
    if (r == APCMB_R_YIELD || step == APCMB_IDLE) return 0;
    if (step == APCMB_PROBE) {
        if (r == APCMB_R_OK) {
            s->live = true;
            s->fails = 0;
            ev |= APCMB_EV_UP;
        } else {
            s->next_us = now_us + APCMB_RETRY_US;
            if (!s->told) { s->told = true; ev |= APCMB_EV_HINT; }
        }
        return ev;
    }
    if (r == APCMB_R_OK) {
        s->fails = 0;
    } else if (++s->fails >= APCMB_FAILS_TO_DROP) {
        s->live = false;
        s->fails = 0;
        s->next_us = now_us + APCMB_LOST_RETRY_US;
        ev |= APCMB_EV_DROP;
    }
    return ev;
}
