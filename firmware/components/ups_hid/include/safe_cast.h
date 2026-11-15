/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Float-to-integer conversions that cannot be undefined.
 *
 * A reading is scaled by what the UPS's descriptor says (units, exponent,
 * physical range), so a hostile or broken device chooses how big the float is.
 * In C, converting a float that does not fit the integer type is undefined
 * behaviour. Xtensa happens to saturate, so the board is not hurt, but a
 * host build (the tests) may not agree, and "happens to" is not a guarantee.
 * These clamp first: NaN and anything below the range give the minimum
 * (0 for the unsigned ones), anything above gives the maximum.
 */
#ifndef UPSA_SAFE_CAST_H
#define UPSA_SAFE_CAST_H

#include <limits.h>
#include <stdint.h>

static inline uint8_t ups_f2u8(double v)
{
    if (!(v > 0.0)) return 0;                       /* also NaN */
    if (v >= 255.0) return 255;
    return (uint8_t)v;
}

static inline uint32_t ups_f2u32(double v)
{
    if (!(v > 0.0)) return 0;
    if (v >= 4294967295.0) return 4294967295u;
    return (uint32_t)v;
}

static inline long ups_f2l(double v)
{
    if (!(v == v)) return 0;                        /* NaN */
    if (v <= (double)LONG_MIN) return LONG_MIN;
    if (v >= (double)LONG_MAX) return LONG_MAX;
    return (long)v;
}

#endif
