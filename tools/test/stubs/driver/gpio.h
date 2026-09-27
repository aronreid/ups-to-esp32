/* SPDX-License-Identifier: GPL-3.0-or-later
 * Enough of driver/gpio.h to syntax-check a pin map. */
#ifndef STUB_DRIVER_GPIO_H
#define STUB_DRIVER_GPIO_H
#include <stdint.h>
#include "esp_err.h"

typedef int gpio_num_t;
#define GPIO_NUM_NC (-1)
#define GPIO_NUM_0 0
#define GPIO_NUM_3 3
#define GPIO_NUM_4 4
#define GPIO_NUM_5 5
#define GPIO_NUM_8 8
#define GPIO_NUM_9 9
#define GPIO_NUM_19 19
#define GPIO_NUM_20 20
#define GPIO_NUM_43 43
#define GPIO_NUM_44 44
#define GPIO_NUM_45 45
#define GPIO_NUM_46 46
#define GPIO_NUM_47 47
#define GPIO_NUM_48 48

typedef enum { GPIO_MODE_INPUT, GPIO_MODE_OUTPUT } gpio_mode_t;
typedef enum { GPIO_PULLUP_DISABLE, GPIO_PULLUP_ENABLE } gpio_pullup_t;
typedef enum { GPIO_PULLDOWN_DISABLE, GPIO_PULLDOWN_ENABLE } gpio_pulldown_t;
typedef enum { GPIO_INTR_DISABLE } gpio_int_type_t;

typedef struct {
    uint64_t pin_bit_mask;
    gpio_mode_t mode;
    gpio_pullup_t pull_up_en;
    gpio_pulldown_t pull_down_en;
    gpio_int_type_t intr_type;
} gpio_config_t;

static inline esp_err_t gpio_config(const gpio_config_t *c) { (void)c; return ESP_OK; }
static inline esp_err_t gpio_set_level(gpio_num_t g, uint32_t l) { (void)g; (void)l; return ESP_OK; }
static inline int gpio_get_level(gpio_num_t g) { (void)g; return 1; }
#endif
