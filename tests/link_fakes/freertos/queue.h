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

#include <stddef.h>

#include "freertos/FreeRTOS.h"

typedef void *QueueHandle_t;
typedef void *SemaphoreHandle_t;

static inline QueueHandle_t xQueueCreate(unsigned length, unsigned item_size) {
    (void)length;
    (void)item_size;
    return (QueueHandle_t)1;
}

static inline BaseType_t xQueueSend(QueueHandle_t queue, const void *item,
                                    int ticks) {
    (void)queue;
    (void)item;
    (void)ticks;
    return pdTRUE;
}

static inline BaseType_t xQueueReceive(QueueHandle_t queue, void *item,
                                       int ticks) {
    (void)queue;
    (void)item;
    (void)ticks;
    return pdFALSE;
}

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    return (SemaphoreHandle_t)1;
}

static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, int ticks) {
    (void)semaphore;
    (void)ticks;
    return pdTRUE;
}

static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore) {
    (void)semaphore;
    return pdTRUE;
}
