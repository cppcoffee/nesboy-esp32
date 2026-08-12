#pragma once
#include <stdint.h>

/* Public-domain 8x8 bitmap font, ASCII 0x20..0x7F (96 glyphs, 8 bytes each).
 * Rendered at 8x16 (each row doubled vertically) for legibility on 240x240. */
extern const uint8_t font8x8_basic[96][8];
