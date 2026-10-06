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
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DIAGNOSTIC_LOG_CAPACITY_BYTES (16 * 1024)

typedef struct {
    char *data;
    size_t len;
    bool truncated;
    uint32_t overwritten_lines;
    uint32_t dropped_lines;
    uint32_t truncated_lines;
} diagnostic_log_snapshot_t;

// Allocates a PSRAM-only ring and mirrors the normal ESP log stream into it.
// The original console sink remains active. Capture stays disabled if PSRAM or
// the mutex is unavailable; there is no internal-RAM, NVS, or flash fallback.
bool diagnostic_log_init(void);

// Clears retained records and counters while keeping capture installed. Safe
// before initialization or when capture is unavailable. Call from task context
// after stopping the old setup's transports; waits for the capture mutex.
void diagnostic_log_clear(void);

// Copies the current oldest-to-newest complete-line tail into PSRAM. The caller
// owns snapshot->data and releases it with diagnostic_log_snapshot_free().
bool diagnostic_log_snapshot(diagnostic_log_snapshot_t *snapshot);
void diagnostic_log_snapshot_free(diagnostic_log_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
