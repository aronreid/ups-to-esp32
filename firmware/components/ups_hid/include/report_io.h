/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A feature report as it comes off the wire, and as it goes back on.
 *
 * When a descriptor declares Report IDs, every report travels with its ID as
 * byte 0. When it declares none, every field is in report 0 and reports travel
 * with NO prefix at all: byte 0 is data. The firmware used to expect the ID
 * byte either way, so a UPS without Report IDs had its first data byte
 * compared against 0, nearly every read rejected, and sat at "waiting for a
 * status report" forever. NUT handles both; so does this.
 *
 * Kept free of ESP-IDF so tools/test/test_report_io.py can run it on the host.
 */
#ifndef UPSA_REPORT_IO_H
#define UPSA_REPORT_IO_H

#include <stdint.h>
#include <string.h>

/* Bytes on the wire for a report of `payload` bytes. */
static inline int report_wire_len(uint8_t report_id, int payload)
{
    return payload + (report_id ? 1 : 0);
}

/* `n` bytes read for report `report_id` -> at most `want` payload bytes in
 * `out`, the form hid_extract() expects. Returns the payload length, or -1 if
 * the reply is empty or carries a different report's ID. */
static inline int report_payload(const uint8_t *buf, int n, uint8_t report_id,
                                 uint8_t *out, int want)
{
    const uint8_t *p = buf;
    int len = n;
    if (report_id) {
        if (n < 2 || buf[0] != report_id) return -1;
        p++;
        len--;
    }
    if (len <= 0) return -1;
    if (len > want) len = want;
    memcpy(out, p, (size_t)len);
    return len;
}

/* Put `payload` bytes back into wire form in `wire` (room for payload + 1).
 * Returns the wire length. */
static inline int report_wire(uint8_t report_id, const uint8_t *payload, int len,
                              uint8_t *wire)
{
    if (report_id) {
        wire[0] = report_id;
        memcpy(wire + 1, payload, (size_t)len);
        return len + 1;
    }
    memcpy(wire, payload, (size_t)len);
    return len;
}

#endif
