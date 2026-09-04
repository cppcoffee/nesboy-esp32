#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "gnuboy.h"
#include "nofrendo.h"
#include "pins.h"
#include "snes.h"

#include "app_config.h"
#include "display.h"
#include "font8x8.h"

enum {
    LCD_HOST = SPI2_HOST,
    LCD_PCLK_HZ = 80 * 1000 * 1000,
    LCD_DMA_CHUNK_LINES = 20,
    LCD_DMA_CHUNKS = LCD_H / LCD_DMA_CHUNK_LINES,
    ST7789_CMD_FRCTRL2 = 0xC6,
    ST7789_60_HZ = 0x0F,
    OSD_FRAMES = 60,    /* 1 second at 60 FPS */
    OSD_TEXT_ROWS = 18, /* dimmed band behind OSD text */
};

static const char *TAG = "display";

/* ---- SPI / DMA pipeline state ---- */
struct display_pipe {
    esp_lcd_panel_handle_t lcd_panel;
    SemaphoreHandle_t lcd_done;
    int pending_transfers;
    uint16_t *fb[2];
};

/* ---- On-screen display state ---- */
struct display_osd {
    int frames;
    int volume;
    int brightness;
    char text[16]; /* non-empty: text mode (SAVED / LOADED / ...) */
};

/* Top-level display state */
static struct {
    struct display_pipe pipe;
    uint16_t palette565[256];
    int brightness_pct;
    struct display_osd osd;
} disp;

/* Pre-filled colour rows for OSD memcpy */
static uint16_t osd_red_row[LCD_W];
static uint16_t osd_blue_row[LCD_W];

/* ---- DMA helpers ---- */
static bool lcd_flush_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    BaseType_t hi = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)user_ctx, &hi);
    return hi == pdTRUE;
}

static void lcd_wait_buffer_dma(void)
{
    if (disp.pipe.pending_transfers < 2) {
        return;
    }
    xSemaphoreTake(disp.pipe.lcd_done, portMAX_DELAY);
    disp.pipe.pending_transfers--;
}

static void lcd_queue_chunk_dma(int y, int rows, const void *data)
{
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(disp.pipe.lcd_panel, 0, y, LCD_W, y + rows, data));
    disp.pipe.pending_transfers++;
}

static void scale_gb_row(const uint16_t *src, uint16_t *dst)
{
    /* Exact nearest-neighbor 3:2 expansion used by x * 2 / 3:
     * every two source pixels become [a, a, b]. */
    for (int x = 0; x < GB_WIDTH; x += 2) {
        uint16_t a = *src++;
        uint16_t b = *src++;
        *dst++ = a;
        *dst++ = a;
        *dst++ = b;
    }
}

static void dim_rows(uint16_t *buffer, int top, int bottom)
{
    for (int row = top; row < bottom; row++) {
        uint16_t *line = buffer + row * LCD_W;
        for (int x = 0; x < LCD_W; x++) {
            line[x] = (line[x] >> 1) & 0x7BEF;
        }
    }
}

static void draw_osd_text(uint16_t *buffer, int screen_y0)
{
    const int text_h = 16;
    /* 16-row text block centered on the 240-row screen; each DMA chunk
     * draws the rows it owns. */
    const int top = (LCD_H - text_h) / 2;

    int band_top = top - 1 - screen_y0;
    int band_bottom = band_top + OSD_TEXT_ROWS;
    if (band_top < 0) {
        band_top = 0;
    }
    if (band_bottom > LCD_DMA_CHUNK_LINES) {
        band_bottom = LCD_DMA_CHUNK_LINES;
    }
    if (band_bottom > band_top) {
        dim_rows(buffer, band_top, band_bottom);
    }

    /* Typewriter reveal: two glyph columns appear per frame (8x16 text, each
     * font row doubled). The animation is driven by the OSD countdown, so it
     * lasts a fixed second regardless of how long the actual save took. */
    int len = (int)strlen(disp.osd.text);
    int text_w = len * 8;
    int reveal = (OSD_FRAMES - disp.osd.frames) * 2;
    if (reveal > text_w) {
        reveal = text_w;
    }

    int x0 = (LCD_W - text_w) / 2;
    for (int row = 0; row < 8; row++) {
        int py0 = top + row * 2 - screen_y0;
        if (py0 < 0 || py0 + 1 >= LCD_DMA_CHUNK_LINES) {
            continue;
        }
        uint16_t *line = buffer + (size_t)py0 * LCD_W + x0;
        for (int i = 0; i < len; i++) {
            char c = disp.osd.text[i];
            if (c < 0x20 || c > 0x7E) {
                c = ' ';
            }
            uint8_t bits = font8x8_basic[(int)c - 0x20][row];
            for (int col = 0; col < 8; col++) {
                if ((bits & (1 << col)) && (i * 8 + col) < reveal) {
                    line[i * 8 + col] = 0xFFFF;
                }
            }
        }
        memcpy(line + LCD_W, line, (size_t)text_w * sizeof(*line));
    }
}

static void draw_osd(uint16_t *buffer, int screen_y0)
{
    if (disp.osd.frames <= 0) {
        return;
    }

    if (disp.osd.text[0] != '\0') {
        draw_osd_text(buffer, screen_y0);
        if (screen_y0 == 0) {
            disp.osd.frames--;
        }
        return;
    }

    /* Volume/brightness bars live in the bottom DMA chunk. */
    if (screen_y0 != LCD_H - LCD_DMA_CHUNK_LINES) {
        return;
    }

    dim_rows(buffer, LCD_DMA_CHUNK_LINES - 10, LCD_DMA_CHUNK_LINES);
    int vol_w = disp.osd.volume * (LCD_W - 24) / 100;
    for (int row = LCD_DMA_CHUNK_LINES - 8; row < LCD_DMA_CHUNK_LINES - 4; row++) {
        memcpy(buffer + row * LCD_W + 12, osd_red_row, vol_w * sizeof(uint16_t));
    }
    int bri_w = disp.osd.brightness * (LCD_W - 24) / 100;
    for (int row = LCD_DMA_CHUNK_LINES - 4; row < LCD_DMA_CHUNK_LINES; row++) {
        memcpy(buffer + row * LCD_W + 12, osd_blue_row, bri_w * sizeof(uint16_t));
    }
    disp.osd.frames--;
}

/* ---- Public API ---- */
void display_init(void)
{
    disp.brightness_pct = 85;

    /* Pre-fill OSD colour rows (doubling memcpy) */
    osd_red_row[0] = 0xF800;
    osd_blue_row[0] = 0x001F;
    for (int n = 1; n < LCD_W; n <<= 1) {
        int fill = n < (LCD_W - n) ? n : (LCD_W - n);
        memcpy(osd_red_row + n, osd_red_row, (size_t)fill * sizeof(uint16_t));
        memcpy(osd_blue_row + n, osd_blue_row, (size_t)fill * sizeof(uint16_t));
    }

    /* LEDC PWM for backlight brightness */
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    ledc_channel_config_t ledc_chan = {
        .channel = LEDC_CHANNEL_0,
        .duty = (disp.brightness_pct * 255 + 50) / 100,
        .gpio_num = LCD_BLK_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .hpoint = 0,
        .timer_sel = LEDC_TIMER_0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_chan));

    disp.pipe.lcd_done = xSemaphoreCreateCounting(2, 0);
    if (!disp.pipe.lcd_done) {
        abort();
    }

    spi_bus_config_t bus = {
        .mosi_io_num = LCD_SDA_PIN,
        .miso_io_num = -1,
        .sclk_io_num = LCD_SCL_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * LCD_H * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = LCD_DC_PIN,
        .cs_gpio_num = LCD_CS_PIN,
        .pclk_hz = LCD_PCLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 2,
        .on_color_trans_done = lcd_flush_done,
        .user_ctx = disp.pipe.lcd_done,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RES_PIN,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &disp.pipe.lcd_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(disp.pipe.lcd_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(disp.pipe.lcd_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(disp.pipe.lcd_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(disp.pipe.lcd_panel, true));

#if LCD_ROTATE_180
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(disp.pipe.lcd_panel, true, true));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(disp.pipe.lcd_panel, 0, 80));
    uint8_t madctl = LCD_CMD_MX_BIT | LCD_CMD_MY_BIT | LCD_CMD_ML_BIT;
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, &madctl, sizeof(madctl)));
#endif

    uint8_t frame_rate = ST7789_60_HZ;
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, ST7789_CMD_FRCTRL2, &frame_rate, sizeof(frame_rate)));

    disp.pipe.fb[0] =
        heap_caps_malloc(LCD_W * LCD_DMA_CHUNK_LINES * 2 * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!disp.pipe.fb[0]) {
        ESP_LOGE(TAG, "out of memory: framebuffers");
        abort();
    }
    disp.pipe.fb[1] = disp.pipe.fb[0] + LCD_W * LCD_DMA_CHUNK_LINES;

    /* Clear the whole screen to black so no garbage shows before the first frame. */
    memset(disp.pipe.fb[0], 0, LCD_W * LCD_DMA_CHUNK_LINES * sizeof(uint16_t));
    for (int y = 0; y < LCD_H; y += LCD_DMA_CHUNK_LINES) {
        ESP_ERROR_CHECK(
            esp_lcd_panel_draw_bitmap(disp.pipe.lcd_panel, 0, y, LCD_W, y + LCD_DMA_CHUNK_LINES, disp.pipe.fb[0]));
        xSemaphoreTake(disp.pipe.lcd_done, portMAX_DELAY);
    }

    ESP_LOGI(TAG, "ST7789 ready: spi=%dMHz panel=60Hz DMA=2x%d lines", LCD_PCLK_HZ / 1000000, LCD_DMA_CHUNK_LINES);
}

void display_set_brightness(int delta)
{
    int new = disp.brightness_pct + delta;
    if (new < 0) {
        new = 0;
    } else if (new > 100) {
        new = 100;
    }

    if (new != disp.brightness_pct) {
        disp.brightness_pct = new;
        uint32_t duty = (disp.brightness_pct * 255 + 50) / 100;
        ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty));
        ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0));
        ESP_LOGI(TAG, "brightness=%d%%", disp.brightness_pct);
    }
}

int display_get_brightness(void)
{
    return disp.brightness_pct;
}

void display_osd_show(int volume, int brightness)
{
    disp.osd = (struct display_osd){
        .frames = OSD_FRAMES,
        .volume = volume,
        .brightness = brightness,
    };
}

void display_osd_text(const char *text)
{
    disp.osd = (struct display_osd){
        .frames = OSD_FRAMES,
        .volume = 0,
        .brightness = 0,
    };
    snprintf(disp.osd.text, sizeof(disp.osd.text), "%s", text ? text : "");
}

void display_build_palette(void)
{
    uint16_t *pal = nofrendo_buildpalette(NES_PALETTE_PVM, 16);
    memcpy(disp.palette565, pal, sizeof(disp.palette565));
    free(pal);
}

/* Shared chunked DMA blit pipeline. `fill` writes one 240-pixel row into
 * one of two alternating buffers; `osd` draws the overlay per chunk. */
typedef void (*display_fill_row_fn)(uint16_t *dst, int screen_y, int row, void *arg);

static void blit_chunks(display_fill_row_fn fill, void *arg, bool osd)
{
    int y = 0;
    for (int chunk = 0; chunk < LCD_DMA_CHUNKS; chunk++) {
        lcd_wait_buffer_dma();
        uint16_t *buffer = disp.pipe.fb[chunk & 1];
        for (int row = 0; row < LCD_DMA_CHUNK_LINES; row++) {
            fill(buffer + row * LCD_W, y + row, row, arg);
        }
        if (osd) {
            draw_osd(buffer, y);
        }
        lcd_queue_chunk_dma(y, LCD_DMA_CHUNK_LINES, buffer);
        y += LCD_DMA_CHUNK_LINES;
    }
}

static void blit_fill_nes(uint16_t *dst, int screen_y, int row, void *arg)
{
    (void)row;
    const uint8_t *src = NES_SCREEN_GETPTR((uint8 *)arg, 8, screen_y);
    for (int x = 0; x < LCD_W; x++) {
        dst[x] = disp.palette565[src[x]];
    }
}

void display_blit(uint8 *bmp)
{
    if (!bmp) {
        return;
    }
    blit_chunks(blit_fill_nes, bmp, true);
}

static void blit_fill_gb(uint16_t *dst, int screen_y, int row, void *arg)
{
    const uint16_t *bmp = arg;
    /* 160x144 centered: 12 blank rows above, 84 below. */
    if (screen_y < 12 || screen_y >= 228) {
        memset(dst, 0, LCD_W * sizeof(uint16_t));
        return;
    }

    int visible_y = screen_y - 12;
    if ((visible_y % 3) == 1 && row > 0) {
        /* Duplicate the previous output row; skipped at chunk boundaries,
         * where the previous row is not in this chunk's buffer. */
        memcpy(dst, dst - LCD_W, LCD_W * sizeof(uint16_t));
    } else {
        const uint16_t *src = bmp + (visible_y * 2 / 3) * GB_WIDTH;
        scale_gb_row(src, dst);
    }
}

void display_blit_gb(const uint16_t *bmp)
{
    if (!bmp) {
        return;
    }
    blit_chunks(blit_fill_gb, (void *)bmp, true);
}

/* Horizontal 256 -> 240 nearest-neighbor mapping: copy 15 pixels and drop
 * every 16th source pixel. */
static void blit_fill_snes(uint16_t *dst, int screen_y, int row, void *arg)
{
    (void)row;
    const uint16_t *bmp = arg;
    const int top_blank = (LCD_H - 224) / 2; /* 8 black rows top and bottom */
    if (screen_y < top_blank || screen_y >= top_blank + 224) {
        memset(dst, 0, LCD_W * sizeof(uint16_t));
    } else {
        const uint16_t *src = bmp + (size_t)(screen_y - top_blank) * SNES_WIDTH;
        for (int group = 0; group < SNES_WIDTH / 16; group++) {
            memcpy(dst, src, 15 * sizeof(*dst));
            dst += 15;
            src += 16;
        }
    }
}

void display_blit_snes(const uint16_t *bmp)
{
    if (!bmp) {
        return;
    }
    blit_chunks(blit_fill_snes, (void *)bmp, true);
}

static void blit_fill_full(uint16_t *dst, int screen_y, int row, void *arg)
{
    (void)row;
    memcpy(dst, (const uint16_t *)arg + (size_t)screen_y * LCD_W, LCD_W * sizeof(uint16_t));
}

void display_draw_fullscreen(const uint16_t *fb)
{
    /* Full-screen blit for menus. Reuse the DMA pipeline to copy the source
     * (which may live in PSRAM) through the two internal DMA buffers. */
    blit_chunks(blit_fill_full, (void *)fb, false);
    /* Drain the last chunk */
    while (disp.pipe.pending_transfers > 0) {
        xSemaphoreTake(disp.pipe.lcd_done, portMAX_DELAY);
        disp.pipe.pending_transfers--;
    }
}
