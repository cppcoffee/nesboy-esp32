#pragma once

typedef struct emulator emulator_t;

/* Find the emulator registered for a ROM extension, or NULL if unsupported. */
const emulator_t *emulator_find(const char *rom_path);

/* Run the selected emulator. It owns the game loop and returns only on failure. */
int emulator_run(const emulator_t *emulator, const char *rom_path);
