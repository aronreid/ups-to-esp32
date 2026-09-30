/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The last crash, remembered across the reboot it caused.
 *
 * A board in the field that resets itself says only "interrupt watchdog" --
 * not where. This keeps the panic's own reason and a short backtrace in RTC
 * memory, which survives a software reset, and on the next boot moves it to
 * NVS so it also survives the power cycles after. It is shown on the page and
 * sent with the fleet heartbeat. Decode the addresses against the release's
 * ELF: xtensa-esp32s3-elf-addr2line -pfiaC -e ups-adaptor-reva.elf 0x....
 */
#ifndef UPSA_CRASHLOG_H
#define UPSA_CRASHLOG_H

/* Call once, early in app_main, after nvs_flash_init(). */
void crashlog_init(void);

/* "Interrupt wdt timeout on CPU1 @ 0x4037a1b2 bt 0x...:0x... v0.09-rc2", or
 * "" if this board has never crashed since the record was cleared. */
const char *crashlog_last(void);

/* Forget it (the page's "clear" and a factory reset). */
void crashlog_clear(void);

#endif /* UPSA_CRASHLOG_H */
