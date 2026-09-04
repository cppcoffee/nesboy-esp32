#pragma once

#include <stdint.h>

/* Display dimensions */
#define LCD_W 240
#define LCD_H 240

/* Initialize LCD hardware (SPI, ST7789, DMA, backlight) */
void display_init(void);

/* Convert and send one emulator frame. The emulator owns its pixel-format
 * adapter; display only owns the LCD/DMA pipeline. */
typedef void (*display_fill_row_fn)(uint16_t *dst, const uint16_t *previous,
                                    int screen_y, const void *frame);
void display_blit(const void *frame, display_fill_row_fn fill_row);

/* Adjust backlight brightness by ±delta percent points (clamped 0–100). */
void display_set_brightness(int delta);

/* Get current brightness percentage. */
int display_get_brightness(void);

/* Show an on-screen indicator with current volume and brightness. */
void display_osd_show(int volume, int brightness);

/* Show a short text overlay ("SAVED", "LOADED", ...) in the middle of the
 * screen with a typewriter reveal animation, lasting about 1 second. */
void display_osd_text(const char *text);

/* Blit a full 240x240 RGB565 buffer to the LCD (blocking). Used by menus. */
void display_draw_fullscreen(const uint16_t *fb);
