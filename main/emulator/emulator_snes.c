#include "emulator_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "audio.h"
#include "buttons.h"
#include "display.h"
#include "frame_stats.h"
#include "rewind.h"
#include "snes.h"

static const char *TAG = "emulator-snes";
static bool rewind_previewing;

/* Render at 30 fps while preserving the ROM's native 50/60 Hz timing. */
#define SNES_DISPLAY_FPS 30

static void video_callback(void *buffer)
{
    display_blit_snes((const uint16_t *)buffer);
}

static void audio_callback(const int16_t *buffer, int samples)
{
    if (!rewind_previewing) {
        audio_write_stereo(buffer, samples);
    }
}

/* SNES_*_MASK bits from the core (snes9x.h) */
static uint32_t pad_from_buttons(int buttons)
{
    uint32_t pad = 0;
    if (buttons & NES_PAD_RIGHT) {
        pad |= SNES_RIGHT_MASK;
    }
    if (buttons & NES_PAD_LEFT) {
        pad |= SNES_LEFT_MASK;
    }
    if (buttons & NES_PAD_UP) {
        pad |= SNES_UP_MASK;
    }
    if (buttons & NES_PAD_DOWN) {
        pad |= SNES_DOWN_MASK;
    }
    if (buttons & NES_PAD_A) {
        pad |= SNES_A_MASK;
    }
    if (buttons & NES_PAD_B) {
        pad |= SNES_B_MASK;
    }
    if (buttons & NES_PAD_SELECT) {
        pad |= SNES_SELECT_MASK;
    }
    if (buttons & NES_PAD_START) {
        pad |= SNES_START_MASK;
    }
    return pad;
}

static int rewind_save(uint8_t *buffer)
{
    size_t written = 0;
    if (snes_save_state_mem(buffer, &written) < 0) {
        return -1;
    }
    return (int)written;
}

static int rewind_load(const uint8_t *buffer)
{
    return snes_load_state_mem(buffer, snes_state_size());
}

static void rewind_preview(void)
{
    rewind_previewing = true;
    snes_run_frame();
    rewind_previewing = false;
}

int emulator_snes_run(const char *rom_path)
{
    /* 256x239 (extended height) RGB565 frame buffer. It does not fit in
     * internal RAM next to the rest of the system, so it lives in PSRAM;
     * the blit is a per-row copy loop, which PSRAM handles fine. */
    uint16_t *pixels = heap_caps_malloc(
        SNES_WIDTH * SNES_HEIGHT_EXTENDED * sizeof(uint16_t), MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (!pixels) {
        ESP_LOGE(TAG, "out of memory: SNES pixels");
        return -1;
    }

    if (snes_init(SNES_AUDIO_RATE_DEFAULT, video_callback, audio_callback) < 0) {
        ESP_LOGE(TAG, "snes_init failed");
        return -1;
    }

    snes_set_framebuffer(pixels);
    if (snes_load_rom_file(rom_path) < 0) {
        ESP_LOGE(TAG, "snes_load_rom_file(%s) failed", rom_path);
        return -1;
    }

    snes_reset();

    uint32_t rom_fps = snes_rom_frames_per_second();
    if (rom_fps != 50 && rom_fps != 60) {
        rom_fps = 60;
    }

    const rewind_backend_t rewind_backend = {
        .state_size = snes_state_size(),
        .refresh_rate = 30, /* display frames per second */
        .slots = SNES_REWIND_SLOTS,
        .save = rewind_save,
        .load = rewind_load,
        .preview = rewind_preview,
    };
    rewind_init(&rewind_backend);

    const TickType_t frame_delay = pdMS_TO_TICKS(1000 / rewind_backend.refresh_rate);
    emulator_settings_t settings = {0};
    int previous_buttons = 0;
    uint32_t emulated_frame_phase = 0;
    uint32_t display_tick_phase = 0;
    TickType_t last_wake = xTaskGetTickCount();

    while (1) {
        int buttons = buttons_read();
        int pressed = buttons & ~previous_buttons;
        snes_set_pad(pad_from_buttons(buttons));
        previous_buttons = buttons;
        emulator_settings_update(&settings, buttons, pressed);

        if (emulator_handle_state_controls(rom_path, &rewind_backend, buttons)) {
            vTaskDelay(frame_delay);
            last_wake = xTaskGetTickCount();
            display_tick_phase = 0;
            continue;
        }

        /* The audio queue paces the long-run average; this delay keeps the
         * blit on an even 30 Hz grid so PAL's 1-2-2 emulated-frame pattern
         * does not surface as 20/40 ms blit jitter. */
        TickType_t display_delay = configTICK_RATE_HZ / SNES_DISPLAY_FPS;
        display_tick_phase += configTICK_RATE_HZ % SNES_DISPLAY_FPS;
        if (display_tick_phase >= SNES_DISPLAY_FPS) {
            display_tick_phase -= SNES_DISPLAY_FPS;
            display_delay++;
        }
        vTaskDelayUntil(&last_wake, display_delay);

        frame_stats_begin();
        emulated_frame_phase += rom_fps;
        int frames_to_run = (int)(emulated_frame_phase / SNES_DISPLAY_FPS);
        emulated_frame_phase %= SNES_DISPLAY_FPS;
        for (int f = 0; f < frames_to_run; f++) {
            snes_set_video_skip(f != frames_to_run - 1);
            snes_run_frame();
        }
        snes_set_video_skip(false);
        frame_stats_end();
    }
}
