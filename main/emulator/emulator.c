#include "emulator.h"

#include <stddef.h>
#include <string.h>
#include <strings.h>

#include "audio.h"
#include "buttons.h"
#include "display.h"
#include "emulator_internal.h"
#include "savestate.h"

struct emulator {
    const char *const *extensions;
    int (*run)(const char *rom_path);
};

static const char *const nes_extensions[] = {".nes", NULL};
static const char *const gb_extensions[] = {".gb", ".gbc", NULL};
static const char *const snes_extensions[] = {".sfc", ".smc", ".swc", ".fig", NULL};

static const emulator_t emulators[] = {
    {.extensions = nes_extensions, .run = emulator_nes_run},
    {.extensions = gb_extensions, .run = emulator_gb_run},
    {.extensions = snes_extensions, .run = emulator_snes_run},
};

const emulator_t *emulator_find(const char *rom_path)
{
    if (!rom_path) {
        return NULL;
    }
    const char *extension = strrchr(rom_path, '.');
    if (!extension) {
        return NULL;
    }

    for (size_t i = 0; i < sizeof(emulators) / sizeof(emulators[0]); i++) {
        for (const char *const *candidate = emulators[i].extensions; *candidate; candidate++) {
            if (strcasecmp(extension, *candidate) == 0) {
                return &emulators[i];
            }
        }
    }
    return NULL;
}

int emulator_run(const emulator_t *emulator, const char *rom_path)
{
    return emulator && rom_path ? emulator->run(rom_path) : -1;
}

bool emulator_handle_state_controls(const char *rom_path, const rewind_backend_t *backend, int buttons)
{
    rewind_action_t action = rewind_frame(savestate_handle_buttons(rom_path, backend, buttons));
    if (action == REWIND_ACTION_NORMAL) {
        return false;
    }

    audio_flush();
    if (action == REWIND_ACTION_STEP) {
        rewind_redraw();
    }
    return true;
}

void emulator_settings_update(emulator_settings_t *settings, int buttons, int pressed)
{
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
