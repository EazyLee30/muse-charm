#pragma once
#include <stdint.h>
#include <stdbool.h>
#define CHARM_FONT_W 18
#define CHARM_FONT_H 22
#define CHARM_FONT_STRIDE 3
#define CHARM_FONT_BYTES (CHARM_FONT_H*CHARM_FONT_STRIDE)
int charm_font_advance(uint32_t cp);
bool charm_font_init(void);
const uint8_t *charm_font_glyph(uint32_t codepoint);
