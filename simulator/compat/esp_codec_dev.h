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

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_CODEC_DEV_OK 0

typedef struct sim_codec_dev *esp_codec_dev_handle_t;

typedef struct {
    unsigned sample_rate;
    unsigned channel;
    unsigned bits_per_sample;
} esp_codec_dev_sample_info_t;

typedef enum {
    ESP_CODEC_DEV_TYPE_IN,
    ESP_CODEC_DEV_TYPE_OUT,
    ESP_CODEC_DEV_TYPE_IN_OUT,
} esp_codec_dev_type_t;

typedef struct {
    esp_codec_dev_type_t dev_type;
    const void *codec_if;
    const void *data_if;
} esp_codec_dev_cfg_t;

esp_codec_dev_handle_t esp_codec_dev_new(const esp_codec_dev_cfg_t *config);
int esp_codec_dev_delete(esp_codec_dev_handle_t device);
int esp_codec_dev_open(esp_codec_dev_handle_t device, const esp_codec_dev_sample_info_t *info);
int esp_codec_dev_close(esp_codec_dev_handle_t device);
int esp_codec_dev_read(esp_codec_dev_handle_t device, void *data, int size);
int esp_codec_dev_write(esp_codec_dev_handle_t device, const void *data, int size);
int esp_codec_dev_set_in_gain(esp_codec_dev_handle_t device, float gain);
int esp_codec_dev_set_out_vol(esp_codec_dev_handle_t device, int volume);

#ifdef __cplusplus
}
#endif
