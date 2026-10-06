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

#include <stdint.h>

// 5x8 pixel font covering printable ASCII (see pixel_font.c for its license).
// Glyphs are column-major; bit 0 of each column byte is the top row.
#define PIXEL_FONT_FIRST  0x20
#define PIXEL_FONT_LAST   0x7e
#define PIXEL_FONT_WIDTH  5
#define PIXEL_FONT_HEIGHT 8

extern const uint8_t pixel_font[PIXEL_FONT_LAST - PIXEL_FONT_FIRST + 1][PIXEL_FONT_WIDTH];
