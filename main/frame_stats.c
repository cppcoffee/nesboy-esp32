#include "frame_stats.h"

#if NES_ENABLE_FRAME_STATS
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
enum { FRAME_STATS_LOG_FRAMES = 600 };

static const char *TAG = "frame-stats";

struct frame_stats_state {
    uint64_t frames;
    int64_t fps_timer;
    int64_t frame_start;
    int64_t audio_start;
    int64_t emulate_us;
    int64_t emulate_max_us;
    int64_t audio_wait_us;
};

static struct frame_stats_state fs;

void frame_stats_begin(void)
{
    fs.frame_start = esp_timer_get_time();
    if (fs.fps_timer == 0) {
        fs.fps_timer = fs.frame_start;
    }
}

void frame_stats_emulation_done(void)
{
    fs.audio_start = esp_timer_get_time();
}

void frame_stats_end(void)
{
    int64_t frame_end = esp_timer_get_time();
    int64_t frame_emulate_us = fs.audio_start - fs.frame_start;
    fs.emulate_us += frame_emulate_us;
    fs.audio_wait_us += frame_end - fs.audio_start;
    if (frame_emulate_us > fs.emulate_max_us) {
        fs.emulate_max_us = frame_emulate_us;
    }

    if ((++fs.frames % FRAME_STATS_LOG_FRAMES) == 0) {
        ESP_LOGI(TAG, "fps=%.1f emulate=%.2fms max=%.2fms audio_wait=%.2fms",
                 FRAME_STATS_LOG_FRAMES * 1000000.0f / (frame_end - fs.fps_timer),
                 fs.emulate_us / (FRAME_STATS_LOG_FRAMES * 1000.0f), fs.emulate_max_us / 1000.0f,
                 fs.audio_wait_us / (FRAME_STATS_LOG_FRAMES * 1000.0f));
        fs.fps_timer = frame_end;
        memset(&fs.emulate_us, 0, sizeof(fs.emulate_us) * 3);
    }
}
#endif
