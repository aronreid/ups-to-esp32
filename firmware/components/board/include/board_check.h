/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Is this firmware on the board it was built for? Pure, so
 * tools/test/test_board_check.py compiles it on the host.
 *
 * A cable install (esptool, myupspal.com/flash) writes whatever it is given:
 * the image-name and signature checks are only on the over-the-air path. So a
 * Rev A image can land on a Rev B board, where it runs on the wrong pins (and
 * may never power the UPS's USB). The flash chip says which board this is:
 * Rev A is an N4 module, 4 MB (early prototypes 8 MB); Rev B is an N16R8,
 * 16 MB. Rev A firmware on 16 MB or more is told so.
 *
 * The other way round cannot be reported: a Rev B image needs the PSRAM a Rev
 * A does not have and stops before any of this runs. The /flash page says to
 * install again with the right board if one does not start.
 *
 * Dev board targets are left alone: they run on any S3, any flash size.
 */
#ifndef BOARD_CHECK_H
#define BOARD_CHECK_H

#include <stdint.h>
#include <stddef.h>

enum { BOARD_CHECK_OTHER = 0, BOARD_CHECK_REV_A = 1, BOARD_CHECK_REV_B = 2 };

#define BOARD_CHECK_REVB_FLASH (16u * 1024 * 1024)

/* NULL when all is well, else a sentence for the page and the log. */
static inline const char *board_check(int target, uint32_t flash_bytes)
{
    if (target == BOARD_CHECK_REV_A && flash_bytes >= BOARD_CHECK_REVB_FLASH) {
        return "This firmware is for Rev A, but this board has 16 MB of flash, like a Rev B. "
               "Install the Rev B firmware from myupspal.com/flash.";
    }
    if (target == BOARD_CHECK_REV_B && flash_bytes != 0 && flash_bytes < BOARD_CHECK_REVB_FLASH) {
        return "This firmware is for Rev B, but this board has less flash than a Rev B. "
               "Install the Rev A firmware from myupspal.com/flash.";
    }
    return NULL;
}

#endif /* BOARD_CHECK_H */
