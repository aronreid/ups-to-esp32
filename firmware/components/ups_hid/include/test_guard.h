/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Pure decision logic for refusing a deep battery test, pulled out of
 * ups_hid.c so it can be compiled and tested on the host
 * (tools/test/test_test_guard.py).
 */
#ifndef UPSA_TEST_GUARD_H
#define UPSA_TEST_GUARD_H

#include <stdbool.h>

/* A deep test runs the battery down (an APC RS 1000G went 100 -> 22 % in 40
 * minutes). Started on a battery that is not full it leaves the UPS with
 * less reserve than its own estimate says, for the length of the test, with
 * the mains still able to fail. Refuse it below 100 %.
 *
 * `charge_valid` is false when the UPS does not report a charge: there is
 * nothing to judge by, so the test is allowed rather than blocked on data
 * the UPS never had. Charge is a float percent; 99.5 absorbs rounding. */
static inline bool deep_test_allowed(bool charge_valid, float charge_pct)
{
    return !charge_valid || charge_pct >= 99.5f;
}

#endif /* UPSA_TEST_GUARD_H */
