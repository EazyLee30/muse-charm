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

#include <stdarg.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_LOG_NONE,
    ESP_LOG_ERROR,
    ESP_LOG_WARN,
    ESP_LOG_INFO,
    ESP_LOG_DEBUG,
    ESP_LOG_VERBOSE,
    ESP_LOG_MAX,
} esp_log_level_t;

void esp_log_write(esp_log_level_t level, const char *tag, const char *format, ...)
    __attribute__((format(printf, 3, 4)));
void esp_log_writev(esp_log_level_t level, const char *tag, const char *format, va_list args);
void esp_log_level_set(const char *tag, esp_log_level_t level);
esp_log_level_t esp_log_level_get(const char *tag);
uint32_t esp_log_timestamp(void);

#define ESP_LOGE(tag, ...) esp_log_write(ESP_LOG_ERROR, tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) esp_log_write(ESP_LOG_WARN, tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) esp_log_write(ESP_LOG_INFO, tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) esp_log_write(ESP_LOG_DEBUG, tag, __VA_ARGS__)
#define ESP_LOGV(tag, ...) esp_log_write(ESP_LOG_VERBOSE, tag, __VA_ARGS__)

#ifdef __cplusplus
}
#endif
