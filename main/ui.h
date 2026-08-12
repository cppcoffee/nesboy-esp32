#pragma once
#include <stdint.h>
#include "display.h"

/* Minimal UI framebuffer for menus. RGB565, LCD_W x LCD_H, allocated in PSRAM.
 * font rendered at 8 wide x 16 tall (rows doubled). Screen dimensions come
 * from display.h (LCD_W / LCD_H) -- not redefined here. */

#define UI_FONT_W 8
#define UI_FONT_H 16 /* 8x8 glyph doubled vertically */

/* Standard colors (RGB565) */
#define UI_COLOR_BLACK  0x0000
#define UI_COLOR_WHITE  0xFFFF
#define UI_COLOR_BLUE   0x001F
#define UI_COLOR_GREEN  0x07E0
#define UI_COLOR_RED    0xF800
#define UI_COLOR_YELLOW 0xFFE0
#define UI_COLOR_CYAN   0x07FF
#define UI_COLOR_GREY   0x7BEF
#define UI_COLOR_DARK   0x2104

/* Allocate/free the menu framebuffer. */
void ui_init(void);
void ui_deinit(void);

/* Fill entire screen with a color. */
void ui_clear(uint16_t color);

/* Fill a rectangle. */
void ui_fill_rect(int x, int y, int w, int h, uint16_t color);

/* Draw one character at (x,y) in color. Returns x + FONT_W. */
int ui_draw_char(int x, int y, char c, uint16_t color);

/* Draw a string at (x,y). Clips to screen. Returns x after last char. */
int ui_draw_text(int x, int y, const char *s, uint16_t color);

/* Push the framebuffer to the LCD (blocking until DMA done). */
void ui_flush(void);
