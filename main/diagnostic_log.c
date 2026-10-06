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

#include "diagnostic_log.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log_write.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define DIAGNOSTIC_LOG_RECORD_BYTES 512
#define DIAGNOSTIC_LOG_TRUNCATION_MARKER " [line truncated]"

static char *s_ring = NULL;
static size_t s_head = 0;
static size_t s_len = 0;
static uint32_t s_overwritten_lines = 0;
static uint32_t s_truncated_lines = 0;
static SemaphoreHandle_t s_lock = NULL;
static _Atomic(vprintf_like_t) s_previous_vprintf = ATOMIC_VAR_INIT(vprintf);
static atomic_bool s_installed = ATOMIC_VAR_INIT(false);
static atomic_flag s_initializing = ATOMIC_FLAG_INIT;
static atomic_uint_fast32_t s_dropped_lines = ATOMIC_VAR_INIT(0);
static atomic_uint_fast32_t s_generation = ATOMIC_VAR_INIT(0);

static bool should_capture_record(const char *record) {
    const char *tag = record ? strchr(record, ')') : NULL;
    if (!tag) return false;
    tag++;
    if (*tag == ' ') tag++;
    return strncmp(tag, "link.", 5) == 0
           && strncmp(tag, "link.heartbeat:", 15) != 0;
}

static size_t normalize_record(char record[DIAGNOSTIC_LOG_RECORD_BYTES],
                               int formatted_len, bool *truncated) {
    if (truncated) *truncated = false;
    if (formatted_len <= 0) return 0;

    size_t len = (size_t)formatted_len;
    bool was_truncated = len >= DIAGNOSTIC_LOG_RECORD_BYTES;
    if (was_truncated) len = DIAGNOSTIC_LOG_RECORD_BYTES - 1;

    while (len > 0 && (record[len - 1] == '\n' || record[len - 1] == '\r')) {
        len--;
    }
    if (len > DIAGNOSTIC_LOG_RECORD_BYTES - 2) {
        len = DIAGNOSTIC_LOG_RECORD_BYTES - 2;
        was_truncated = true;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)record[i];
        if (c == '\r' || c == '\n') {
            record[i] = ' ';
        } else if ((c < 0x20 && c != '\t') || c == 0x7f) {
            record[i] = '.';
        }
    }

    if (was_truncated) {
        static const char marker[] = DIAGNOSTIC_LOG_TRUNCATION_MARKER;
        const size_t marker_len = sizeof(marker) - 1;
        const size_t max_prefix = DIAGNOSTIC_LOG_RECORD_BYTES - marker_len - 2;
        if (len > max_prefix) len = max_prefix;
        memcpy(record + len, marker, marker_len);
        len += marker_len;
    }
    record[len++] = '\n';
    record[len] = '\0';
    if (truncated) *truncated = was_truncated;
    return len;
}

static void evict_oldest_record_locked(void) {
    if (s_len == 0) return;
    char c;
    do {
        c = s_ring[s_head];
        s_head = (s_head + 1) % DIAGNOSTIC_LOG_CAPACITY_BYTES;
        s_len--;
    } while (s_len > 0 && c != '\n');
    s_overwritten_lines++;
}

static void append_record_locked(const char *record, size_t len) {
    if (!record || len == 0 || len > DIAGNOSTIC_LOG_CAPACITY_BYTES) return;
    while (DIAGNOSTIC_LOG_CAPACITY_BYTES - s_len < len) {
        evict_oldest_record_locked();
    }

    size_t tail = (s_head + s_len) % DIAGNOSTIC_LOG_CAPACITY_BYTES;
    size_t first = DIAGNOSTIC_LOG_CAPACITY_BYTES - tail;
    if (first > len) first = len;
    memcpy(s_ring + tail, record, first);
    if (first < len) memcpy(s_ring, record + first, len - first);
    s_len += len;
}

static int diagnostic_log_vprintf(const char *format, va_list args) {
    uint_fast32_t generation = atomic_load_explicit(
        &s_generation, memory_order_acquire);
    va_list output_args;
    va_list capture_args;
    va_copy(output_args, args);
    va_copy(capture_args, args);

    vprintf_like_t output = atomic_load_explicit(
        &s_previous_vprintf, memory_order_acquire);
    if (!output) output = vprintf;
    int output_result = output(format, output_args);
    va_end(output_args);

    char record[DIAGNOSTIC_LOG_RECORD_BYTES];
    int formatted_len = vsnprintf(record, sizeof(record), format, capture_args);
    va_end(capture_args);
    bool line_truncated = false;
    size_t record_len = normalize_record(
        record, formatted_len, &line_truncated);
    if (record_len == 0 || !should_capture_record(record)) return output_result;

    if (!s_lock || xSemaphoreTake(s_lock, 0) != pdTRUE) {
        atomic_fetch_add_explicit(
            &s_dropped_lines, 1, memory_order_relaxed);
        return output_result;
    }
    // A callback can be delayed in the console sink while setup is reset.
    // Only append records that began in the current capture generation.
    if (generation == atomic_load_explicit(&s_generation, memory_order_relaxed)) {
        if (line_truncated) s_truncated_lines++;
        append_record_locked(record, record_len);
    }
    xSemaphoreGive(s_lock);
    return output_result;
}

bool diagnostic_log_init(void) {
    if (atomic_load_explicit(&s_installed, memory_order_acquire)) return true;
    if (atomic_flag_test_and_set_explicit(
            &s_initializing, memory_order_acquire)) {
        return false;
    }

    char *ring = heap_caps_malloc(DIAGNOSTIC_LOG_CAPACITY_BYTES,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ring) {
        atomic_flag_clear_explicit(&s_initializing, memory_order_release);
        return false;
    }
    SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    if (!lock) {
        free(ring);
        atomic_flag_clear_explicit(&s_initializing, memory_order_release);
        return false;
    }

    s_ring = ring;
    s_head = 0;
    s_len = 0;
    s_overwritten_lines = 0;
    s_truncated_lines = 0;
    s_lock = lock;
    atomic_store_explicit(&s_dropped_lines, 0, memory_order_relaxed);

    // Keep a valid fallback during the atomic sink swap in case another task
    // logs before esp_log_set_vprintf() returns the previous sink.
    atomic_store_explicit(
        &s_previous_vprintf, vprintf, memory_order_release);
    vprintf_like_t previous = esp_log_set_vprintf(diagnostic_log_vprintf);
    if (previous) {
        atomic_store_explicit(
            &s_previous_vprintf, previous, memory_order_release);
    }
    atomic_store_explicit(&s_installed, true, memory_order_release);
    atomic_flag_clear_explicit(&s_initializing, memory_order_release);
    return true;
}

void diagnostic_log_clear(void) {
    if (!atomic_load_explicit(&s_installed, memory_order_acquire)) return;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return;

    atomic_fetch_add_explicit(&s_generation, 1, memory_order_release);
    memset(s_ring, 0, DIAGNOSTIC_LOG_CAPACITY_BYTES);
    s_head = 0;
    s_len = 0;
    s_overwritten_lines = 0;
    s_truncated_lines = 0;
    atomic_store_explicit(&s_dropped_lines, 0, memory_order_relaxed);
    xSemaphoreGive(s_lock);
}

bool diagnostic_log_snapshot(diagnostic_log_snapshot_t *snapshot) {
    if (!snapshot) return false;
    memset(snapshot, 0, sizeof(*snapshot));
    if (!atomic_load_explicit(&s_installed, memory_order_acquire)
        || !s_ring || !s_lock) {
        return false;
    }

    // Allocate before taking the writer lock: allocators may themselves log,
    // and the capture callback must never recursively wait on this mutex.
    char *copy = heap_caps_malloc(DIAGNOSTIC_LOG_CAPACITY_BYTES + 1,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) return false;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        free(copy);
        return false;
    }

    size_t first = DIAGNOSTIC_LOG_CAPACITY_BYTES - s_head;
    if (first > s_len) first = s_len;
    memcpy(copy, s_ring + s_head, first);
    if (first < s_len) memcpy(copy + first, s_ring, s_len - first);
    copy[s_len] = '\0';

    snapshot->data = copy;
    snapshot->len = s_len;
    snapshot->overwritten_lines = s_overwritten_lines;
    snapshot->dropped_lines = (uint32_t)atomic_load_explicit(
        &s_dropped_lines, memory_order_relaxed);
    snapshot->truncated_lines = s_truncated_lines;
    snapshot->truncated = snapshot->overwritten_lines > 0
                          || snapshot->dropped_lines > 0
                          || snapshot->truncated_lines > 0;
    xSemaphoreGive(s_lock);
    return true;
}

void diagnostic_log_snapshot_free(diagnostic_log_snapshot_t *snapshot) {
    if (!snapshot) return;
    free(snapshot->data);
    memset(snapshot, 0, sizeof(*snapshot));
}
