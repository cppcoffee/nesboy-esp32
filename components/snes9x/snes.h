/* Snes9x wrapper API for nesboy-esp32. See snes9x.c. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "src/snes9x.h" /* SNES_WIDTH, SNES_HEIGHT_EXTENDED, SNES_*_MASK */

#define SNES_AUDIO_RATE_DEFAULT 32000
#define SNES_ROM_MAX_FILE_SIZE  (6u * 1024u * 1024u + 512u)

int snes_init(int audio_rate, size_t rom_file_size,
              void (*video_cb)(void *pixels),
              void (*audio_cb)(const int16_t *samples, int frames));

/* 256x224 RGB565 frame buffer (uint16_t), pitch = 256 pixels. */
void snes_set_framebuffer(uint16_t *pixels);

/* 50 (PAL) or 60 (NTSC) depending on the loaded ROM. */
uint32_t snes_rom_frames_per_second(void);

int snes_load_rom_file(const char *rom_path);

/* SNES_*_MASK button bits. */
void snes_set_pad(uint32_t buttons);

/* Skip PPU rendering for subsequent frames (audio unaffected). */
void snes_set_video_skip(bool skip);

/* Run one emulated frame. */
void snes_run_frame(void);

void snes_reset(void);

/* Save states. snes_state_size() is an upper bound; snes_save_state_mem
 * writes the exact size. */
size_t snes_state_size(void);
int snes_save_state_mem(uint8_t *buffer, size_t *written);
int snes_load_state_mem(const uint8_t *buffer, size_t size);
