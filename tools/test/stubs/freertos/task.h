/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef STUB_FREERTOS_TASK_H
#define STUB_FREERTOS_TASK_H
#include "freertos/FreeRTOS.h"
typedef void *TaskHandle_t;
static inline void vTaskDelay(TickType_t t) { (void)t; }
static inline BaseType_t xTaskCreate(void (*fn)(void *), const char *n, uint32_t s,
                                     void *p, int pr, TaskHandle_t *h)
{ (void)fn; (void)n; (void)s; (void)p; (void)pr; (void)h; return pdPASS; }
static inline void vTaskDelete(TaskHandle_t h) { (void)h; }
#endif
