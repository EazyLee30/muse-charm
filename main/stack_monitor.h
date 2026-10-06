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

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t minimum_free_bytes;
    uint32_t last_sample_ticks;
} stack_monitor_t;

#define STACK_MONITOR_INIT { UINT32_MAX, 0 }

// State belongs to the current task. NULL logs once, for task exit/reboot paths.
void stack_monitor_record(stack_monitor_t *state);
// Busy loops scan at most once per second; record() samples callbacks immediately.
void stack_monitor_poll(stack_monitor_t *state);

#ifdef __cplusplus
}
#endif
