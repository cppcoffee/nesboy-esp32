#pragma once

#include "rewind.h"

typedef struct {
    int volume_repeat;
    int brightness_repeat;
} emulator_settings_t;

void emulator_settings_update(emulator_settings_t *settings, int buttons, int pressed);
/* Handle save/load/rewind controls. Returns true when normal emulation should be skipped. */
bool emulator_handle_state_controls(const char *rom_path, const rewind_backend_t *backend, int buttons);
int emulator_nes_run(const char *rom_path);
int emulator_gb_run(const char *rom_path);
