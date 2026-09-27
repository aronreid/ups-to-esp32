/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Usage + collection -> NUT variable name.
 *
 * The goal is to pass through whatever the UPS actually provides, not a curated
 * subset. The parser already discovers every Power Device and Battery System
 * field in the descriptor; this table gives each one its standard NUT name so
 * clients recognise it.
 *
 * Why a table at all, rather than emitting raw usages? NUT variable names are a
 * defined namespace that Home Assistant, Synology and upsmon parse. A variable
 * they do not recognise is ignored, so inventing names achieves nothing on that
 * channel. NUT's own usbhid-ups works exactly this way.
 *
 * So: standard names on NUT, and everything else -- including usages with no
 * NUT equivalent -- exposed raw over HTTP and MQTT where no namespace applies.
 * Nothing the UPS reports is discarded; it just reaches different outputs.
 */
#ifndef UPSA_UPS_VARMAP_H
#define UPSA_UPS_VARMAP_H

#include <stdbool.h>
#include <stdint.h>
#include "hid_parser.h"

typedef enum {
    UPS_FMT_INT,      /* "%.0f"  */
    UPS_FMT_DEC1,     /* "%.1f"  */
    UPS_FMT_DEC2,     /* "%.2f"  */
    UPS_FMT_STRING,   /* fetched via a string descriptor, not a number */
} ups_fmt_t;

typedef struct {
    uint16_t    usage_page;
    uint16_t    usage;
    uint16_t    collection;   /* HID_COLL_NONE matches any collection */
    const char *nut_name;
    ups_fmt_t   fmt;
} ups_varmap_t;

/* NUT name for a parsed field, or NULL when the usage has no standard NUT
 * equivalent. NULL is normal: such fields still reach /api/raw and MQTT. */
const ups_varmap_t *ups_varmap_lookup(const hid_field_t *f);

/* Iterate the whole table, for building LIST VAR and for diagnostics. */
const ups_varmap_t *ups_varmap_table(size_t *count);

#endif /* UPSA_UPS_VARMAP_H */
