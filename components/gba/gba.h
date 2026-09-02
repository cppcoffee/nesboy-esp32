/* gpSP GBA emulator wrapper for nesboy-esp32.
 * Based on gpSP (https://github.com/notaz/gpsp) - GPL v2.
 *
 * The core runs one full video frame per gba_run_frame() call. The caller
 * provides the frame buffer, button state and an optional audio callback. */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GBA_WIDTH  240
#define GBA_HEIGHT 160

/* Audio the core natively renders at. */
#define GBA_AUDIO_RATE 32768

typedef enum {
    GBA_PIXEL_565_LE,
} gba_pixel_format_t;

typedef enum {
    GBA_AUDIO_STEREO_S16,
} gba_audio_format_t;

/* video_cb is called once per frame with a 240x160 RGB565 image.
 * audio_cb is called once per frame with interleaved stereo s16 samples
 * (frames * 2 int16 values, 32768 Hz). */
int gba_init(int audio_rate, gba_audio_format_t audio_format,
             gba_pixel_format_t pixel_format,
             void (*video_cb)(void *pixels),
             void (*audio_cb)(const int16_t *samples, int frames));

/* Provide the core's frame buffer (GBA_WIDTH * GBA_HEIGHT u16, plus one
 * spare row) and sound buffer (capacity in int16 samples). */
void gba_set_framebuffer(uint16_t *pixels);
void gba_set_soundbuffer(int16_t *sound, size_t sound_capacity_samples);

/* Load a ROM from a file path (1 to 32 MB). Must be called before reset. */
int gba_load_rom_file(const char *rom_path);

/* Button mask made of input_buttons_type bits (BUTTON_A, BUTTON_B, ...). */
void gba_set_pad(uint32_t buttons);

/* Run one frame: emulation, video and audio callbacks. */
void gba_run_frame(void);

/* Skip the video callback (but keep audio) for subsequent frames. */
void gba_set_video_skip(bool skip);

void gba_reset(bool hard);

/* Battery-backed cart RAM (SRAM/flash/EEPROM) persistence. The buffer is
 * up to 64 KiB (gamepak_backup, 128 KiB allocated). */
uint8_t *gba_backup_ram(void);
size_t gba_backup_ram_size(void);

/* Save states. state_size() returns the exact snapshot size (416 KiB).
 * save/load take that many bytes. */
size_t gba_state_size(void);
int gba_save_state_mem(uint8_t *buffer);
int gba_load_state_mem(const uint8_t *buffer);
