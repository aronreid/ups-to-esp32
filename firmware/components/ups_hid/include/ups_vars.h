/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The pure half of NUT's variable tables: which table a UPS gets, whether a
 * row matches a field, and how a value is formatted and sanity-checked. No
 * ESP-IDF, so tools/test/test_vars.py runs exactly this code on the host
 * against real descriptors and NUT's own decoded values. ups_hid.c does the
 * USB reads and holds the results.
 */
#ifndef UPSA_UPS_VARS_H
#define UPSA_UPS_VARS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "hid_parser.h"
#include "ups_vartab.h"

/* NUT's subdriver for a USB ID: exact VID:PID first, then the vendor; -1 for
 * none (the common rows). */
int  ups_vars_sub_for(uint16_t vid, uint16_t pid);

/* Is field `f` of `map` the one row `r` names: same usage, same full path? */
bool ups_vars_row_matches(const hid_report_map_t *map, const hid_field_t *f,
                          const ups_vartab_row_t *r);

/* The value NUT would publish for row `r` from one report's payload, or false
 * for no value: not in the lookup, or outside what any working UPS reports.
 * VCONV_STRINGID needs a USB read and is not handled here (false). */
bool ups_vars_format(const ups_vartab_row_t *r, const hid_field_t *f,
                     const uint8_t *payload, int len, char *out, size_t cap);

#endif
