/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * When a failed USB setup is worth a diagnostic log upload. Pure logic,
 * pulled out of ups_hid.c so it can be compiled and tested on the host
 * (tools/test/test_enum_fail_upload.py).
 *
 * One failed step that the next try gets past is normal: a UPS whose USB
 * chip isn't ready at power-up fails the first port reset and attaches half
 * a second later (seen on a Tripp Lite SMART1500LCDT). Uploading a log
 * for that made every such boot look like GitHub #24's UPS that never
 * attaches (GitLab #100). So a failure arms a grace period, and when it ends
 * the log goes only if no UPS is attached, or if the setup failed again
 * within it. The link log and the WARN line are kept either way.
 */
#ifndef UPSA_ENUM_FAIL_UPLOAD_H
#define UPSA_ENUM_FAIL_UPLOAD_H

#include <stdbool.h>
#include <stdint.h>

#define EFU_GRACE_US (30LL * 1000000)

typedef struct {
    int64_t  due_us;        /* when the grace period ends; 0 = none running */
    uint32_t fails_at_arm;  /* the since-boot failure count that armed it */
} efu_t;

static inline void efu_init(efu_t *e)
{
    e->due_us = 0;
    e->fails_at_arm = 0;
}

/* A failed setup; `fails` is the count since boot. Later failures within a
 * running grace period don't extend it. */
static inline void efu_fail(efu_t *e, uint32_t fails, int64_t now_us)
{
    if (e->due_us) return;
    e->due_us = now_us + EFU_GRACE_US;
    if (e->due_us == 0) e->due_us = 1;
    e->fails_at_arm = fails;
}

/* Call every loop. True once, when the grace period ends and the failure
 * still matters: no UPS attached, or another failure came in meanwhile. */
static inline bool efu_poll(efu_t *e, uint32_t fails, bool attached, int64_t now_us)
{
    if (!e->due_us || now_us < e->due_us) return false;
    bool again = fails != e->fails_at_arm;
    e->due_us = 0;
    return !attached || again;
}

#endif /* UPSA_ENUM_FAIL_UPLOAD_H */
