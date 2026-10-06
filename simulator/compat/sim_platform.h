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

#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Real monotonic time is the default. Setting or advancing time selects the
 * deterministic clock until sim_time_use_realtime(true) is called. */
void sim_time_use_realtime(bool realtime);
bool sim_time_is_realtime(void);
void sim_time_reset(void);
void sim_time_set_us(int64_t time_us);
void sim_time_advance_us(int64_t delta_us);
uint32_t sim_time_tick_ms(void);
void sim_delay_ms(uint32_t milliseconds);

/* Drives a simulated GPIO input and runs its registered ISR on matching edges. */
void sim_gpio_set_input_level(gpio_num_t gpio, int level);

#ifdef __cplusplus
}
#endif
