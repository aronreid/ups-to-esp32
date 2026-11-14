/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Pure decision logic for status_hold(), pulled out of ups_hid.c so it can be
 * compiled and tested on the host (tools/test/test_status_hold.py) without
 * the rest of that file's ESP-IDF, USB host and mbedtls dependencies.
 */
#ifndef UPSA_STATUS_HOLD_H
#define UPSA_STATUS_HOLD_H

#include <stdint.h>

/* UPS_STATUS_UNKNOWN from ups_data.h, repeated here rather than included: it
 * is 0 by definition (ups_data.h's ups_status_bits_t), and pulling in that
 * header would pull in the rest of ups_data_t's dependencies for one enum
 * value. status_hold_test.c below asserts the two stay equal. */
#define STATUS_HOLD_UNKNOWN 0u

/* Should a fresh status read of `raw` replace `prev`, or should `prev` be
 * held a while longer because this looks like the transient all-zero blip
 * several UPSes show for a minute or two after a self-test (NUT #2104, which
 * made upsmon shut hosts down when a status glitch like this one was taken
 * at face value)?
 *
 * `hold_since_us` is the caller's per-UPS clock: 0 when not currently
 * holding, else when the hold started. This function only ever starts that
 * clock or resets it to 0 -- it never gives up on its own. The caller (a
 * periodic loop, not this function) decides when the hold has run long
 * enough and forces a real recovery; see STATUS_HOLD_US in ups_hid.c. */
static inline uint32_t status_hold(uint32_t raw, uint32_t prev,
                                    int64_t *hold_since_us, int64_t now_us)
{
    if (raw == STATUS_HOLD_UNKNOWN && prev != STATUS_HOLD_UNKNOWN) {
        if (*hold_since_us == 0) *hold_since_us = now_us;
        return prev;
    }
    *hold_since_us = 0;
    return raw;
}

#endif /* UPSA_STATUS_HOLD_H */
