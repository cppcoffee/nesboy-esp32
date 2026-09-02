#include "emulator_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nofrendo.h"

#include "app_config.h"
#include "audio.h"
#include "buttons.h"
#include "display.h"
#include "frame_stats.h"
#include "rewind.h"

static const char *TAG = "emulator-nes";

enum {
    NES_BUF_PITCH = 8 + 256 + 8,
    NES_BUF_HEIGHT = 240,
    NES_BUF_SIZE = NES_BUF_PITCH * NES_BUF_HEIGHT,
};

static int load_rom(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        ESP_LOGE(TAG, "fopen(%s) failed", path);
        return -1;
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size < 16 || size > 0x200000) {
        ESP_LOGE(TAG, "bad ROM size: %ld", size);
        fclose(file);
        return -1;
    }

    uint8_t *data = heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM);
    if (!data) {
        ESP_LOGE(TAG, "no PSRAM for ROM (%ld bytes)", size);
        fclose(file);
        return -1;
    }
    if (fread(data, 1, (size_t)size, file) != (size_t)size) {
        ESP_LOGE(TAG, "read failed");
        fclose(file);
        free(data);
        return -1;
    }
    fclose(file);

    if (nes_insertcart(rom_loadmem(data, size)) < 0) {
        ESP_LOGE(TAG, "rom_loadmem failed");
        return -1;
    }
    ESP_LOGI(TAG, "ROM loaded: %s (%ld bytes)", path, size);
    return 0;
}

static int rewind_save(uint8_t *buffer)
{
    return state_save_mem(buffer);
}

static int rewind_load(const uint8_t *buffer)
{
    return state_load_mem(buffer);
}

static void rewind_preview(void)
{
    nes_emulate(true);
}

int emulator_nes_run(const char *rom_path)
{
    display_build_palette();

    uint8_t *pixels = heap_caps_malloc(NES_BUF_SIZE, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!pixels) {
        ESP_LOGE(TAG, "out of memory: pixels");
        return -1;
    }

    nes_t *nes = nes_init(SYS_DETECT, AUDIO_RATE, false, NULL);
    if (!nes) {
        ESP_LOGE(TAG, "nes_init failed");
        return -1;
    }
    nes->blit_func = display_blit;

    if (load_rom(rom_path) < 0) {
        return -1;
    }
    nes_setvidbuf(pixels);
    const rewind_backend_t rewind_backend = {
        .state_size = state_mem_size(),
        .refresh_rate = nes->refresh_rate,
        .slots = NES_REWIND_SLOTS,
        .save = rewind_save,
        .load = rewind_load,
        .preview = rewind_preview,
    };
    rewind_init(&rewind_backend);

    const TickType_t frame_delay = pdMS_TO_TICKS(1000 / nes->refresh_rate);
    emulator_settings_t settings = {0};
    int previous_buttons = 0;

    while (1) {
        int buttons = buttons_read();
        input_update(0, buttons);

        int pressed = buttons & ~previous_buttons;
        previous_buttons = buttons;
        emulator_settings_update(&settings, buttons, pressed);

        if (emulator_handle_state_controls(rom_path, &rewind_backend, buttons)) {
            vTaskDelay(frame_delay);
            continue;
        }

        frame_stats_begin();
        nes_emulate(true);
        audio_write(nes->apu->buffer, nes->apu->samples_per_frame);
        frame_stats_end();
    }
}
