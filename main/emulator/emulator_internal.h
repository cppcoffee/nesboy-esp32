#pragma once

#include <stddef.h>
#include <stdint.h>

#include "display.h"
#include "rewind.h"

typedef struct {
  int volume_repeat;
  int brightness_repeat;
} emulator_settings_t;

void emulator_settings_update(emulator_settings_t *settings, int buttons,
                              int pressed);
/* Handle save/load/rewind controls. Returns true when normal emulation should
 * be skipped. */
bool emulator_handle_state_controls(const char *rom_path,
                                    const rewind_backend_t *backend,
                                    int buttons);
/* Give an emulator the shared CPU0 display worker and a second framebuffer.
 * If allocation fails, presenting remains correct but synchronous. */
void emulator_video_start(size_t frame_size, uint32_t memory_caps,
                          display_fill_row_fn fill_row);
void *emulator_video_present(void *frame);
int emulator_nes_run(const char *rom_path);
int emulator_gb_run(const char *rom_path);
int emulator_sms_run(const char *rom_path);
int emulator_snes_run(const char *rom_path);
