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

#include "customer_product_info.h"

#include <stdio.h>
#include <string.h>

// Development defaults for boards without product metadata support.
static esp_err_t copy_default(char *text, size_t text_len, size_t min_len,
                              const char *value)
{
    if (text == NULL || text_len < min_len) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(text, text_len, "%s", value);
    return ESP_OK;
}

esp_err_t customer_product_info_get_sn(char *sn, size_t sn_len)
{
    return copy_default(sn, sn_len, CUSTOMER_PRODUCT_INFO_SN_LEN + 1u,
                        "00000000000000");
}

esp_err_t customer_product_info_get_region_text(char *region, size_t region_len)
{
    return copy_default(region, region_len, CUSTOMER_PRODUCT_INFO_TEXT_LEN + 1u,
                        "USA");
}

esp_err_t customer_product_info_get_colour_text(char *colour, size_t colour_len)
{
    return copy_default(colour, colour_len, CUSTOMER_PRODUCT_INFO_TEXT_LEN + 1u,
                        "WHITE");
}

esp_err_t customer_product_info_read(customer_product_info_t *info)
{
    if (info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(info, 0, sizeof(*info));
    return customer_product_info_get_sn(info->sn, sizeof(info->sn));
}
