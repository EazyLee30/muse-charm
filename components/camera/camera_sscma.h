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

#include "camera.h"
#include "driver/spi_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A camera that speaks Seeed's SSCMA over SPI: the SenseCAP Watcher's Himax
 * WiseEye2, or a Grove Vision AI V2. The chip is powered only for a capture
 * or a stream: power, reset, wait for it to boot, pick the resolution, sample,
 * power off. Between them nothing runs: no task, no polling. The board
 * supplies the lines, wherever they are (GPIOs or an I/O expander).
 */

typedef struct {
    const char *name;               /* camera_driver_t.name */
    spi_host_device_t host;         /* its own bus, or one it shares with only idle devices */
    int sclk, mosi, miso, cs;       /* GPIOs */
    esp_err_t (*power)(bool on);
    esp_err_t (*reset)(bool hold);  /* true holds the chip in reset; NULL: power-up resets it */
    bool (*has_data)(void);         /* the chip's sync line: a reply is waiting */
    /* AT+SENSOR options: 0 = 240x240, 1 = 416x416, 2 = 480x480, 3 = 640x480 */
    int resolution;                 /* stills */
    int stream_resolution;          /* streamed frames, to keep up */
} camera_sscma_config_t;

/* The backend for `config` (copied), to pass to camera_register(). One per device. */
const camera_driver_t *camera_sscma(const camera_sscma_config_t *config);

#ifdef __cplusplus
}
#endif
