#include "emulator.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "audio.h"
#include "buttons.h"
#include "display.h"
#include "emulator_internal.h"
#include "savestate.h"

static const char *TAG = "emulator";

enum {
  VIDEO_TASK_STACK = 4096,
  VIDEO_TASK_PRIORITY = 3, /* audio on CPU0 remains higher at 4 */
};

static struct {
  QueueHandle_t ready;
  QueueHandle_t free;
  display_fill_row_fn fill_row;
  bool running;
} video;

static void video_task(void *arg) {
  (void)arg;

  while (1) {
    void *frame;
    xQueueReceive(video.ready, &frame, portMAX_DELAY);
    display_blit(frame, video.fill_row);
    xQueueSend(video.free, &frame, portMAX_DELAY);
  }
}

struct emulator {
  const char *const *extensions;
  int (*run)(const char *rom_path);
};

static const char *const nes_extensions[] = {".nes", NULL};
static const char *const gb_extensions[] = {".gb", ".gbc", NULL};
static const char *const snes_extensions[] = {".sfc", ".smc", ".swc", ".fig",
                                              NULL};
static const char *const sms_extensions[] = {".sms", ".gg", NULL};

static const emulator_t emulators[] = {
    {.extensions = nes_extensions, .run = emulator_nes_run},
    {.extensions = gb_extensions, .run = emulator_gb_run},
    {.extensions = snes_extensions, .run = emulator_snes_run},
    {.extensions = sms_extensions, .run = emulator_sms_run},
};

const emulator_t *emulator_find(const char *rom_path) {
  if (!rom_path) {
    return NULL;
  }
  const char *extension = strrchr(rom_path, '.');
  if (!extension) {
    return NULL;
  }

  for (size_t i = 0; i < sizeof(emulators) / sizeof(emulators[0]); i++) {
    for (const char *const *candidate = emulators[i].extensions; *candidate;
         candidate++) {
      if (strcasecmp(extension, *candidate) == 0) {
        return &emulators[i];
      }
    }
  }
  return NULL;
}

int emulator_run(const emulator_t *emulator, const char *rom_path) {
  return emulator && rom_path ? emulator->run(rom_path) : -1;
}

void emulator_video_start(size_t frame_size, uint32_t memory_caps,
                          display_fill_row_fn fill_row) {
  video.fill_row = fill_row;
  bool spare_in_psram = (memory_caps & MALLOC_CAP_SPIRAM) != 0;
  void *spare_frame = heap_caps_malloc(frame_size, memory_caps);
  if (!spare_frame && (memory_caps & MALLOC_CAP_INTERNAL)) {
    /* Large internal-RAM frames (SMS is 96 KB) may not fit twice; a PSRAM
     * spare only costs fill_row some PSRAM read latency. */
    spare_frame =
        heap_caps_malloc(frame_size, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (spare_frame) {
      spare_in_psram = true;
      ESP_LOGW(TAG, "spare framebuffer fell back to PSRAM");
    }
  }
  if (!spare_frame) {
    ESP_LOGW(TAG, "single-buffer synchronous display fallback");
    return;
  }

  video.ready = xQueueCreate(1, sizeof(void *));
  video.free = xQueueCreate(1, sizeof(void *));
  if (!video.ready || !video.free ||
      xTaskCreatePinnedToCore(video_task, "emulator-video", VIDEO_TASK_STACK,
                              NULL, VIDEO_TASK_PRIORITY, NULL, 0) != pdPASS) {
    if (video.ready) {
      vQueueDelete(video.ready);
    }
    if (video.free) {
      vQueueDelete(video.free);
    }
    video.ready = NULL;
    video.free = NULL;
    heap_caps_free(spare_frame);
    ESP_LOGW(TAG, "single-buffer synchronous display fallback");
    return;
  }

  xQueueSend(video.free, &spare_frame, 0);
  video.running = true;
  ESP_LOGI(
      TAG, "async video: frame=%u spare=%s internal_free=%u largest=%u",
      (unsigned)frame_size, spare_in_psram ? "PSRAM" : "internal",
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                 MALLOC_CAP_8BIT));
}

void *emulator_video_present(void *frame) {
  if (!frame || !video.fill_row) {
    return frame;
  }
  if (!video.running) {
    display_blit(frame, video.fill_row);
    return frame;
  }

  xQueueSend(video.ready, &frame, portMAX_DELAY);
  xQueueReceive(video.free, &frame, portMAX_DELAY);
  return frame;
}

bool emulator_handle_state_controls(const char *rom_path,
                                    const rewind_backend_t *backend,
                                    int buttons) {
  rewind_action_t action =
      rewind_frame(savestate_handle_buttons(rom_path, backend, buttons));
  if (action == REWIND_ACTION_NORMAL) {
    return false;
  }

  audio_flush();
  if (action == REWIND_ACTION_STEP) {
    rewind_redraw();
  }
  return true;
}

void emulator_settings_update(emulator_settings_t *settings, int buttons,
                              int pressed) {
  if (!(buttons & NES_PAD_SELECT)) {
    settings->volume_repeat = 0;
    settings->brightness_repeat = 0;
    return;
  }

  if (buttons & NES_PAD_UP) {
    if ((pressed & NES_PAD_UP) || settings->volume_repeat == 0) {
      audio_change_volume(1);
      display_osd_show(audio_get_volume(), display_get_brightness());
      settings->volume_repeat = (pressed & NES_PAD_UP) ? 20 : 6;
    }
    settings->volume_repeat--;
  } else if (buttons & NES_PAD_DOWN) {
    if ((pressed & NES_PAD_DOWN) || settings->volume_repeat == 0) {
      audio_change_volume(-1);
      display_osd_show(audio_get_volume(), display_get_brightness());
      settings->volume_repeat = (pressed & NES_PAD_DOWN) ? 20 : 6;
    }
    settings->volume_repeat--;
  } else {
    settings->volume_repeat = 0;
  }

  if (buttons & NES_PAD_LEFT) {
    if ((pressed & NES_PAD_LEFT) || settings->brightness_repeat == 0) {
      display_set_brightness(-2);
      display_osd_show(audio_get_volume(), display_get_brightness());
      settings->brightness_repeat = (pressed & NES_PAD_LEFT) ? 20 : 6;
    }
    settings->brightness_repeat--;
  } else if (buttons & NES_PAD_RIGHT) {
    if ((pressed & NES_PAD_RIGHT) || settings->brightness_repeat == 0) {
      display_set_brightness(2);
      display_osd_show(audio_get_volume(), display_get_brightness());
      settings->brightness_repeat = (pressed & NES_PAD_RIGHT) ? 20 : 6;
    }
    settings->brightness_repeat--;
  } else {
    settings->brightness_repeat = 0;
  }
}
