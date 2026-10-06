/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int gpio_num_t;

enum {
    GPIO_NUM_NC = -1,
    GPIO_NUM_0 = 0,
    GPIO_NUM_1,
    GPIO_NUM_2,
    GPIO_NUM_3,
    GPIO_NUM_4,
    GPIO_NUM_5,
    GPIO_NUM_6,
    GPIO_NUM_7,
    GPIO_NUM_8,
    GPIO_NUM_9,
    GPIO_NUM_10,
    GPIO_NUM_11,
    GPIO_NUM_12,
    GPIO_NUM_13,
    GPIO_NUM_14,
    GPIO_NUM_15,
    GPIO_NUM_16,
    GPIO_NUM_17,
    GPIO_NUM_18,
    GPIO_NUM_19,
    GPIO_NUM_20,
    GPIO_NUM_21,
    GPIO_NUM_22,
    GPIO_NUM_23,
    GPIO_NUM_24,
    GPIO_NUM_25,
    GPIO_NUM_26,
    GPIO_NUM_27,
    GPIO_NUM_28,
    GPIO_NUM_29,
    GPIO_NUM_30,
    GPIO_NUM_31,
    GPIO_NUM_32,
    GPIO_NUM_33,
    GPIO_NUM_34,
    GPIO_NUM_35,
    GPIO_NUM_36,
    GPIO_NUM_37,
    GPIO_NUM_38,
    GPIO_NUM_39,
    GPIO_NUM_40,
    GPIO_NUM_41,
    GPIO_NUM_42,
    GPIO_NUM_43,
    GPIO_NUM_44,
    GPIO_NUM_45,
    GPIO_NUM_46,
    GPIO_NUM_47,
    GPIO_NUM_48,
    GPIO_NUM_MAX,
};

typedef enum {
    GPIO_MODE_DISABLE,
    GPIO_MODE_INPUT = 1,
    GPIO_MODE_OUTPUT = 2,
    GPIO_MODE_OUTPUT_OD = 6,
    GPIO_MODE_INPUT_OUTPUT_OD = 7,
    GPIO_MODE_INPUT_OUTPUT = 3,
} gpio_mode_t;

typedef enum { GPIO_PULLUP_DISABLE, GPIO_PULLUP_ENABLE } gpio_pullup_t;
typedef enum { GPIO_PULLDOWN_DISABLE, GPIO_PULLDOWN_ENABLE } gpio_pulldown_t;
typedef enum {
    GPIO_INTR_DISABLE,
    GPIO_INTR_POSEDGE,
    GPIO_INTR_NEGEDGE,
    GPIO_INTR_ANYEDGE,
    GPIO_INTR_LOW_LEVEL,
    GPIO_INTR_HIGH_LEVEL,
    GPIO_INTR_MAX,
} gpio_int_type_t;

typedef struct {
    uint64_t pin_bit_mask;
    gpio_mode_t mode;
    gpio_pullup_t pull_up_en;
    gpio_pulldown_t pull_down_en;
    gpio_int_type_t intr_type;
} gpio_config_t;

typedef void (*gpio_isr_t)(void *argument);

#define GPIO_IS_VALID_GPIO(gpio) ((gpio) >= GPIO_NUM_0 && (gpio) < GPIO_NUM_MAX)
#define GPIO_IS_VALID_OUTPUT_GPIO(gpio) GPIO_IS_VALID_GPIO(gpio)

esp_err_t gpio_config(const gpio_config_t *config);
esp_err_t gpio_reset_pin(gpio_num_t gpio);
esp_err_t gpio_set_direction(gpio_num_t gpio, gpio_mode_t mode);
esp_err_t gpio_set_level(gpio_num_t gpio, uint32_t level);
int gpio_get_level(gpio_num_t gpio);
esp_err_t gpio_set_intr_type(gpio_num_t gpio, gpio_int_type_t type);
esp_err_t gpio_intr_enable(gpio_num_t gpio);
esp_err_t gpio_intr_disable(gpio_num_t gpio);
esp_err_t gpio_install_isr_service(int flags);
esp_err_t gpio_isr_handler_add(gpio_num_t gpio, gpio_isr_t handler, void *argument);
esp_err_t gpio_isr_handler_remove(gpio_num_t gpio);
esp_err_t gpio_wakeup_enable(gpio_num_t gpio, gpio_int_type_t type);
esp_err_t gpio_wakeup_disable(gpio_num_t gpio);

#ifdef __cplusplus
}
#endif
