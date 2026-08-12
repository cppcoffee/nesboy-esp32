#pragma once

#include <stdint.h>
#include "nofrendo.h"

/* Display dimensions */
#define LCD_W 240
#define LCD_H 240

/* Initialize LCD hardware (SPI, ST7789, DMA, backlight) */
void display_init(void);

/* Build the 8-bit → RGB565 palette LUT (call once after nofrendo init) */
void display_build_palette(void);

/* Blit one NES frame to the LCD. Designed to be used as nes->blit_func. */
void display_blit(uint8 *bmp);

/* Scale a 160x144 RGB565 Game Boy frame to 240x216 and center it vertically. */
void display_blit_gb(const uint16_t *bmp);

/* Adjust backlight brightness by ±delta percent points (clamped 0–100). */
void display_set_brightness(int delta);

/* Get current brightness percentage. */
int display_get_brightness(void);

/* Show an on-screen indicator with current volume and brightness. */
void display_osd_show(int volume, int brightness);

/* Show a short text overlay ("SAVED", "LOADED", ...) at the bottom of the
 * screen with a typewriter reveal animation, lasting about 1 second. */
void display_osd_text(const char *text);

/* Blit a full 240x240 RGB565 buffer to the LCD (blocking). Used by menus. */
void display_draw_fullscreen(const uint16_t *fb);
