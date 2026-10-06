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

#include "stack_monitor.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void stack_monitor_record(stack_monitor_t *state) {
    uint32_t free_bytes = (uint32_t)(uxTaskGetStackHighWaterMark(NULL)
                                    * sizeof(StackType_t));
    if (state) {
        state->last_sample_ticks = xTaskGetTickCount();
        if (free_bytes >= state->minimum_free_bytes) return;
        state->minimum_free_bytes = free_bytes;
    }
    ESP_LOGI("link.stack", "%s stack high water mark: %u bytes free",
             pcTaskGetName(NULL), (unsigned)free_bytes);
}

void stack_monitor_poll(stack_monitor_t *state) {
    TickType_t now = xTaskGetTickCount();
    if (state->minimum_free_bytes == UINT32_MAX
        || (TickType_t)(now - state->last_sample_ticks) >= pdMS_TO_TICKS(1000)) {
        stack_monitor_record(state);
    }
}
