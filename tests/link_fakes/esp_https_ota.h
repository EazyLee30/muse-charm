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

#include "esp_app_desc.h"
#include "esp_http_client.h"

#define ESP_ERR_HTTPS_OTA_IN_PROGRESS 1

typedef void *esp_https_ota_handle_t;
typedef struct {
    const esp_http_client_config_t *http_config;
} esp_https_ota_config_t;

esp_err_t esp_https_ota_begin(const esp_https_ota_config_t *config,
                             esp_https_ota_handle_t *handle);
esp_err_t esp_https_ota_get_img_desc(esp_https_ota_handle_t handle,
                                    esp_app_desc_t *desc);
esp_err_t esp_https_ota_perform(esp_https_ota_handle_t handle);
bool esp_https_ota_is_complete_data_received(esp_https_ota_handle_t handle);
esp_err_t esp_https_ota_finish(esp_https_ota_handle_t handle);
esp_err_t esp_https_ota_abort(esp_https_ota_handle_t handle);
