#include "emulator_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
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
    SNES_FRAME_TIME_US = 1000000,
    /* Ignore sub-millisecond scheduling jitter before dropping a frame. */
    SNES_RENDER_LATE_US = 1000,
    /* Recover after a debugger/save-state stall instead of skipping forever. */
    SNES_MAX_LAG_FRAMES = 4,
    SNES_DISPLAY_TASK_STACK = 4096,
    SNES_DISPLAY_TASK_PRIORITY = 3, /* audio on CPU0 remains higher at 4 */
};

static struct {
    QueueHandle_t ready;
    QueueHandle_t free;
    bool async;
} video_pipe;

static void display_task(void *arg)
{
    (void)arg;

    while (1) {
        uint16_t *pixels;
        xQueueReceive(video_pipe.ready, &pixels, portMAX_DELAY);
        display_blit_snes(pixels);
        xQueueSend(video_pipe.free, &pixels, portMAX_DELAY);
    }
}

static bool video_pipe_init(uint16_t *spare_pixels)
{
    video_pipe.ready = xQueueCreate(1, sizeof(uint16_t *));
    video_pipe.free = xQueueCreate(1, sizeof(uint16_t *));
    if (!video_pipe.ready || !video_pipe.free ||
        xTaskCreatePinnedToCore(display_task, "snes-display", SNES_DISPLAY_TASK_STACK, NULL,
                                SNES_DISPLAY_TASK_PRIORITY, NULL, 0) != pdPASS) {
        if (video_pipe.ready) {
            vQueueDelete(video_pipe.ready);
        }
        if (video_pipe.free) {
            vQueueDelete(video_pipe.free);
        }
        video_pipe = (typeof(video_pipe)){0};
        return false;
    }

    xQueueSend(video_pipe.free, &spare_pixels, 0);
    video_pipe.async = true;
    return true;
}

static void video_callback(void *buffer)
{
    uint16_t *pixels = buffer;
    if (!video_pipe.async) {
        display_blit_snes(pixels);
        return;
    }

    /* CPU0 scales/transmits this completed buffer while CPU1 renders into
     * the other one. Back-pressure here also prevents tearing. */
    xQueueSend(video_pipe.ready, &pixels, portMAX_DELAY);
    xQueueReceive(video_pipe.free, &pixels, portMAX_DELAY);
    snes_set_framebuffer(pixels);
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

    uint16_t *spare_pixels = heap_caps_malloc(
        SNES_WIDTH * SNES_HEIGHT_EXTENDED * sizeof(uint16_t), MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (!spare_pixels || !video_pipe_init(spare_pixels)) {
        if (spare_pixels) {
            heap_caps_free(spare_pixels);
        }
        ESP_LOGW(TAG, "single-buffer synchronous display fallback");
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
        .refresh_rate = (int)rom_fps,
        .slots = SNES_REWIND_SLOTS,
        .save = rewind_save,
        .load = rewind_load,
        .preview = rewind_preview,
    };
    rewind_init(&rewind_backend);

    const TickType_t frame_delay = pdMS_TO_TICKS(1000 / rewind_backend.refresh_rate);
    emulator_settings_t settings = {0};
    int previous_buttons = 0;
    const int64_t frame_time_us = SNES_FRAME_TIME_US / rom_fps;
    const uint32_t frame_time_remainder = SNES_FRAME_TIME_US % rom_fps;
    uint32_t frame_time_phase = 0;
    uint32_t idle_frame_phase = 0;
    int64_t next_frame_us = esp_timer_get_time();

    while (1) {
        int buttons = buttons_read();
        int pressed = buttons & ~previous_buttons;
        snes_set_pad(pad_from_buttons(buttons));
        previous_buttons = buttons;
        emulator_settings_update(&settings, buttons, pressed);

        if (emulator_handle_state_controls(rom_path, &rewind_backend, buttons)) {
            vTaskDelay(frame_delay);
            next_frame_us = esp_timer_get_time();
            frame_time_phase = 0;
            continue;
        }

        /* Try to render every native 50/60 Hz frame. If a heavy scene falls
         * behind real time, skip only PPU output until the audio-paced core
         * catches up; emulation and sound always keep their native rate. */
        int64_t now = esp_timer_get_time();
        int64_t lag_us = now - next_frame_us;
        if (lag_us > frame_time_us * SNES_MAX_LAG_FRAMES) {
            next_frame_us = now;
            frame_time_phase = 0;
            lag_us = 0;
        }
        bool skip_video = lag_us > SNES_RENDER_LATE_US;

        frame_stats_begin();
        snes_set_video_skip(skip_video);
        snes_run_frame();
        snes_set_video_skip(false);
        frame_stats_end(!skip_video);

        next_frame_us += frame_time_us;
        frame_time_phase += frame_time_remainder;
        if (frame_time_phase >= rom_fps) {
            frame_time_phase -= rom_fps;
            next_frame_us++;
        }
        if (++idle_frame_phase >= rom_fps) {
            idle_frame_phase = 0;
            vTaskDelay(1); /* let CPU1's idle task feed its watchdog */
        }
    }
}
