/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef UPSA_UPS_HID_H
#define UPSA_UPS_HID_H

#include "ups_data.h"
#include "esp_err.h"

/* Start the USB host stack and the UPS polling task.
 *
 * Owns the whole device lifecycle: enumerate, fetch and parse the report
 * descriptor, then poll Feature reports on an interval. Survives the UPS being
 * unplugged, replugged or power-cycled mid-session without a reboot -- that is
 * a hard requirement, not a nicety (CLAUDE.md, "Things that will bite").
 */
esp_err_t ups_hid_start(void);

/* Copy the current snapshot. Thread-safe; takes an internal mutex.
 * Always populated: check .attached before trusting the readings. */
void ups_hid_get(ups_data_t *out);

/* Force a VBUS power-cycle and re-enumeration. Exposed so the web UI can offer
 * a "reset UPS link" button, and so the watchdog can call it automatically. */
esp_err_t ups_hid_recover(void);

/* Commands a UPS may support. Each maps onto one writable Feature usage, and
 * every one is refused with ESP_ERR_NOT_SUPPORTED unless the corresponding bit
 * is set in ups_data_t.caps -- discovered from the descriptor, not assumed. */
typedef enum {
    UPS_CMD_TEST_QUICK,     /* start a quick battery self-test      */
    UPS_CMD_TEST_DEEP,      /* start a deep (runtime calibration)   */
    UPS_CMD_TEST_ABORT,     /* stop a test in progress              */
    UPS_CMD_BEEPER_DISABLE, /* silence it permanently               */
    UPS_CMD_BEEPER_ENABLE,  /* allow it to sound again              */
    UPS_CMD_BEEPER_MUTE,    /* silence the current alarm only       */
    UPS_CMD_LOWBATT_LIMIT,  /* arg = percent, the LB threshold      */
    UPS_CMD_LOAD_OFF,       /* arg = seconds, then the outlets die  */
    UPS_CMD_BATTERY_DATE,   /* arg = HID packed date, see below     */
} ups_cmd_t;

/* Send a command. `arg` is used only by the two commands that take one.
 *
 * UPS_CMD_LOAD_OFF is not like the others: it cuts power to everything the UPS
 * is feeding, and on a finished board that includes this adaptor. There is no
 * matching "on" command on every UPS -- this APC has DelayBeforeShutdown and
 * no DelayBeforeStartup -- so the load stays dead until mains returns. Callers
 * are expected to have confirmed with a human first. */
esp_err_t ups_hid_command(ups_cmd_t cmd, int32_t arg);

/* The attached UPS's raw HID report descriptor, as the device returned it.
 *
 * This parser is meant to work across UPS makes, and the only way anyone finds
 * out where it does not is by looking at a descriptor it got wrong. Exposing
 * the bytes means a person with an unsupported UPS can send the exact input
 * this firmware saw, and tools/decode-hid.py reads the same file.
 *
 * Returns the length and points `out` at the buffer, or 0 with `out` untouched
 * when nothing is attached. The buffer lives as long as the attachment. */
size_t ups_hid_report_descriptor(const uint8_t **out);

/* UPS_CMD_BATTERY_DATE takes the HID packed form: bits 0-4 day, 5-8 month,
 * 9-15 year since 1980. ESP_OK means the UPS ACKed the transfer, NOT that it
 * kept the value -- read battery_mfr_date back to find out. */
#define UPS_PACK_DATE(y, m, d) \
    ((int32_t)((((y) - 1980) & 0x7F) << 9 | ((m) & 0x0F) << 5 | ((d) & 0x1F)))

#endif /* UPSA_UPS_HID_H */
