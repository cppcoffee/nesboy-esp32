#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Minimal Sega Master System / Game Gear emulator core.
 *
 * Implements the Z80 CPU, VDP mode 4 (the mode virtually all commercial
 * SMS/GG games use), the SN76489 PSG, and the common cartridge mappers
 * (Sega 8/16/32 KB paging, Codemasters). SMS BIOS is not required; games
 * boot directly from their entry point like most emulators do.
 *
 * Video output is RGB565 (little endian) into a caller-provided framebuffer:
 *   - SMS:  256x192
 *   - GG:   160x144
 * Audio output is interleaved stereo int16 at the requested sample rate.
 */

#define SMS_WIDTH  (256)
#define SMS_HEIGHT (192)
#define GG_WIDTH   (160)
#define GG_HEIGHT  (144)

typedef enum {
    SMS_HW_SMS,
    SMS_HW_GG,
} sms_hw_t;

typedef enum {
    SMS_PAD_UP = 0x01,
    SMS_PAD_DOWN = 0x02,
    SMS_PAD_LEFT = 0x04,
    SMS_PAD_RIGHT = 0x08,
    SMS_PAD_B1 = 0x10, /* trigger 1 / GG start */
    SMS_PAD_B2 = 0x20, /* trigger 2 */
} sms_pad_t;

typedef void (*sms_video_cb_t)(void *buffer);
typedef void (*sms_audio_cb_t)(const int16_t *buffer, int samples);

/* Configure the core. Call once before loading a ROM. hw selects the
 * console variant (screen size + I/O map details). */
int sms_init(sms_hw_t hw, int sample_rate, sms_video_cb_t video_cb, sms_audio_cb_t audio_cb);

/* Provide the RGB565 frame buffer (SMS_WIDTH*SMS_HEIGHT or GG_* pixels) and
 * the per-frame audio buffer (capacity in int16 samples, stereo pairs). */
void sms_set_framebuffer(uint16_t *buffer);
void sms_set_soundbuffer(int16_t *buffer, size_t capacity);

/* Load a .sms/.gg ROM image (whole file, up to ~1 MB). Returns 0 on success. */
int sms_load_rom(const uint8_t *data, size_t size);
void sms_free_rom(void);

/* Reset the machine and start execution. */
void sms_reset(void);

/* Emulate exactly one video frame. Produces one video callback and one
 * audio callback with all samples generated during the frame. */
void sms_run_frame(void);

/* Update the controller state (bitmask of sms_pad_t). */
void sms_set_pad(int pad);

/* In-memory save states for rewind/savestate. The buffer must be at least
 * sms_state_size() bytes. Save returns bytes written; load returns 0. */
size_t sms_state_size(void);
int sms_save_state_mem(void *buffer, size_t size);
int sms_load_state_mem(const void *buffer, size_t size);

/* Battery RAM persistence: returns true when the cart has battery-backed
 * RAM that changed since the last sync. */
bool sms_sram_dirty(void);
int sms_load_sram(const char *file);
int sms_save_sram(const char *file);
