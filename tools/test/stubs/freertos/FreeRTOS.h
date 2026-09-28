/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef STUB_FREERTOS_H
#define STUB_FREERTOS_H
#include <stdint.h>
#include <stddef.h>   /* the real headers provide NULL; so must the stub */
typedef int BaseType_t;
typedef uint32_t TickType_t;
#define pdPASS 1
#define pdFALSE 0
#define pdTRUE 1
#define portMAX_DELAY 0xFFFFFFFFu
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#endif
