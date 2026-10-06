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

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "ota.h"
#include "esp_app_desc.h"

static esp_app_desc_t running = { .version = "999.0.0" };
static int callbacks;
static ota_result_t last_result;
static char last_detail[96];

const esp_app_desc_t *esp_app_get_description(void) { return &running; }

static void on_result(const ota_event_t *event, void *user) {
    assert(user == &callbacks);
    callbacks++;
    last_result = event->result;
    snprintf(last_detail, sizeof(last_detail), "%s", event->detail);
    if (event->running_version) {
        assert(strcmp(event->running_version, running.version) == 0);
    }
}

#if CONFIG_HOMEHUB_OTA_ENABLED
#include "esp_https_ota.h"
#include "freertos/task.h"
#include "stack_monitor.h"

void *esp_crt_bundle_attach;
static esp_app_desc_t incoming = { .version = "1.0.1" };
static TaskFunction_t pending_task;
static void *pending_arg;
static int starts, downloads, finishes, aborts, reboots;
static esp_err_t finish_result;

BaseType_t xTaskCreate(TaskFunction_t task, const char *name,
                       unsigned stack_depth, void *params,
                       unsigned priority, TaskHandle_t *out_handle) {
    (void)name; (void)stack_depth; (void)priority; (void)out_handle;
    assert(!pending_task);
    pending_task = task;
    pending_arg = params;
    return pdPASS;
}

void vTaskDelete(TaskHandle_t task) { (void)task; }
void vTaskDelay(int ticks) { (void)ticks; }
void esp_restart(void) { reboots++; }
void stack_monitor_record(stack_monitor_t *state) { (void)state; }
const char *esp_err_to_name(esp_err_t err) { (void)err; return "fake error"; }

esp_err_t esp_https_ota_begin(const esp_https_ota_config_t *config,
                             esp_https_ota_handle_t *handle) {
    assert(strcmp(config->http_config->url, "https://example.com/update.bin") == 0);
    starts++;
    *handle = &incoming;
    return ESP_OK;
}

esp_err_t esp_https_ota_get_img_desc(esp_https_ota_handle_t handle,
                                    esp_app_desc_t *desc) {
    assert(handle == &incoming);
    *desc = incoming;
    return ESP_OK;
}

esp_err_t esp_https_ota_perform(esp_https_ota_handle_t handle) {
    assert(handle == &incoming);
    downloads++;
    return ESP_OK;
}

bool esp_https_ota_is_complete_data_received(esp_https_ota_handle_t handle) {
    assert(handle == &incoming);
    return true;
}

esp_err_t esp_https_ota_finish(esp_https_ota_handle_t handle) {
    assert(handle == &incoming);
    finishes++;
    return finish_result;
}

esp_err_t esp_https_ota_abort(esp_https_ota_handle_t handle) {
    assert(handle == &incoming);
    aborts++;
    return ESP_OK;
}

static void attempt(const char *version, bool force, ota_result_t expected) {
    snprintf(incoming.version, sizeof(incoming.version), "%s", version);
    starts = downloads = finishes = aborts = reboots = callbacks = 0;
    ota_start("https://example.com/update.bin", force, on_result, &callbacks);
    assert(callbacks == 0);
    assert(pending_task);
    TaskFunction_t task = pending_task;
    pending_task = NULL;
    task(pending_arg);
    assert(callbacks == 1);
    assert(last_result == expected);
    assert(starts == 1);
    assert(downloads == (expected != OTA_RESULT_SKIPPED));
    assert(finishes == (expected != OTA_RESULT_SKIPPED));
    assert(aborts == (expected == OTA_RESULT_SKIPPED));
    assert(reboots == (expected == OTA_RESULT_APPLIED));
}
#endif

int main(void) {
#if CONFIG_HOMEHUB_OTA_ENABLED
    assert(ota_is_enabled());
    attempt("1.0.1", false, OTA_RESULT_SKIPPED);
    attempt("999.0.0", false, OTA_RESULT_SKIPPED);
    attempt("1000.0.0", false, OTA_RESULT_APPLIED);
    attempt("1.0.1", true, OTA_RESULT_APPLIED);
    finish_result = ESP_FAIL;
    attempt("1000.0.0", false, OTA_RESULT_FAILED);
#else
    // No network, task or flash fakes are linked in this configuration.
    // A dependency on any of those operations would fail the link.
    assert(!ota_is_enabled());
    const char *urls[] = { "https://example.com/update.bin", "", NULL };
    for (unsigned i = 0; i < sizeof(urls) / sizeof(urls[0]); i++) {
        for (int force = 0; force <= 1; force++) {
            callbacks = 0;
            ota_start(urls[i], force, on_result, &callbacks);
            assert(callbacks == 1);
            assert(last_result == OTA_RESULT_SKIPPED);
            assert(strcmp(last_detail, "OTA disabled in this build") == 0);
            ota_start(urls[i], force, NULL, NULL);
        }
    }
#endif
    puts("OTA harness passed");
    return 0;
}
