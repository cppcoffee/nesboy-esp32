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
    int64_t audio_wait_start;
    int64_t frame_audio_wait_us;
    int64_t emulate_us;
    int64_t emulate_max_us;
    int64_t audio_wait_us;
};

static struct frame_stats_state fs;

void frame_stats_begin(void)
{
    fs.frame_start = esp_timer_get_time();
    fs.frame_audio_wait_us = 0;
    fs.audio_wait_start = 0;
    if (fs.fps_timer == 0) {
        fs.fps_timer = fs.frame_start;
    }
}

void frame_stats_audio_wait_begin(void)
{
    fs.audio_wait_start = esp_timer_get_time();
}

void frame_stats_audio_wait_end(void)
{
    if (fs.audio_wait_start != 0) {
        fs.frame_audio_wait_us += esp_timer_get_time() - fs.audio_wait_start;
        fs.audio_wait_start = 0;
    }
}

void frame_stats_end(void)
{
    int64_t frame_end = esp_timer_get_time();
    int64_t frame_emulate_us = frame_end - fs.frame_start - fs.frame_audio_wait_us;
    if (frame_emulate_us < 0) {
        frame_emulate_us = 0;
    }
    fs.emulate_us += frame_emulate_us;
    fs.audio_wait_us += fs.frame_audio_wait_us;
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
