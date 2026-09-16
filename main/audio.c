#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "pins.h"

#include "app_config.h"
#include "audio.h"
#include "frame_stats.h"

enum {
    AUDIO_MAX_SAMPLES_PER_FRAME = AUDIO_RATE / 50 + 2,
    AUDIO_DMA_DESC_NUM = 8,
    AUDIO_DMA_FRAME_NUM = 240,
    AUDIO_TASK_PRIORITY = 4,
    AUDIO_VOLUME_Q8 = (AUDIO_VOLUME_PERCENT * 256 + 50) / 100,
};

typedef struct {
    int samples;
    int16_t data[AUDIO_MAX_SAMPLES_PER_FRAME * 2];
} audio_frame_t;

static const char *TAG = "audio";

struct audio_state {
    i2s_chan_handle_t i2s_tx;
    QueueHandle_t free_queue;
    QueueHandle_t ready_queue;
    audio_frame_t frames[2];
    int volume_pct;
};

static struct audio_state au;

static void audio_task(void *arg)
{
    (void)arg;

    while (1) {
        audio_frame_t *frame;
        xQueueReceive(au.ready_queue, &frame, portMAX_DELAY);

        size_t frame_bytes = (size_t)frame->samples * 2 * sizeof(int16_t);
        size_t written = 0;
        esp_err_t err = i2s_channel_write(au.i2s_tx, frame->data, frame_bytes, &written, portMAX_DELAY);
        if (err != ESP_OK || written != frame_bytes) {
            ESP_LOGE(TAG, "I2S write failed: %s, %u/%u bytes", esp_err_to_name(err), (unsigned)written,
                     (unsigned)frame_bytes);
        }
        xQueueSend(au.free_queue, &frame, portMAX_DELAY);
    }
}

void audio_init(void)
{
    au.volume_pct = AUDIO_VOLUME_PERCENT;

    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.dma_desc_num = AUDIO_DMA_DESC_NUM;
    chan.dma_frame_num = AUDIO_DMA_FRAME_NUM;
    chan.auto_clear_after_cb = true;
    chan.intr_priority = 2;
    ESP_ERROR_CHECK(i2s_new_channel(&chan, &au.i2s_tx, NULL));

    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE);
    clk_cfg.clk_src = I2S_CLK_SRC_PLL_240M;

    i2s_std_config_t cfg = {
        .clk_cfg = clk_cfg,
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg =
            {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = AMP_BCLK_PIN,
                .ws = AMP_WS_PIN,
                .dout = AMP_DOUT_PIN,
                .din = I2S_GPIO_UNUSED,
                .invert_flags = {0},
            },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(au.i2s_tx, &cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(au.i2s_tx));

    au.free_queue = xQueueCreate(2, sizeof(audio_frame_t *));
    au.ready_queue = xQueueCreate(2, sizeof(audio_frame_t *));
    if (!au.free_queue || !au.ready_queue) {
        ESP_LOGE(TAG, "audio queue setup failed");
        abort();
    }
    for (int i = 0; i < 2; i++) {
        audio_frame_t *frame = &au.frames[i];
        xQueueSend(au.free_queue, &frame, 0);
    }
    if (xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, AUDIO_TASK_PRIORITY, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "audio task setup failed");
        abort();
    }

    ESP_LOGI(TAG, "HT517 ready: rate=%dHz DMA=%dx%d bclk=%d ws=%d dout=%d volume=%d%%", AUDIO_RATE, AUDIO_DMA_DESC_NUM,
             AUDIO_DMA_FRAME_NUM, AMP_BCLK_PIN, AMP_WS_PIN, AMP_DOUT_PIN, AUDIO_VOLUME_PERCENT);
}

void audio_change_volume(int delta)
{
    int new = au.volume_pct + delta;
    if (new < 0) {
        new = 0;
    } else if (new > 100) {
        new = 100;
    }

    if (new != au.volume_pct) {
        au.volume_pct = new;
        ESP_LOGI(TAG, "volume=%d%%", au.volume_pct);
    }
}

int audio_get_volume(void)
{
    return au.volume_pct;
}

void audio_flush(void)
{
    audio_frame_t *frame;
    while (xQueueReceive(au.ready_queue, &frame, 0) == pdTRUE) {
        xQueueSend(au.free_queue, &frame, 0);
    }
}

static audio_frame_t *audio_acquire_frame(void)
{
    audio_frame_t *frame;
    frame_stats_audio_wait_begin();
    xQueueReceive(au.free_queue, &frame, portMAX_DELAY);
    frame_stats_audio_wait_end();
    return frame;
}

static void audio_write_frame(const int16_t *buf, int samples, bool stereo)
{
    static bool audio_seen[2];

    if (samples <= 0 || samples > AUDIO_MAX_SAMPLES_PER_FRAME) {
        ESP_LOGE(TAG, "invalid audio frame: %d samples", samples);
        return;
    }

    if (!audio_seen[stereo]) {
        int peak = 0;
        int values = samples * (stereo ? 2 : 1);
        for (int i = 0; i < values; i++) {
            int magnitude = buf[i] < 0 ? -buf[i] : buf[i];
            if (magnitude > peak) {
                peak = magnitude;
            }
        }
        if (peak > 0) {
            ESP_LOGI(TAG, "%s audio active: samples=%d input peak=%d volume=%d%%", stereo ? "GB" : "NES", samples, peak,
                     au.volume_pct);
            audio_seen[stereo] = true;
        }
    }

    audio_frame_t *frame = audio_acquire_frame();
    frame->samples = samples;
    int volume_q8 = (au.volume_pct * 256 + 50) / 100;
    for (int i = 0; i < samples; i++) {
        int left = buf[i * (stereo ? 2 : 1)];
        int right = stereo ? buf[i * 2 + 1] : left;
        frame->data[i * 2] = (int16_t)((left * volume_q8) >> 8);
        frame->data[i * 2 + 1] = (int16_t)((right * volume_q8) >> 8);
    }

    xQueueSend(au.ready_queue, &frame, portMAX_DELAY);
}

void audio_write(const int16_t *buf, int samples)
{
    audio_write_frame(buf, samples, false);
}

void audio_write_stereo(const int16_t *buf, int samples)
{
    audio_write_frame(buf, samples, true);
}
