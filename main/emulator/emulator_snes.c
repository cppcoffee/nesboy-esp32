#include "emulator_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

enum {
    /* SNES audio renders at 32 kHz. Cap the mix to one frame worth. */
    SNES_AUDIO_BUFFER_SAMPLES = SNES_AUDIO_RATE_DEFAULT / 50 + 2,
    SNES_SRAM_SAVE_FRAMES = 5,
};

/* SNES runs at 60 (NTSC) or 50 (PAL) fps; each 30 Hz display frame runs two
 * emulated frames. PAL games run ~2.5% fast at this rate (50/2 vs 30), which
 * is acceptable for the 30 fps target. */
#define SNES_FRAMES_PER_DISPLAY_FRAME 2

static void video_callback(void *buffer)
{
    display_blit_snes((const uint16_t *)buffer);
}

static void audio_callback(const int16_t *buffer, int samples)
{
    if (!rewind_previewing) {
        audio_write_stereo_resampled(buffer, samples, SNES_AUDIO_RATE_DEFAULT);
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

static int make_save_path(const char *rom_path, char *save_path, size_t size, const char *extension)
{
    int written = snprintf(save_path, size, "%s", rom_path);
    if (written < 0 || (size_t)written >= size) {
        return -1;
    }
    char *slash = strrchr(save_path, '/');
    char *dot = strrchr(save_path, '.');
    if (!dot || (slash && dot < slash)) {
        dot = save_path + strlen(save_path);
    }
    snprintf(dot, size - (size_t)(dot - save_path), "%s", extension);
    return 0;
}

static int load_sram(const char *rom_path)
{
    char save_path[192];
    if (make_save_path(rom_path, save_path, sizeof(save_path), ".srm") < 0) {
        return -1;
    }
    FILE *file = fopen(save_path, "rb");
    if (!file) {
        return -1;
    }
    size_t size = fread(snes_sram(), 1, snes_sram_size(), file);
    fclose(file);
    ESP_LOGI(TAG, "loaded battery RAM: %s (%u bytes)", save_path, (unsigned)size);
    return 0;
}

static int save_sram(const char *rom_path)
{
    char save_path[192];
    if (make_save_path(rom_path, save_path, sizeof(save_path), ".srm") < 0) {
        return -1;
    }
    FILE *file = fopen(save_path, "wb");
    if (!file) {
        return -1;
    }
    size_t written = fwrite(snes_sram(), 1, snes_sram_size(), file);
    fclose(file);
    if (written != snes_sram_size()) {
        return -1;
    }
    return 0;
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
    load_sram(rom_path);

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
    int save_timer = 0;

    while (1) {
        int buttons = buttons_read();
        int pressed = buttons & ~previous_buttons;
        snes_set_pad(pad_from_buttons(buttons));
        previous_buttons = buttons;
        emulator_settings_update(&settings, buttons, pressed);

        if (emulator_handle_state_controls(rom_path, &rewind_backend, buttons)) {
            vTaskDelay(frame_delay);
            continue;
        }

        frame_stats_begin();
        for (int f = 0; f < SNES_FRAMES_PER_DISPLAY_FRAME; f++) {
            snes_set_video_skip(f != SNES_FRAMES_PER_DISPLAY_FRAME - 1);
            snes_run_frame();
        }
        snes_set_video_skip(false);
        frame_stats_emulation_done();
        frame_stats_end();

        if (++save_timer >= SNES_SRAM_SAVE_FRAMES) {
            save_timer = 0;
            if (save_sram(rom_path) < 0) {
                ESP_LOGE(TAG, "failed to save SRAM: %s", rom_path);
            }
        }
    }
}
