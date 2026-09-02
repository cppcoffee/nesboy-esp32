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
#include "gba.h"
#include "rewind.h"

static const char *TAG = "emulator-gba";
static bool rewind_previewing;

enum {
    /* ~1/60 s of GBA audio at 32768 Hz, plus slack. */
    GBA_AUDIO_MAX_SAMPLES = GBA_AUDIO_RATE / 55 + 8,
    /* Battery file is synced when changed; checked every 5 display frames. */
    GBA_SRAM_SAVE_FRAMES = 5,
};

/* The device targets 30 display fps while the GBA runs at 60: each display
 * frame runs two emulated frames. The first one renders video (the blit is
 * fed from the second one too — the core re-renders on request), audio from
 * both is queued and the 48 kHz output paces the loop. */
#define GBA_FRAMES_PER_DISPLAY_FRAME 2

static void video_callback(void *buffer)
{
    display_blit_gba((const uint16_t *)buffer);
}

static void audio_callback(const int16_t *buffer, int samples)
{
    if (!rewind_previewing) {
        audio_write_stereo_resampled(buffer, samples, GBA_AUDIO_RATE);
    }
}

static uint32_t pad_from_buttons(int buttons)
{
    /* input_buttons_type bits from gba/input.h (BUTTON_A, BUTTON_B, ...) */
    uint32_t pad = 0;
    if (buttons & NES_PAD_RIGHT) {
        pad |= 0x10; /* BUTTON_RIGHT */
    }
    if (buttons & NES_PAD_LEFT) {
        pad |= 0x20; /* BUTTON_LEFT */
    }
    if (buttons & NES_PAD_UP) {
        pad |= 0x40; /* BUTTON_UP */
    }
    if (buttons & NES_PAD_DOWN) {
        pad |= 0x80; /* BUTTON_DOWN */
    }
    if (buttons & NES_PAD_A) {
        pad |= 0x01; /* BUTTON_A */
    }
    if (buttons & NES_PAD_B) {
        pad |= 0x02; /* BUTTON_B */
    }
    if (buttons & NES_PAD_SELECT) {
        pad |= 0x04; /* BUTTON_SELECT */
    }
    if (buttons & NES_PAD_START) {
        pad |= 0x08; /* BUTTON_START */
    }
    return pad;
}

static int rewind_save(uint8_t *buffer)
{
    return gba_save_state_mem(buffer);
}

static int rewind_load(const uint8_t *buffer)
{
    return gba_load_state_mem(buffer);
}

static void rewind_preview(void)
{
    rewind_previewing = true;
    gba_run_frame();
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
    if (make_save_path(rom_path, save_path, sizeof(save_path), ".sav") < 0) {
        return -1;
    }
    FILE *file = fopen(save_path, "rb");
    if (!file) {
        return -1;
    }
    size_t size = fread(gba_backup_ram(), 1, gba_backup_ram_size(), file);
    fclose(file);
    ESP_LOGI(TAG, "loaded battery RAM: %s (%u bytes)", save_path, (unsigned)size);
    return 0;
}

static int save_sram(const char *rom_path)
{
    char save_path[192];
    if (make_save_path(rom_path, save_path, sizeof(save_path), ".sav") < 0) {
        return -1;
    }
    FILE *file = fopen(save_path, "wb");
    if (!file) {
        return -1;
    }
    size_t written = fwrite(gba_backup_ram(), 1, gba_backup_ram_size(), file);
    fclose(file);
    if (written != gba_backup_ram_size()) {
        return -1;
    }
    return 0;
}

int emulator_gba_run(const char *rom_path)
{
    /* GBA ROMs are large: cache 8 MB of the ROM in PSRAM (the core swaps
     * 32 KB pages from the file on the SD card beyond that). The 77 KB
     * frame buffer also lives in PSRAM — the blit is a per-row copy loop,
     * so PSRAM latency is negligible, and it keeps internal RAM for the
     * DMA/LCD pipeline. */
    uint16_t *pixels =
        heap_caps_malloc((GBA_WIDTH * (GBA_HEIGHT + 1)) * sizeof(uint16_t), MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    int16_t *sound =
        heap_caps_malloc(GBA_AUDIO_MAX_SAMPLES * 2 * sizeof(int16_t), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!pixels || !sound) {
        ESP_LOGE(TAG, "out of memory: GBA buffers");
        return -1;
    }

    if (gba_init(AUDIO_RATE, GBA_AUDIO_STEREO_S16, GBA_PIXEL_565_LE, video_callback, audio_callback) < 0) {
        ESP_LOGE(TAG, "gba_init failed");
        return -1;
    }

    gba_set_framebuffer(pixels);
    gba_set_soundbuffer(sound, GBA_AUDIO_MAX_SAMPLES * 2);
    if (gba_load_rom_file(rom_path) < 0) {
        ESP_LOGE(TAG, "gba_load_rom_file(%s) failed", rom_path);
        return -1;
    }

    gba_reset(true);
    load_sram(rom_path);

    const rewind_backend_t rewind_backend = {
        .state_size = gba_state_size(),
        .refresh_rate = 30, /* display frames per second */
        .slots = GBA_REWIND_SLOTS,
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
        gba_set_pad(pad_from_buttons(buttons));
        previous_buttons = buttons;
        emulator_settings_update(&settings, buttons, pressed);

        if (emulator_handle_state_controls(rom_path, &rewind_backend, buttons)) {
            vTaskDelay(frame_delay);
            continue;
        }

        frame_stats_begin();
        for (int f = 0; f < GBA_FRAMES_PER_DISPLAY_FRAME; f++) {
            if (f == GBA_FRAMES_PER_DISPLAY_FRAME - 1) {
                /* Render the last emulated frame of the pair; earlier ones
                 * skip video but still feed audio. */
                gba_set_video_skip(false);
            } else {
                gba_set_video_skip(true);
            }
            gba_run_frame();
        }
        gba_set_video_skip(false);
        frame_stats_emulation_done();
        frame_stats_end();

        if (++save_timer >= GBA_SRAM_SAVE_FRAMES) {
            save_timer = 0;
            if (save_sram(rom_path) < 0) {
                ESP_LOGE(TAG, "failed to save battery RAM: %s", rom_path);
            }
        }
    }
}
