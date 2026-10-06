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

typedef int esp_efuse_block_t;
typedef int esp_efuse_purpose_t;

#define EFUSE_BLK_KEY0 4
#define ESP_EFUSE_KEY_PURPOSE_HMAC_UP 8

static inline esp_efuse_purpose_t esp_efuse_get_key_purpose(esp_efuse_block_t block) {
    (void)block;
    return 0;
}

static inline int esp_efuse_read_field_bit(const void *field[]) {
    (void)field;
    return 0;
}
