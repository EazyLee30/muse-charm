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

/* components/camera for test_camera.py: the registry's one-holder rule for
 * frames and streams, and backend start-up, against a scripted backend. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "camera.h"

static int inits, captures, releases, stream_starts, stream_stops;
static esp_err_t init_err, capture_err, stream_err;
static const uint8_t jpeg[] = { 0xFF, 0xD8, 0x00, 0xFF, 0xD9 };

static esp_err_t scripted_init(void)
{
    inits++;
    return init_err;
}

static esp_err_t scripted_capture(camera_frame_t *out)
{
    captures++;
    if (capture_err) {
        out->jpeg = jpeg;   /* partly filled on failure: the registry clears it */
        return capture_err;
    }
    out->jpeg = jpeg;
    out->len = sizeof(jpeg);
    out->width = 4;
    out->height = 3;
    return ESP_OK;
}

static void scripted_release(camera_frame_t *frame)
{
    assert(frame->jpeg == jpeg);
    releases++;
}

static camera_frame_cb_t streaming_to;

static esp_err_t scripted_stream_start(camera_frame_cb_t on_frame, void *ctx)
{
    (void)ctx;
    stream_starts++;
    if (!stream_err) {
        streaming_to = on_frame;
    }
    return stream_err;
}

static void scripted_stream_stop(void)
{
    stream_stops++;
    streaming_to = NULL;
}

static const camera_driver_t scripted = {
    .name = "scripted",
    .max_width = 4,
    .max_height = 3,
    .init = scripted_init,
    .capture = scripted_capture,
    .release = scripted_release,
    .stream_start = scripted_stream_start,
    .stream_stop = scripted_stream_stop,
};

static const camera_driver_t stills_only = {
    .name = "stills only",
    .capture = scripted_capture,
};

static int frames_seen;

static void count_frame(const camera_frame_t *frame, void *ctx)
{
    assert(frame && ctx == &frames_seen);
    frames_seen++;
}

static void no_camera(void)
{
    camera_frame_t f;
    assert(!camera_get());
    assert(camera_capture(&f) == ESP_ERR_NOT_SUPPORTED && !f.jpeg);
    camera_release(&f);   /* nothing out: harmless */
    assert(camera_stream_start(count_frame, &frames_seen) == ESP_ERR_NOT_SUPPORTED);
    camera_stream_stop();   /* nothing streaming: harmless */
}

static void registered_camera(void)
{
    camera_register(&scripted);
    assert(camera_get() == &scripted);
    camera_frame_t f, other;

    /* A failed start isn't remembered: the next capture tries again. */
    init_err = ESP_FAIL;
    assert(camera_capture(&f) == ESP_FAIL && !f.jpeg && captures == 0);
    init_err = ESP_OK;
    assert(camera_capture(&f) == ESP_OK && inits == 2 && f.len == sizeof(jpeg));

    /* One frame out at a time. */
    assert(camera_capture(&other) == ESP_ERR_INVALID_STATE && !other.jpeg);
    camera_release(&f);
    assert(releases == 1 && !f.jpeg);

    /* Started once; a failed capture frees the camera and clears the frame. */
    capture_err = ESP_ERR_TIMEOUT;
    assert(camera_capture(&f) == ESP_ERR_TIMEOUT && !f.jpeg && inits == 2);
    capture_err = ESP_OK;
    assert(camera_capture(&f) == ESP_OK && inits == 2);
    camera_release(&f);
    camera_release(&f);   /* twice: the second is a no-op */
    assert(releases == 2);

    /* A newly registered camera is started afresh. */
    camera_register(&scripted);
    assert(camera_capture(&f) == ESP_OK && inits == 3);
    camera_release(&f);
}

static void streams(void)
{
    camera_register(&scripted);
    camera_frame_t f;

    /* A stream holds the camera like a frame: no capture or second stream meanwhile. */
    assert(camera_stream_start(count_frame, &frames_seen) == ESP_OK && stream_starts == 1);
    camera_frame_t frame = { .jpeg = jpeg, .len = sizeof(jpeg) };
    streaming_to(&frame, &frames_seen);
    assert(frames_seen == 1);
    assert(camera_capture(&f) == ESP_ERR_INVALID_STATE && !f.jpeg);
    assert(camera_stream_start(count_frame, &frames_seen) == ESP_ERR_INVALID_STATE && stream_starts == 1);
    camera_stream_stop();
    assert(stream_stops == 1 && !streaming_to);
    camera_stream_stop();   /* twice: the second is a no-op */
    assert(stream_stops == 1);

    /* Stopped, the camera is free again. */
    assert(camera_capture(&f) == ESP_OK);
    camera_release(&f);

    /* A stream that fails to start frees the camera. */
    stream_err = ESP_ERR_TIMEOUT;
    assert(camera_stream_start(count_frame, &frames_seen) == ESP_ERR_TIMEOUT);
    camera_stream_stop();
    assert(stream_stops == 1);
    stream_err = ESP_OK;
    assert(camera_capture(&f) == ESP_OK);
    camera_release(&f);

    /* A backend that can't stream says so, and keeps taking stills. */
    camera_register(&stills_only);
    assert(camera_stream_start(count_frame, &frames_seen) == ESP_ERR_NOT_SUPPORTED);
    assert(camera_capture(&f) == ESP_OK);
    camera_release(&f);
}

int main(void)
{
    no_camera();
    registered_camera();
    streams();
    puts("ok");
    return 0;
}
