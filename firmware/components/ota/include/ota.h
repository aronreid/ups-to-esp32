/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Firmware update over the air, from this project's GitHub releases.
 *
 * WHY A RELEASE AND NOT A BRANCH. A device that follows a branch installs
 * whatever was pushed last, including a commit that was never meant for
 * hardware. A release is a deliberate act with a tag on it, so publishing one
 * is the only way to move devices.
 *
 * WHICH BINARY. Assets are named ups-adaptor-<target>.bin, and the target
 * comes from BOARD_OTA_TARGET. The two boards have different pin maps: a Rev A
 * image on a devkit drives GPIO4 as a VBUS enable that is not there, and a
 * devkit image on Rev A never raises VBUS at all, so the UPS never enumerates.
 * An asset for this device's target must exist or nothing is installed.
 *
 * ROLLBACK. The new image is written to the unused slot and booted. It must
 * then reach a working state and call ota_confirm(); if it panics or is reset
 * before that, the bootloader reverts to the previous slot. A device in a
 * closet cannot be recovered by hand, so this is not optional.
 */
#ifndef UPSA_OTA_H
#define UPSA_OTA_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef enum {
    OTA_IDLE = 0,
    OTA_CHECKING,       /* asking GitHub what the latest release is */
    OTA_AVAILABLE,      /* a different version exists for this target */
    OTA_UP_TO_DATE,
    OTA_DOWNLOADING,
    OTA_INSTALLED,      /* written and verified; reboot to run it */
    OTA_FAILED,
    /* The published release exists but its image is built for another board,
     * so this device will not install it however many times it is asked. A
     * state of its own rather than a flavour of OTA_FAILED, because nothing
     * the owner can do here will help and the page should stop offering a
     * button that only dead-ends. */
    OTA_INCOMPATIBLE,
} ota_state_t;

/* CONSENT. Two separate things, and only one of them happens on its own:
 *
 *   CHECKING  asks GitHub what the latest release is. Off unless enabled, and
 *             the status page says plainly that it contacts github.com -- a
 *             device on a private network reaching the internet on a timer is
 *             the owner's decision, not ours.
 *   INSTALLING  never happens automatically. Ever. It writes flash and reboots
 *             a device that something in the house may be depending on, and no
 *             amount of confidence in a release justifies doing that unasked.
 *
 * So the most this can do by itself is notice, and say so.
 */
typedef struct {
    ota_state_t state;
    char  running[48];     /* version this firmware was built as */
    char  latest[48];      /* newest release tag seen, "" if unknown */
    char  error[96];       /* why the last attempt failed, "" if none */
    int   progress;        /* 0-100 while downloading */
    bool  pending_verify;  /* running from a freshly installed image */
    bool  auto_check;      /* permitted to ask GitHub on a timer */
    bool  checked_once;    /* has ever completed a check this boot */
    /* Running something that is not a release: a local build, stamped with a
     * commit hash rather than a tag. Every published release then "differs",
     * so one is offered as an update -- usually an OLDER one, which would
     * throw away whatever is being tested. The offer stands, because
     * returning a bench board to a release is legitimate, but the page makes
     * it quiet rather than a red alert. */
    bool  dev_build;
    /* A tag this board downloaded and then refused on identity, "" if none.
     * Kept so the twelve-hourly check stops re-offering an image that was
     * already rejected once. Deliberately not persisted: the usual fix is to
     * re-upload the asset under the same tag, and a memory that survived a
     * reboot would keep refusing the corrected file. An explicit check from
     * the page clears it, so a person is never stuck. */
    char  refused[48];
} ota_status_t;

esp_err_t ota_start(void);              /* start the worker task */
void      ota_get(ota_status_t *out);
esp_err_t ota_check(void);              /* ask GitHub, non-blocking */
esp_err_t ota_install(void);            /* download and write, non-blocking */

/* Permit or forbid the periodic check. Stored in NVS, so it survives a reboot
 * and an update. Installing still needs a person either way. */
esp_err_t ota_set_auto_check(bool enabled);

/* Marks the running image good and cancels the pending rollback -- but only
 * once the device has EARNED it. Returns true when the image is confirmed (or
 * was never on trial), false while it still has conditions left to meet.
 *
 * THE RETURN VALUE IS THE POINT. This used to return void, and its caller
 * called it once, the moment Wi-Fi associated, and latched a local "confirmed"
 * flag whether or not anything had been confirmed. The gate below refuses an
 * image five seconds into its life, correctly -- and was then never asked
 * again, so the rollback stayed armed for the life of the image and any power
 * cut downgraded the device to the version it had just replaced. Keep calling
 * this until it returns true.
 *
 * The first version confirmed as soon as Wi-Fi associated. That is worthless
 * against the bugs that actually shipped here: a handler table too small to
 * register, and a web server stack too small to answer. Both let the device
 * boot, associate, and confirm itself as healthy seconds before the first
 * request killed it -- cancelling the rollback that existed to catch exactly
 * that. Associating proves the radio works, nothing more. */
bool      ota_confirm(void);

/* Evidence that the web server is not merely listening but answering. Called
 * by the HTTP layer on each completed request. */
void      ota_note_request_served(void);

#endif /* UPSA_OTA_H */
