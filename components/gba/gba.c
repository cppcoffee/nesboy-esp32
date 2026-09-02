/* gpSP GBA emulator wrapper for nesboy-esp32.
 * Based on gpSP (https://github.com/notaz/gpsp) - GPL v2.
 */

#include "gba.h"

#include <stdio.h>
#include <string.h>

#include "common.h"
#include "gba_cc_lut.h"

static void (*frame_video_cb)(void *pixels);
static void (*frame_audio_cb)(const int16_t *samples, int frames);

u32 skip_next_frame = 0;

boot_mode selected_boot_mode = boot_game;
int sprite_limit = 1;

u32 idle_loop_target_pc = 0xFFFFFFFF;
u32 translation_gate_target_pc[MAX_TRANSLATION_GATES];
u32 translation_gate_targets = 0;

static int16_t *sound_buffer_out;
static size_t sound_capacity;
static int audio_output_rate;

int gba_init(int audio_rate, gba_audio_format_t audio_format,
             gba_pixel_format_t pixel_format,
             void (*video_cb)(void *pixels),
             void (*audio_cb)(const int16_t *samples, int frames))
{
    (void)audio_format;
    (void)pixel_format;
    frame_video_cb = video_cb;
    frame_audio_cb = audio_cb;
    sound_buffer_out = NULL;
    sound_capacity = 0;
    audio_output_rate = audio_rate;

    /* Frame memory (VRAM/EWRAM/IWRAM/backup) must be allocated before the
     * core touches any of it. */
    if (!gbsp_memory_init()) {
        return -1;
    }

    /* No saved input state, boot straight into the game. */
    selected_boot_mode = boot_game;
    sprite_limit = 1;
    sound_master_enable = true;

    init_gamepak_buffer();
    init_sound();
    init_cpu();
    update_input();
    return 0;
}

void gba_set_framebuffer(uint16_t *pixels)
{
    gba_screen_pixels = pixels;
}

void gba_set_soundbuffer(int16_t *sound, size_t sound_capacity_samples)
{
    sound_buffer_out = sound;
    sound_capacity = sound_capacity_samples;
}

int gba_load_rom_file(const char *rom_path)
{
    if (load_gamepak(rom_path, FEAT_DISABLE, FEAT_DISABLE, SERIAL_MODE_DISABLED) != 0) {
        return -1;
    }
    return 0;
}

void gba_set_pad(uint32_t buttons)
{
    gba_button_state = buttons;
}

void gba_run_frame(void)
{
    clear_gamepak_stickybits();
    rumble_frame_reset();
    update_input();
    execute_arm(execute_cycles);

    if (frame_audio_cb && sound_buffer_out && sound_capacity) {
        u32 frames = sound_read_samples(sound_buffer_out, sound_capacity / 2);
        if (frames) {
            frame_audio_cb(sound_buffer_out, (int)frames);
        }
    }

    if (frame_video_cb && !skip_next_frame) {
        frame_video_cb(gba_screen_pixels);
    }
}

void gba_set_video_skip(bool skip)
{
    skip_next_frame = skip ? 1 : 0;
}

void gba_reset(bool hard)
{
    (void)hard;
    reset_gba();
}

uint8_t *gba_backup_ram(void)
{
    return gamepak_backup;
}

size_t gba_backup_ram_size(void)
{
    /* Largest possible save type (128 KiB flash). */
    return 1024 * 128;
}

size_t gba_state_size(void)
{
    return GBA_STATE_MEM_SIZE;
}

int gba_save_state_mem(uint8_t *buffer)
{
    gba_save_state(buffer);
    return 0;
}

int gba_load_state_mem(const uint8_t *buffer)
{
    return gba_load_state(buffer) ? 0 : -1;
}
