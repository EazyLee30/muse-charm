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

#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
#define MALLOC_CAP_DMA 8

#ifdef LINK_FAKE_CUSTOM_HEAP_CAPS
void *heap_caps_malloc(size_t size, int caps);
size_t heap_caps_get_free_size(int caps);
size_t heap_caps_get_minimum_free_size(int caps);
size_t heap_caps_get_largest_free_block(int caps);
#else
#include <stdlib.h>

static inline void *heap_caps_malloc(size_t size, int caps) {
    (void)caps;
    return malloc(size);
}

static inline size_t heap_caps_get_free_size(int caps) {
    return caps == MALLOC_CAP_SPIRAM ? 4000000u : 64000u;
}

static inline size_t heap_caps_get_minimum_free_size(int caps) {
    (void)caps;
    return 48000u;
}

static inline size_t heap_caps_get_largest_free_block(int caps) {
    (void)caps;
    return 32000u;
}
#endif
