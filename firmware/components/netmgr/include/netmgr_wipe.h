/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * What a factory reset erases beyond settings: the outage log and the long
 * history, which record when the previous owner's mains failed and how their
 * UPS behaved. Shared by both netmgr variants (Wi-Fi and Ethernet).
 *
 * Order matters. The partition is erased FIRST and the log's NVS namespace
 * after it: that namespace holds the "forget records before this number"
 * mark (outagelog_clear), so erasing it while the records are still in flash
 * would bring back every outage the owner had cleared. If power is lost
 * halfway, the mark is still there and nothing reappears.
 *
 * The erase is in 64 KB blocks with a pause between them, so the idle task
 * (and with it the task watchdog) keeps running through the tens of seconds a
 * 10 MB partition takes.
 */
#ifndef UPSA_NETMGR_WIPE_H
#define UPSA_NETMGR_WIPE_H

#include "esp_partition.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Must match OUTAGELOG_PART_SUBTYPE / OUTAGELOG_PART_LABEL in outagelog.h
 * (tools/test/test_factory_reset.py pins that they do). */
#define NETMGR_WIPE_PART_SUBTYPE  0x40
#define NETMGR_WIPE_PART_LABEL    "outagelog"
#define NETMGR_WIPE_NVS_NS        "olog"

static inline void netmgr_wipe_logs(void)
{
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                        NETMGR_WIPE_PART_SUBTYPE,
                                                        NETMGR_WIPE_PART_LABEL);
    if (p) {
        const uint32_t blk = 64 * 1024;
        for (uint32_t off = 0; off < p->size; off += blk) {
            uint32_t n = p->size - off < blk ? p->size - off : blk;
            if (esp_partition_erase_range(p, off, n) != ESP_OK) return;   /* keep the mark */
            vTaskDelay(1);
        }
    }
    nvs_handle_t h;
    if (nvs_open(NETMGR_WIPE_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
}

#endif
