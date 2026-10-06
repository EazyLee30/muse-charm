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

#include "camera.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

static const camera_driver_t *s_driver;

/* Held from capture to release, or from stream start to stop. Only its holder
 * starts the backend, so that needs no lock of its own. */
static atomic_flag s_busy = ATOMIC_FLAG_INIT;
static bool s_started;
static bool s_streaming;

void camera_register(const camera_driver_t *driver)
{
    s_driver = driver;
    s_started = false;
}

const camera_driver_t *camera_get(void)
{
    return s_driver;
}

/* With s_busy held: the backend's one-time setup, again after a failed start. */
static esp_err_t start(const camera_driver_t *d)
{
    esp_err_t err = !s_started && d->init ? d->init() : ESP_OK;
    s_started = err == ESP_OK;
    return err;
}

esp_err_t camera_capture(camera_frame_t *out)
{
    memset(out, 0, sizeof(*out));
    const camera_driver_t *d = s_driver;
    if (!d) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (atomic_flag_test_and_set(&s_busy)) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = start(d);
    if (err == ESP_OK) {
        err = d->capture(out);
    }
    if (err != ESP_OK) {
        memset(out, 0, sizeof(*out));
        atomic_flag_clear(&s_busy);
    }
    return err;
}

void camera_release(camera_frame_t *frame)
{
    if (!frame->jpeg) {
        return;
    }
    if (s_driver && s_driver->release) {
        s_driver->release(frame);
    }
    memset(frame, 0, sizeof(*frame));
    atomic_flag_clear(&s_busy);
}

esp_err_t camera_stream_start(camera_frame_cb_t on_frame, void *ctx)
{
    const camera_driver_t *d = s_driver;
    if (!d || !d->stream_start || !d->stream_stop) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (atomic_flag_test_and_set(&s_busy)) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = start(d);
    if (err == ESP_OK) {
        err = d->stream_start(on_frame, ctx);
    }
    s_streaming = err == ESP_OK;
    if (err != ESP_OK) {
        atomic_flag_clear(&s_busy);
    }
    return err;
}

void camera_stream_stop(void)
{
    if (!s_streaming) {
        return;
    }
    s_driver->stream_stop();
    s_streaming = false;
    atomic_flag_clear(&s_busy);
}
