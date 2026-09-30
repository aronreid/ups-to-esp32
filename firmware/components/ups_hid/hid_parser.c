/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "hid_parser.h"
#include "ups_profile.h"
#include "esp_log.h"
#include <string.h>
#include <math.h>

static const char *TAG = "hid_parse";

static const uint32_t *s_vendor_keep;
static size_t          s_vendor_keep_n;

void hid_parser_keep_vendor(const uint32_t *sorted, size_t count)
{
    s_vendor_keep = sorted;
    s_vendor_keep_n = count;
}

static bool vendor_kept(uint32_t u)
{
    size_t lo = 0, hi = s_vendor_keep_n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (s_vendor_keep[mid] == u) return true;
        if (s_vendor_keep[mid] < u) lo = mid + 1; else hi = mid;
    }
    return false;
}

/* ---- Unit decoding ---------------------------------------------------- */

int hid_unit_decimal_offset(uint32_t unit)
{
    if (unit == 0) return 0;                  /* dimensionless: %, counts */

    uint8_t system = unit & 0x0F;
    /* Only SI Linear (1) and SI Rotation (2) use cm/g bases. English systems
     * are not seen on UPSes; treat anything else as needing no shift rather
     * than guessing. */
    if (system != 1 && system != 2) return 0;

    /* Signed 4-bit nibbles: length at bits 4-7, mass at bits 8-11. */
    int8_t len  = (int8_t)((unit >> 4) & 0x0F);
    int8_t mass = (int8_t)((unit >> 8) & 0x0F);
    if (len  > 7) len  -= 16;
    if (mass > 7) mass -= 16;

    /* 1 m = 10^2 cm, 1 kg = 10^3 g. */
    return 2 * len + 3 * mass;
}

/* ---- Descriptor walk -------------------------------------------------- */

static hid_item_type_t main_tag_to_type(uint8_t tag, bool *is_data)
{
    *is_data = true;
    switch (tag) {
    case 0x8: return HID_ITEM_INPUT;
    case 0x9: return HID_ITEM_OUTPUT;
    case 0xB: return HID_ITEM_FEATURE;
    default:  *is_data = false; return HID_ITEM_INPUT;
    }
}

size_t hid_parse_report_descriptor(const uint8_t *desc, size_t len,
                                   hid_report_map_t *out)
{
    memset(out, 0, sizeof(*out));

    /* Bit cursors are per report ID AND per report type: one report ID can
     * carry both an Input and a Feature report with independent bit spaces. */
    static uint16_t cursor[256][HID_ITEM_TYPE_COUNT];
    memset(cursor, 0, sizeof(cursor));

    uint16_t usage_page = 0;
    uint8_t  report_id = 0;
    uint16_t report_size = 0, report_count = 0;
    int8_t   unit_exponent = 0;
    uint32_t unit = 0;
    int32_t  logical_min = 0;   /* sign of the range decides is_signed */
    int32_t  logical_max = 0;
    int32_t  phys_min = 0, phys_max = 0;
    bool     have_phys_min = false, have_phys_max = false;

    /* page << 16 | usage. A 2-byte Usage takes the Usage Page in force when
     * it is declared; a 4-byte one carries its own page. As NUT. */
    uint32_t usages[32];
    size_t   usage_count = 0;
    int32_t  usage_min = -1, usage_max = -1;

    /* Collection stack, as path nodes. A Collection takes its identity from
     * the FIRST usage declared before it (NUT's UsageTab[0]); an indexed one
     * pushes a second node, HID_PATH_INDEX | n. Deeper than
     * HID_PATH_MAX_DEPTH is clamped rather than overflowing. */
    uint32_t coll_node[HID_PATH_MAX_DEPTH];
    bool     coll_indexed[HID_PATH_MAX_DEPTH];  /* the node above is an index */
    size_t   coll_depth = 0;
    size_t   dropped_paths = 0;

    size_t truncated = 0;

    for (size_t i = 0; i < len; ) {
        uint8_t item = desc[i++];
        if (item == 0xFE) {                     /* long item: skip payload */
            if (i >= len) break;
            uint8_t sz = desc[i];
            i += 2 + sz;
            continue;
        }
        uint8_t size = item & 0x03;
        if (size == 3) size = 4;
        uint8_t type = (item >> 2) & 0x03;
        uint8_t tag  = (item >> 4) & 0x0F;

        if (i + size > len) break;
        uint32_t data = 0;
        for (uint8_t b = 0; b < size; b++) data |= (uint32_t)desc[i + b] << (8 * b);
        /* Sign-extend for the items where it matters. */
        int32_t sdata = (int32_t)data;
        if (size && (data & (1u << (size * 8 - 1)))) {
            sdata = (int32_t)(data | (0xFFFFFFFFu << (size * 8)));
        }
        i += size;

        if (type == 1) {                        /* Global */
            switch (tag) {
            case 0x0: usage_page = (uint16_t)data; break;
            case 0x1: logical_min = sdata; break;
            case 0x2: logical_max = sdata; break;
            case 0x3: phys_min = sdata; have_phys_min = true; break;
            case 0x4: phys_max = sdata; have_phys_max = true; break;
            case 0x5:
                unit_exponent = (int8_t)(data & 0x0F);
                if (unit_exponent > 7) unit_exponent -= 16;
                break;
            case 0x6: unit = data; break;
            case 0x7: report_size = (uint16_t)data; break;
            case 0x8: report_id = (uint8_t)data; break;
            case 0x9: report_count = (uint16_t)data; break;
            default: break;
            }
        } else if (type == 2) {                 /* Local */
            switch (tag) {
            case 0x0:
                if (usage_count < 32) {
                    usages[usage_count++] = (size == 4) ? data
                                          : ((uint32_t)usage_page << 16) | (data & 0xFFFF);
                }
                break;
            case 0x1: usage_min = (int32_t)data; break;
            case 0x2: usage_max = (int32_t)data; break;
            default: break;
            }
        } else if (type == 0) {                 /* Main */
            if (tag == 0x0A) {                  /* Collection */
                if (coll_depth < HID_PATH_MAX_DEPTH) {
                    coll_node[coll_depth] = usage_count ? usages[0] : 0;
                    coll_indexed[coll_depth] = false;
                    coll_depth++;
                    if (data >= 0x80 && coll_depth < HID_PATH_MAX_DEPTH) {
                        coll_node[coll_depth] = HID_PATH_INDEX | (data & 0x7F);
                        coll_indexed[coll_depth] = true;
                        coll_depth++;
                    }
                }
                usage_count = 0;
                usage_min = usage_max = -1;
                continue;
            }
            if (tag == 0x0C) {                  /* End Collection */
                if (coll_depth && coll_indexed[coll_depth - 1]) coll_depth--;
                if (coll_depth) coll_depth--;
                usage_count = 0;
                usage_min = usage_max = -1;
                continue;
            }

            bool is_data;
            hid_item_type_t itype = main_tag_to_type(tag, &is_data);

            if (is_data) {
                /* Nearest enclosing collection that actually disambiguates a
                 * value. UPS and PresentStatus wrap everything and say nothing,
                 * so they are skipped. */
                uint16_t enclosing = HID_COLL_NONE;
                for (size_t d = coll_depth; d-- > 0; ) {
                    if ((coll_node[d] >> 16) != HID_PAGE_POWER_DEVICE) continue;
                    uint16_t c = (uint16_t)coll_node[d];
                    if (c == HID_COLL_BATTERY_SYSTEM || c == HID_COLL_BATTERY ||
                        c == HID_COLL_CHARGER || c == HID_COLL_POWER_CONVERTER ||
                        c == HID_COLL_OUTLET_SYSTEM || c == HID_COLL_INPUT ||
                        c == HID_COLL_OUTPUT || c == HID_COLL_FLOW ||
                        c == HID_COLL_OUTLET || c == HID_COLL_POWER_SUMMARY) {
                        enclosing = c;
                        break;
                    }
                }
                /* Constant is NOT padding. Many UPSes flag their read-only
                 * readings Constant (0x23, 0xA3): charge, runtime, ACPresent on
                 * APC Smart-UPS, most of an Eaton, several CyberPowers. Taking
                 * the flag as "skip" left those units at 2-17 fields out of
                 * 500-2000 descriptor bytes and no status at all. NUT's parser
                 * keeps them too. Padding is a Constant item with NO usage,
                 * and the u >= 0 test below already skips that. */
                /* This item's path: an existing entry, or a new one. */
                uint8_t path_ix = 0;
                {
                    size_t p;
                    for (p = 0; p < out->path_count; p++) {
                        const hid_path_t *hp = &out->paths[p];
                        if (hp->depth == coll_depth &&
                            !memcmp(hp->node, coll_node, coll_depth * sizeof(uint32_t))) break;
                    }
                    if (p == out->path_count) {
                        if (out->path_count < HID_MAX_PATHS) {
                            out->paths[p].depth = (uint8_t)coll_depth;
                            memcpy(out->paths[p].node, coll_node, coll_depth * sizeof(uint32_t));
                            out->path_count++;
                        } else {
                            p = 0;          /* counted and reported below */
                            dropped_paths++;
                        }
                    }
                    path_ix = (uint8_t)p;
                }
                int shift = unit_exponent - hid_unit_decimal_offset(unit);
                /* As NUT's libhid.c logical_to_physical(): a physical range
                 * applies only if both ends are declared, not both zero, and
                 * both ranges are the right way round. Equal ranges are 1:1. */
                bool  phys = have_phys_min && have_phys_max &&
                             !(phys_min == 0 && phys_max == 0) &&
                             phys_max > phys_min && logical_max > logical_min &&
                             !(phys_min == logical_min && phys_max == logical_max);
                float pscale = phys ? (float)((double)(phys_max - phys_min) /
                                              (double)(logical_max - logical_min)) : 1.0f;
                float poff   = phys ? (float)((double)phys_min - (double)logical_min * pscale)
                                    : 0.0f;

                for (uint16_t n = 0; n < report_count; n++) {
                    /* Usage for this instance: explicit list first, then a
                     * Usage Minimum/Maximum range, else repeat the last one. */
                    int64_t u = -1;           /* page << 16 | usage */
                    if (n < usage_count) {
                        u = usages[n];
                    } else if (usage_min >= 0 && usage_max >= usage_min) {
                        int32_t ranged = usage_min + (int32_t)n - (int32_t)usage_count;
                        if (ranged <= usage_max) u = ((uint32_t)usage_page << 16) | (uint32_t)ranged;
                    } else if (usage_count) {
                        u = usages[usage_count - 1];
                    }
                    uint16_t upage = (uint16_t)((uint64_t)u >> 16);
                    bool relevant = (upage == HID_PAGE_POWER_DEVICE ||
                                     upage == HID_PAGE_BATTERY_SYSTEM ||
                                     (upage >= 0xFF00 && u >= 0 && vendor_kept((uint32_t)u)));

                    if (relevant && u >= 0) {
                        if (out->count < HID_MAX_FIELDS) {
                            hid_field_t *f = &out->fields[out->count++];
                            f->usage_page    = upage;
                            f->usage         = (uint16_t)u;
                            f->path          = path_ix;
                            f->collection    = enclosing;
                            f->report_id     = report_id;
                            f->item_type     = itype;
                            f->bit_offset    = cursor[report_id][itype];
                            f->bit_size      = report_size;
                            f->unit_exponent = unit_exponent;
                            f->unit          = unit;
                            f->decimal_shift = (int8_t)shift;
                            f->has_phys      = phys;
                            f->phys_scale    = pscale;
                            f->phys_offset   = poff;
                            f->is_signed     = (logical_min < 0);
                            f->found         = true;
                        } else {
                            truncated++;
                        }
                    }
                    cursor[report_id][itype] += report_size;
                }
            }
            /* Local items reset after every Main item, global ones persist. */
            usage_count = 0;
            usage_min = usage_max = -1;
        }
    }

    if (dropped_paths) {
        ESP_LOGE(TAG, "more than %d distinct collection paths; %u fields share "
                      "path 0 -- raise HID_MAX_PATHS", HID_MAX_PATHS, (unsigned)dropped_paths);
    }
    if (truncated) {
        ESP_LOGE(TAG, "descriptor has more fields than HID_MAX_FIELDS (%d); "
                      "dropped %u -- raise the limit",
                 HID_MAX_FIELDS, (unsigned)truncated);
    }
    ESP_LOGI(TAG, "parsed %u power/battery fields from %u descriptor bytes",
             (unsigned)out->count, (unsigned)len);
    return out->count;
}

/* ---- Lookup ----------------------------------------------------------- */

const hid_field_t *hid_find_typed(const hid_report_map_t *map,
                                  uint16_t usage_page, uint16_t usage,
                                  hid_item_type_t type)
{
    for (size_t i = 0; i < map->count; i++) {
        const hid_field_t *f = &map->fields[i];
        if (f->usage_page == usage_page && f->usage == usage && f->item_type == type) {
            return f;
        }
    }
    return NULL;
}

const hid_field_t *hid_find_in(const hid_report_map_t *map,
                               uint16_t usage_page, uint16_t usage,
                               uint16_t collection)
{
    /* Feature first, as in hid_find. */
    for (int pass = 0; pass < 2; pass++) {
        hid_item_type_t want = pass ? HID_ITEM_INPUT : HID_ITEM_FEATURE;
        for (size_t i = 0; i < map->count; i++) {
            const hid_field_t *f = &map->fields[i];
            if (f->usage_page == usage_page && f->usage == usage &&
                f->collection == collection && f->item_type == want) {
                return f;
            }
        }
    }
    return NULL;
}

const hid_field_t *hid_find(const hid_report_map_t *map,
                            uint16_t usage_page, uint16_t usage)
{
    /* Feature first: that is where UPSes keep authoritative values, and the
     * same usage often appears as both Input and Feature. */
    const hid_field_t *f = hid_find_typed(map, usage_page, usage, HID_ITEM_FEATURE);
    if (f) return f;
    return hid_find_typed(map, usage_page, usage, HID_ITEM_INPUT);
}

/* ---- Extraction ------------------------------------------------------- */

float hid_extract(const hid_field_t *f, const uint8_t *report, size_t len)
{
    if (!f || !f->found || f->bit_size == 0 || f->bit_size > 32) return 0.0f;

    /* bit_offset is relative to the payload; the caller strips the report ID. */
    uint32_t raw = 0;
    for (uint16_t b = 0; b < f->bit_size; b++) {
        uint16_t bit = f->bit_offset + b;
        if (bit / 8 >= len) break;
        if (report[bit / 8] & (1u << (bit % 8))) raw |= 1u << b;
    }

    float v;
    if (f->is_signed && f->bit_size < 32 && (raw & (1u << (f->bit_size - 1)))) {
        v = (float)(int32_t)(raw | (0xFFFFFFFFu << f->bit_size));
    } else if (f->is_signed && f->bit_size == 32) {
        /* A 32-bit signed field needs no extension, just the cast. Missing
         * it turned Eaton's, APC Smart-UPS's and PowerWalker's delay of -1
         * ("no countdown") into 4294967295 s (NUT's logs: -1). */
        v = (float)(int32_t)raw;
    } else {
        v = (float)raw;
    }

    if (f->has_phys) v = v * f->phys_scale + f->phys_offset;
    /* decimal_shift already accounts for HID's cm/g base units. */
    return v * powf(10.0f, (float)f->decimal_shift);
}

bool hid_to_raw(const hid_field_t *f, double physical, bool round_up, uint32_t *raw)
{
    if (!f || !f->found || f->bit_size == 0 || f->bit_size > 32) return false;
    double x = physical / pow(10.0, (double)f->decimal_shift);
    if (f->has_phys && f->phys_scale != 0.0f) x = (x - f->phys_offset) / f->phys_scale;
    /* Refuse before rounding: rounding -5 s up gives 0, which is "now". */
    if (x < 0) return false;
    /* A tiny epsilon so 60 s / 60 is 1 minute, not 2 from float noise. */
    x = round_up ? ceil(x - 1e-6) : floor(x + 0.5);
    if (x < 0) return false;
    double hi = (f->bit_size >= 32) ? 4294967295.0 : (double)((1ull << f->bit_size) - 1);
    if (x > hi) return false;
    *raw = (uint32_t)x;
    return true;
}

uint32_t hid_extract_raw(const hid_field_t *f, const uint8_t *report, size_t len)
{
    if (!f || !f->found || f->bit_size == 0 || f->bit_size > 32) return 0;
    uint32_t raw = 0;
    for (uint16_t b = 0; b < f->bit_size; b++) {
        uint16_t bit = f->bit_offset + b;
        if (bit / 8 >= len) break;
        if (report[bit / 8] & (1u << (bit % 8))) raw |= 1u << b;
    }
    return raw;
}

/* ---- Profiles --------------------------------------------------------- */

const ups_profile_t *hid_apply_quirks(hid_report_map_t *map,
                                      uint16_t vid, uint16_t pid)
{
    const ups_profile_t *p = ups_profile_find(vid, pid);

    ESP_LOGI(TAG, "device %04x:%04x -> profile \"%s\"",
             vid, pid, ups_profile_name(p));

    if (p && p->fixup) p->fixup(map);
    return p;
}
