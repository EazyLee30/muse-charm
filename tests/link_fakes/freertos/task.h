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

#include "freertos/FreeRTOS.h"

typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);

#ifdef LINK_FAKE_CUSTOM_TASKS
BaseType_t xTaskCreate(TaskFunction_t task, const char *name,
                       unsigned stack_depth, void *params,
                       unsigned priority, TaskHandle_t *out_handle);
void vTaskDelete(TaskHandle_t task);
void vTaskDelay(int ticks);
#else
static inline void vTaskDelay(int ticks) {
    (void)ticks;
}

static inline BaseType_t xTaskCreate(TaskFunction_t task,
                                     const char *name,
                                     unsigned stack_depth,
                                     void *params,
                                     unsigned priority,
                                     TaskHandle_t *out_handle) {
    (void)task;
    (void)name;
    (void)stack_depth;
    (void)params;
    (void)priority;
    if (out_handle) *out_handle = (TaskHandle_t)1;
    return pdPASS;
}

static inline void vTaskDelete(TaskHandle_t task) {
    (void)task;
}
#endif
