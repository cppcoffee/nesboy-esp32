#include "emulator_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

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
#include "sms.h"

static const char *TAG = "emulator-sms";
static bool rewind_previewing;

enum {
  SMS_AUDIO_MAX_SAMPLES = AUDIO_RATE / 50 + 2,
};

static sms_hw_t sms_hw;
static int sms_screen_w;
static int sms_screen_h;

/* SMS 256x192 -> 240x180 (15/16 decimation on both axes, centered) and
 * GG 160x144 -> 240x216 (1.5x, same geometry as GB). The core hands over
 * RGB565 rows of whichever framebuffer emulator_video_present() returned. */
static void fill_display_row(uint16_t *dst, const uint16_t *previous,
                             int screen_y, const void *frame) {
  if (sms_hw == SMS_HW_GG) {
    if (screen_y < 12 || screen_y >= 228) {
      memset(dst, 0, LCD_W * sizeof(*dst));
      return;
    }
    int visible_y = screen_y - 12;
    if ((visible_y % 3) == 1 && previous) {
      memcpy(dst, previous, LCD_W * sizeof(*dst));
      return;
    }
    const uint16_t *src =
        (const uint16_t *)frame + (size_t)visible_y * 2 / 3 * GG_WIDTH;
    for (int x = 0; x < GG_WIDTH; x += 2) {
      uint16_t a = *src++;
      uint16_t b = *src++;
      *dst++ = a;
      *dst++ = a;
      *dst++ = b;
    }
    return;
  }

  (void)previous;
  const int top_blank = (LCD_H - 180) / 2;
  if (screen_y < top_blank || screen_y >= top_blank + 180) {
    memset(dst, 0, LCD_W * sizeof(*dst));
    return;
  }
  const uint16_t *src = (const uint16_t *)frame +
                        (size_t)(screen_y - top_blank) * 16 / 15 * SMS_WIDTH;
  for (int group = 0; group < SMS_WIDTH / 16; group++) {
    memcpy(dst, src, 15 * sizeof(*dst));
    dst += 15;
    src += 16;
  }
}

static void video_callback(void *buffer) {
  sms_set_framebuffer(emulator_video_present(buffer));
}

static void audio_callback(const int16_t *buffer, int samples) {
  if (!rewind_previewing) {
    audio_write_stereo(buffer, samples);
  }
}

static int pad_from_buttons(int buttons) {
  int pad = 0;
  if (buttons & NES_PAD_RIGHT) {
    pad |= SMS_PAD_RIGHT;
  }
  if (buttons & NES_PAD_LEFT) {
    pad |= SMS_PAD_LEFT;
  }
  if (buttons & NES_PAD_UP) {
    pad |= SMS_PAD_UP;
  }
  if (buttons & NES_PAD_DOWN) {
    pad |= SMS_PAD_DOWN;
  }
  if (buttons & NES_PAD_B) {
    pad |= SMS_PAD_B1;
  }
  if (buttons & NES_PAD_A) {
    pad |= SMS_PAD_B2;
  }
  if (buttons & NES_PAD_START) {
    pad |= SMS_PAD_B1; /* GG start maps to trigger 1 */
  }
  return pad;
}

static int rewind_save(uint8_t *buffer) {
  return sms_save_state_mem(buffer, sms_state_size());
}

static int rewind_load(const uint8_t *buffer) {
  return sms_load_state_mem(buffer, sms_state_size());
}

static void rewind_preview(void) {
  rewind_previewing = true;
  sms_run_frame();
  rewind_previewing = false;
}

int emulator_sms_run(const char *rom_path) {
  const char *extension = strrchr(rom_path, '.');
  bool game_gear = extension && strcasecmp(extension, ".gg") == 0;
  sms_hw = game_gear ? SMS_HW_GG : SMS_HW_SMS;
  sms_screen_w = game_gear ? GG_WIDTH : SMS_WIDTH;
  sms_screen_h = game_gear ? GG_HEIGHT : SMS_HEIGHT;

  uint16_t *pixels =
      heap_caps_malloc(sms_screen_w * sms_screen_h * sizeof(uint16_t),
                       MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  int16_t *sound = heap_caps_malloc(SMS_AUDIO_MAX_SAMPLES * 2 * sizeof(int16_t),
                                    MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  if (!pixels || !sound) {
    ESP_LOGE(TAG, "out of memory: SMS buffers");
    return -1;
  }

  if (sms_init(sms_hw, AUDIO_RATE, video_callback, audio_callback) < 0) {
    ESP_LOGE(TAG, "sms_init failed");
    return -1;
  }

  sms_set_framebuffer(pixels);
  sms_set_soundbuffer(sound, SMS_AUDIO_MAX_SAMPLES * 2);

  FILE *file = fopen(rom_path, "rb");
  if (!file) {
    ESP_LOGE(TAG, "fopen(%s) failed", rom_path);
    return -1;
  }
  fseek(file, 0, SEEK_END);
  long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  if (size < 0x400 || size > 0x200000) {
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

  if (sms_load_rom(data, (size_t)size) < 0) {
    ESP_LOGE(TAG, "sms_load_rom failed");
    free(data);
    return -1;
  }
  free(data);

  sms_reset();

  emulator_video_start(sms_screen_w * sms_screen_h * sizeof(uint16_t),
                       MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL, fill_display_row);

  const rewind_backend_t rewind_backend = {
      .state_size = sms_state_size(),
      .refresh_rate = 60,
      .slots = SMS_REWIND_SLOTS,
      .save = rewind_save,
      .load = rewind_load,
      .preview = rewind_preview,
  };
  rewind_init(&rewind_backend);

  const TickType_t frame_delay =
      pdMS_TO_TICKS(1000 / rewind_backend.refresh_rate);
  emulator_settings_t settings = {0};
  int previous_buttons = 0;

  while (1) {
    int buttons = buttons_read();
    int pressed = buttons & ~previous_buttons;
    sms_set_pad(pad_from_buttons(buttons));
    previous_buttons = buttons;
    emulator_settings_update(&settings, buttons, pressed);

    if (emulator_handle_state_controls(rom_path, &rewind_backend, buttons)) {
      vTaskDelay(frame_delay);
      continue;
    }

    frame_stats_begin();
    sms_run_frame();
    frame_stats_end(true);
  }
}
