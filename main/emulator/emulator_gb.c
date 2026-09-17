#include "emulator_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gnuboy.h"
#include "nofrendo.h"

#include "app_config.h"
#include "audio.h"
#include "buttons.h"
#include "frame_stats.h"
#include "rewind.h"

static const char *TAG = "emulator-gb";
static bool rewind_previewing;

enum {
    GB_AUDIO_MAX_SAMPLES = AUDIO_RATE / 50 + 2,
};

static void video_callback(void *buffer)
{
    gnuboy_set_framebuffer(emulator_video_present(buffer));
}

static void fill_display_row(uint16_t *dst, const uint16_t *previous, int screen_y, const void *frame)
{
    if (screen_y < 12 || screen_y >= 228) {
        memset(dst, 0, LCD_W * sizeof(*dst));
        return;
    }

    int visible_y = screen_y - 12;
    const uint16_t *src = (const uint16_t *)frame + visible_y * 2 / 3 * GB_WIDTH;
#if GB_SCALE_FAST
    if ((visible_y % 3) == 1 && previous) {
        memcpy(dst, previous, LCD_W * sizeof(*dst));
        return;
    }

    for (int x = 0; x < GB_WIDTH; x += 2) {
        uint16_t a = *src++;
        uint16_t b = *src++;
        *dst++ = a;
        *dst++ = a;
        *dst++ = b;
    }
#else
    (void)previous;
    /* Naive nearest-neighbor 3:2: recompute every output column. */
    for (int x = 0; x < LCD_W; x++) {
        dst[x] = src[x * 2 / 3];
    }
#endif
}

static void audio_callback(void *buffer, size_t length)
{
    if (!rewind_previewing) {
        audio_write_stereo(buffer, (int)(length / 2));
    }
}

static int pad_from_buttons(int buttons)
{
    int pad = 0;
    if (buttons & NES_PAD_RIGHT) {
        pad |= GB_PAD_RIGHT;
    }
    if (buttons & NES_PAD_LEFT) {
        pad |= GB_PAD_LEFT;
    }
    if (buttons & NES_PAD_UP) {
        pad |= GB_PAD_UP;
    }
    if (buttons & NES_PAD_DOWN) {
        pad |= GB_PAD_DOWN;
    }
    if (buttons & NES_PAD_A) {
        pad |= GB_PAD_A;
    }
    if (buttons & NES_PAD_B) {
        pad |= GB_PAD_B;
    }
    if (buttons & NES_PAD_SELECT) {
        pad |= GB_PAD_SELECT;
    }
    if (buttons & NES_PAD_START) {
        pad |= GB_PAD_START;
    }
    return pad;
}

static int rewind_save(uint8_t *buffer)
{
    return gnuboy_save_state_mem(buffer, gnuboy_state_size());
}

static int rewind_load(const uint8_t *buffer)
{
    return gnuboy_load_state_mem(buffer, gnuboy_state_size());
}

static void rewind_preview(void)
{
    rewind_previewing = true;
    gnuboy_run(true);
    rewind_previewing = false;
}

int emulator_gb_run(const char *rom_path)
{
    uint16_t *pixels = heap_caps_malloc(GB_WIDTH * GB_HEIGHT * sizeof(uint16_t), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    int16_t *sound =
        heap_caps_malloc(GB_AUDIO_MAX_SAMPLES * 2 * sizeof(int16_t), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!pixels || !sound) {
        ESP_LOGE(TAG, "out of memory: Game Boy buffers");
        return -1;
    }

    if (gnuboy_init(AUDIO_RATE, GB_AUDIO_STEREO_S16, GB_PIXEL_565_LE, video_callback, audio_callback) < 0) {
        ESP_LOGE(TAG, "gnuboy_init failed");
        return -1;
    }

    gnuboy_set_framebuffer(pixels);
    gnuboy_set_soundbuffer(sound, GB_AUDIO_MAX_SAMPLES * 2);
    if (gnuboy_load_rom_file(rom_path) < 0) {
        return -1;
    }

    emulator_video_start(GB_WIDTH * GB_HEIGHT * sizeof(uint16_t), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL,
                         fill_display_row);

    gnuboy_set_palette(GB_PALETTE_DMG);
    gnuboy_reset(true);

    const rewind_backend_t rewind_backend = {
        .state_size = gnuboy_state_size(),
        .refresh_rate = 60,
        .slots = GB_REWIND_SLOTS,
        .save = rewind_save,
        .load = rewind_load,
        .preview = rewind_preview,
    };
#if REWIND_ENABLE
    rewind_init(&rewind_backend);
#endif

    const TickType_t frame_delay = pdMS_TO_TICKS(1000 / rewind_backend.refresh_rate);
    emulator_settings_t settings = {0};
    int previous_buttons = 0;

    while (1) {
        int buttons = buttons_read();
        int pressed = buttons & ~previous_buttons;
        gnuboy_set_pad(pad_from_buttons(buttons));
        previous_buttons = buttons;
        emulator_settings_update(&settings, buttons, pressed);

        if (emulator_handle_state_controls(rom_path, &rewind_backend, buttons)) {
            vTaskDelay(frame_delay);
            continue;
        }

        frame_stats_begin();
        gnuboy_run(true);
        frame_stats_end(true);
    }
}
