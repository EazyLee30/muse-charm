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

#include <assert.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;

#define pdFALSE 0
#define pdTRUE 1
#define pdFAIL 0
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define portTICK_PERIOD_MS 1u
#define pdMS_TO_TICKS(milliseconds) ((TickType_t)(milliseconds))
#define pdTICKS_TO_MS(ticks) ((uint32_t)(ticks))

#define BIT0 (1u << 0)
#define BIT1 (1u << 1)
#define BIT2 (1u << 2)
#define BIT3 (1u << 3)
#define BIT4 (1u << 4)
#define BIT5 (1u << 5)
#define BIT6 (1u << 6)
#define BIT7 (1u << 7)
#define BIT8 (1u << 8)
#define BIT9 (1u << 9)
#define BIT(n) (1u << (n))
#define BIT64(n) (UINT64_C(1) << (n))

typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(mux) ((void)pthread_mutex_lock(mux))
#define portEXIT_CRITICAL(mux) ((void)pthread_mutex_unlock(mux))
#define portENTER_CRITICAL_SAFE(mux) portENTER_CRITICAL(mux)
#define portEXIT_CRITICAL_SAFE(mux) portEXIT_CRITICAL(mux)
#define portENTER_CRITICAL_ISR(mux) portENTER_CRITICAL(mux)
#define portEXIT_CRITICAL_ISR(mux) portEXIT_CRITICAL(mux)
#define portYIELD_FROM_ISR(...) ((void)0)

#ifdef __cplusplus
}
#endif
