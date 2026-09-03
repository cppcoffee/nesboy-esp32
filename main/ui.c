#include "ui.h"
#include "display.h"
#include "font8x8.h"
#include "esp_heap_caps.h"
#include <stdlib.h>
#include <string.h>

static uint16_t *ui_fb = NULL;

void ui_init(void)
{
    if (!ui_fb) {
        ui_fb = heap_caps_malloc(LCD_W * LCD_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    }
}

void ui_deinit(void)
{
    free(ui_fb);
    ui_fb = NULL;
}

void ui_clear(uint16_t color)
{
    if (!ui_fb) {
        return;
    }
    /* fast fill: exploit uint16 fill via the first word, then doubling copy */
    ui_fb[0] = color;
    for (int n = 1; n < LCD_W * LCD_H; n <<= 1) {
        int fill = n < (LCD_W * LCD_H - n) ? n : (LCD_W * LCD_H - n);
        memcpy(ui_fb + n, ui_fb, (size_t)fill * sizeof(uint16_t));
    }
}

void ui_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (!ui_fb) {
        return;
    }
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > LCD_W) {
        w = LCD_W - x;
    }
    if (y + h > LCD_H) {
        h = LCD_H - y;
    }
    if (w <= 0 || h <= 0) {
        return;
    }

    for (int row = 0; row < h; row++) {
        uint16_t *line = ui_fb + (size_t)(y + row) * LCD_W + x;
        for (int col = 0; col < w; col++) {
            line[col] = color;
        }
    }
}

int ui_draw_char(int x, int y, char c, uint16_t color)
{
    if (!ui_fb) {
        return x;
    }
    if (c < 0x20 || c > 0x7E) {
        c = ' ';
    }
    const uint8_t *glyph = font8x8_basic[(int)c - 0x20];

    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        int py0 = y + row * 2;
        int py1 = py0 + 1;
        if (py0 < 0 || py0 >= LCD_H) {
            continue;
        }
        for (int col = 0; col < 8; col++) {
            if (bits & (1 << col)) {
                int px = x + col;
                if (px < 0 || px >= LCD_W) {
                    continue;
                }
                ui_fb[(size_t)py0 * LCD_W + px] = color;
                if (py1 >= 0 && py1 < LCD_H) {
                    ui_fb[(size_t)py1 * LCD_W + px] = color;
                }
            }
        }
    }
    return x + UI_FONT_W;
}

int ui_draw_text(int x, int y, const char *s, uint16_t color)
{
    while (*s) {
        x = ui_draw_char(x, y, *s, color);
        s++;
    }
    return x;
}

void ui_blit(int x, int y, int w, int h, const uint16_t *pixels)
{
    if (!ui_fb || !pixels) {
        return;
    }
    if (x < 0) {
        w += x;
        pixels -= x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        pixels -= y * w;
        y = 0;
    }
    if (x + w > LCD_W) {
        w = LCD_W - x;
    }
    if (y + h > LCD_H) {
        h = LCD_H - y;
    }
    if (w <= 0 || h <= 0) {
        return;
    }

    for (int row = 0; row < h; row++) {
        memcpy(ui_fb + (size_t)(y + row) * LCD_W + x, pixels + (size_t)row * w, (size_t)w * sizeof(uint16_t));
    }
}

void ui_blit_keyed(int x, int y, int w, int h, const uint16_t *pixels, uint16_t transparent)
{
    if (!ui_fb || !pixels || w <= 0 || h <= 0) {
        return;
    }

    int src_w = w;
    int src_x = 0;
    int src_y = 0;
    if (x < 0) {
        src_x = -x;
        w += x;
        x = 0;
    }
    if (y < 0) {
        src_y = -y;
        h += y;
        y = 0;
    }
    if (x + w > LCD_W) {
        w = LCD_W - x;
    }
    if (y + h > LCD_H) {
        h = LCD_H - y;
    }
    if (w <= 0 || h <= 0) {
        return;
    }

    for (int row = 0; row < h; row++) {
        uint16_t *dst = ui_fb + (size_t)(y + row) * LCD_W + x;
        const uint16_t *src = pixels + (size_t)(src_y + row) * src_w + src_x;
        for (int col = 0; col < w; col++) {
            if (src[col] != transparent) {
                dst[col] = src[col];
            }
        }
    }
}

void ui_flush(void)
{
    if (!ui_fb) {
        return;
    }
    display_draw_fullscreen(ui_fb);
}
