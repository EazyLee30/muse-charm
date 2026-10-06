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

#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diagnostic_log.h"
#include "esp_heap_caps.h"
#include "esp_log_write.h"
#include "freertos/semphr.h"

#define CONSOLE_BYTES (128 * 1024)

static bool g_fail_heap;
static bool g_fail_mutex;
static bool g_force_writer_contention;
static bool g_clear_during_console;
static int g_heap_calls;
static int g_last_caps;
static size_t g_last_size;
static int g_lock_depth;
static int g_writer_take_count;
static int g_snapshot_take_count;
static int g_give_count;
static int g_mutex_token;
static int g_install_count;
static bool g_reenter_install;
static bool g_nested_init_result;
static unsigned char *g_ring_storage;
static char g_console[CONSOLE_BYTES];
static size_t g_console_len;

static int console_vprintf(const char *format, va_list args);
static vprintf_like_t g_current_sink = console_vprintf;

static void fail_at(const char *file, int line, const char *fmt, ...) {
    va_list args;
    fprintf(stderr, "%s:%d: ", file, line);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    exit(1);
}

#define CHECK(condition, ...) \
    do { if (!(condition)) fail_at(__FILE__, __LINE__, __VA_ARGS__); } while (0)

void *heap_caps_malloc(size_t size, int caps) {
    g_heap_calls++;
    g_last_caps = caps;
    g_last_size = size;
    if (g_fail_heap) return NULL;
    void *allocation = malloc(size);
    if (allocation && size == DIAGNOSTIC_LOG_CAPACITY_BYTES) {
        g_ring_storage = allocation;
        memset(g_ring_storage, 0xa5, size);
    }
    return allocation;
}

size_t heap_caps_get_free_size(int caps) { (void)caps; return 0; }
size_t heap_caps_get_minimum_free_size(int caps) { (void)caps; return 0; }
size_t heap_caps_get_largest_free_block(int caps) { (void)caps; return 0; }

SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    return g_fail_mutex ? NULL : &g_mutex_token;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t wait_ticks) {
    CHECK(semaphore == &g_mutex_token, "unexpected mutex");
    if (wait_ticks == 0) {
        g_writer_take_count++;
        if (g_force_writer_contention) return pdFALSE;
    } else {
        CHECK(wait_ticks == portMAX_DELAY, "unexpected blocking timeout");
        g_snapshot_take_count++;
    }
    CHECK(g_lock_depth == 0, "recursive lock");
    g_lock_depth++;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore) {
    CHECK(semaphore == &g_mutex_token, "unexpected mutex");
    CHECK(g_lock_depth == 1, "unlock without lock");
    g_lock_depth--;
    g_give_count++;
    return pdTRUE;
}

static int console_vprintf(const char *format, va_list args) {
    size_t available = CONSOLE_BYTES - g_console_len;
    int written = vsnprintf(g_console + g_console_len, available, format, args);
    if (written > 0) {
        size_t stored = (size_t)written < available ? (size_t)written
                                                    : available - 1;
        g_console_len += stored;
    }
    if (g_clear_during_console) {
        g_clear_during_console = false;
        diagnostic_log_clear();
    }
    return written;
}

static int call_sink(vprintf_like_t sink, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int result = sink(format, args);
    va_end(args);
    return result;
}

vprintf_like_t esp_log_set_vprintf(vprintf_like_t sink) {
    vprintf_like_t previous = g_current_sink;
    g_current_sink = sink;
    g_install_count++;
    if (g_reenter_install) {
        g_reenter_install = false;
        g_nested_init_result = diagnostic_log_init();
        call_sink(sink,
                  "E (0) HTTP_CLIENT: Error parse url https://example/?token=secret\n");
    }
    return previous;
}

static int emit_log(const char *format, ...) {
    va_list args;
    va_start(args, format);
    int result = g_current_sink(format, args);
    va_end(args);
    return result;
}

static void clear_console(void) {
    memset(g_console, 0, sizeof(g_console));
    g_console_len = 0;
}

static diagnostic_log_snapshot_t take_snapshot(void) {
    diagnostic_log_snapshot_t snapshot = {0};
    CHECK(diagnostic_log_snapshot(&snapshot), "snapshot failed");
    CHECK(snapshot.data != NULL, "snapshot did not own a buffer");
    CHECK(snapshot.data[snapshot.len] == '\0', "snapshot is not terminated");
    return snapshot;
}

static void check_uninitialized_clear(void) {
    int heap_calls = g_heap_calls;
    diagnostic_log_clear();
    CHECK(g_heap_calls == heap_calls, "clear allocated before initialization");
    CHECK(g_install_count == 0, "clear installed the log sink");
    CHECK(g_writer_take_count == 0 && g_snapshot_take_count == 0,
          "clear tried to lock before initialization");
    diagnostic_log_snapshot_t snapshot = {0};
    CHECK(!diagnostic_log_snapshot(&snapshot),
          "clear enabled capture before initialization");
}

static void test_init_failures_and_install_once(void) {
    check_uninitialized_clear();
    g_fail_heap = true;
    CHECK(!diagnostic_log_init(), "initialization should fail without PSRAM");
    CHECK(g_install_count == 0, "failed init replaced the console sink");
    CHECK(g_last_size == DIAGNOSTIC_LOG_CAPACITY_BYTES,
          "ring allocation size mismatch: %zu", g_last_size);
    CHECK(g_last_caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
          "ring must request PSRAM-only capabilities: %d", g_last_caps);
    check_uninitialized_clear();

    g_fail_heap = false;
    g_fail_mutex = true;
    CHECK(!diagnostic_log_init(), "initialization should fail without mutex");
    CHECK(g_install_count == 0, "mutex failure replaced the console sink");
    check_uninitialized_clear();

    g_fail_mutex = false;
    g_reenter_install = true;
    CHECK(diagnostic_log_init(), "PSRAM initialization failed");
    CHECK(g_install_count == 1, "log sink was not installed once");
    CHECK(!g_nested_init_result, "concurrent init bypassed single-owner guard");
    CHECK(diagnostic_log_init(), "idempotent initialization failed");
    CHECK(g_install_count == 1, "second init chained the hook to itself");
}

static void test_console_tee_and_normalized_capture(void) {
    clear_console();
    const char *expected = "I (7) link.app: value=alpha\n";
    int result = emit_log("I (%d) link.app: value=%s\n", 7, "alpha");
    CHECK(result == (int)strlen(expected), "previous sink result changed");
    CHECK(strcmp(g_console, expected) == 0,
          "console output changed: %s", g_console);

    diagnostic_log_snapshot_t snapshot = take_snapshot();
    CHECK(strcmp(snapshot.data, expected) == 0,
          "captured formatting changed: %s", snapshot.data);
    CHECK(!snapshot.truncated, "small log should not be truncated");
    diagnostic_log_snapshot_free(&snapshot);
    CHECK(snapshot.data == NULL && snapshot.len == 0,
          "snapshot free did not clear ownership");

    emit_log("W (8) link.app: remote=%s\n", "a\r\nb\x01");
    emit_log("I (9) link.heartbeat: hb t=9s\n");
    emit_log("E (9) HTTP_CLIENT: Error parse url https://example/?token=secret\n");
    snapshot = take_snapshot();
    CHECK(strstr(snapshot.data, "W (8) link.app: remote=a  b.\n") != NULL,
          "control bytes were not normalized: %s", snapshot.data);
    CHECK(strstr(snapshot.data, "heartbeat") == NULL,
          "high-volume heartbeat entered support history");
    CHECK(strstr(g_console, "heartbeat: hb t=9s") != NULL,
          "filtered heartbeat did not reach console");
    CHECK(strstr(snapshot.data, "token=secret") == NULL,
          "framework URL leaked into support history");
    CHECK(strstr(g_console, "token=secret") != NULL,
          "excluded framework line did not reach console");
    diagnostic_log_snapshot_free(&snapshot);
}

static void test_nonblocking_drop_and_snapshot_allocation_failure(void) {
    g_force_writer_contention = true;
    emit_log("E (10) link.app: dropped capture still reaches console\n");
    g_force_writer_contention = false;

    diagnostic_log_snapshot_t snapshot = take_snapshot();
    CHECK(snapshot.dropped_lines == 1 && snapshot.truncated,
          "contention drop was not reported");
    CHECK(strstr(snapshot.data, "dropped capture") == NULL,
          "contended record unexpectedly entered the ring");
    CHECK(strstr(g_console, "dropped capture still reaches console") != NULL,
          "contention blocked normal logging");
    diagnostic_log_snapshot_free(&snapshot);

    g_fail_heap = true;
    CHECK(!diagnostic_log_snapshot(&snapshot),
          "snapshot should fail without PSRAM");
    g_fail_heap = false;
}

static void test_whole_line_wrap_and_long_line_marker(void) {
    char payload[240];
    memset(payload, 'x', sizeof(payload) - 1);
    payload[sizeof(payload) - 1] = '\0';
    for (int i = 0; i < 100; i++) {
        emit_log("I (%d) link.app: wrap-%03d %s\n", i + 100, i, payload);
    }
    emit_log("I (999) link.app: newest-complete-line\n");

    diagnostic_log_snapshot_t snapshot = take_snapshot();
    CHECK(snapshot.len <= DIAGNOSTIC_LOG_CAPACITY_BYTES,
          "wrapped snapshot exceeded capacity: %zu", snapshot.len);
    CHECK(snapshot.overwritten_lines > 0 && snapshot.truncated,
          "whole-record eviction was not reported");
    CHECK(strstr(snapshot.data, "wrap-000") == NULL,
          "oldest overwritten record survived");
    CHECK(strstr(snapshot.data, "newest-complete-line\n") != NULL,
          "latest record missing");
    const char *cursor = snapshot.data;
    int previous = -1;
    int seen = 0;
    while ((cursor = strstr(cursor, "wrap-")) != NULL) {
        int current = -1;
        CHECK(sscanf(cursor, "wrap-%d", &current) == 1,
              "could not parse wrapped record");
        CHECK(current > previous,
              "wrapped records are out of order: %d after %d",
              current, previous);
        previous = current;
        seen++;
        cursor += 5;
    }
    CHECK(seen > 0, "wrapped snapshot retained no numbered records");
    CHECK(snapshot.len == 0 || snapshot.data[snapshot.len - 1] == '\n',
          "snapshot ended with a partial record");
    diagnostic_log_snapshot_free(&snapshot);

    char long_value[900];
    memset(long_value, 'L', sizeof(long_value) - 1);
    long_value[sizeof(long_value) - 1] = '\0';
    emit_log("W (1000) link.app: %s\n", long_value);
    snapshot = take_snapshot();
    CHECK(strstr(snapshot.data, " [line truncated]\n") != NULL,
          "long-record marker missing");
    CHECK(snapshot.truncated_lines == 1 && snapshot.truncated,
          "long-record truncation was not reported");
    diagnostic_log_snapshot_free(&snapshot);

    CHECK(g_lock_depth == 0, "mutex left locked");
    CHECK(g_writer_take_count > 0 && g_snapshot_take_count > 0,
          "writer/snapshot lock modes were not exercised");
    CHECK(g_give_count == g_writer_take_count - 1 + g_snapshot_take_count,
          "unexpected mutex balance: give=%d writer=%d snapshot=%d",
          g_give_count, g_writer_take_count, g_snapshot_take_count);
}

static void check_empty_snapshot(void) {
    diagnostic_log_snapshot_t snapshot = take_snapshot();
    CHECK(snapshot.len == 0 && snapshot.data[0] == '\0',
          "cleared snapshot retained log data");
    CHECK(!snapshot.truncated && snapshot.overwritten_lines == 0
          && snapshot.dropped_lines == 0 && snapshot.truncated_lines == 0,
          "cleared snapshot retained log counters");
    diagnostic_log_snapshot_free(&snapshot);
}

static void test_clear_resets_history_and_keeps_capture_active(void) {
    diagnostic_log_snapshot_t snapshot = take_snapshot();
    CHECK(snapshot.len > 0 && snapshot.overwritten_lines > 0
          && snapshot.dropped_lines > 0 && snapshot.truncated_lines > 0,
          "clear test must start with wrapped, dropped and truncated history");
    diagnostic_log_snapshot_free(&snapshot);

    int heap_calls = g_heap_calls;
    vprintf_like_t sink = g_current_sink;
    unsigned char *ring_storage = g_ring_storage;
    g_fail_heap = true;
    diagnostic_log_clear();
    diagnostic_log_clear();
    g_fail_heap = false;
    CHECK(g_heap_calls == heap_calls, "clear tried to allocate memory");
    CHECK(g_install_count == 1 && g_current_sink == sink,
          "clear reinstalled or replaced the console hook");
    CHECK(g_ring_storage == ring_storage, "clear replaced the ring allocation");
    CHECK(g_lock_depth == 0, "clear left the ring mutex locked");
    for (size_t i = 0; i < DIAGNOSTIC_LOG_CAPACITY_BYTES; i++) {
        CHECK(g_ring_storage[i] == 0, "clear retained ring byte at %zu", i);
    }
    check_empty_snapshot();

    clear_console();
    const char *expected = "I (1001) link.app: capture after clear\n";
    CHECK(emit_log("%s", expected) == (int)strlen(expected),
          "clear changed the console result");
    CHECK(strcmp(g_console, expected) == 0, "console tee stopped after clear");
    snapshot = take_snapshot();
    CHECK(strcmp(snapshot.data, expected) == 0 && !snapshot.truncated,
          "capture did not restart with fresh history: %s", snapshot.data);
    diagnostic_log_snapshot_free(&snapshot);
}

static void test_clear_discards_in_flight_capture(void) {
    clear_console();
    char long_value[900];
    memset(long_value, 'L', sizeof(long_value) - 1);
    long_value[sizeof(long_value) - 1] = '\0';
    g_clear_during_console = true;
    int result = emit_log("W (1002) link.app: in-flight %s\n", long_value);
    CHECK(result > 0 && (size_t)result == g_console_len,
          "in-flight clear changed the console result");
    CHECK(strstr(g_console, "in-flight ") != NULL,
          "in-flight clear interrupted console output");
    CHECK(!g_clear_during_console, "console callback did not clear history");
    check_empty_snapshot();

    const char *expected = "I (1003) link.app: fresh capture\n";
    emit_log("%s", expected);
    diagnostic_log_snapshot_t snapshot = take_snapshot();
    CHECK(strcmp(snapshot.data, expected) == 0 && !snapshot.truncated,
          "new capture failed after discarding an in-flight record: %s",
          snapshot.data);
    diagnostic_log_snapshot_free(&snapshot);
}

int main(void) {
    test_init_failures_and_install_once();
    test_console_tee_and_normalized_capture();
    test_nonblocking_drop_and_snapshot_allocation_failure();
    test_whole_line_wrap_and_long_line_marker();
    test_clear_resets_history_and_keeps_capture_active();
    test_clear_discards_in_flight_capture();
    return 0;
}
