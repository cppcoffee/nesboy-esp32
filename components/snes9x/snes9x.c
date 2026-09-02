/* Snes9x wrapper for nesboy-esp32.
 * Based on Snes9x 1.52-ish core as adapted by retro-go - Snes9x license (see src/LICENSE).
 *
 * The core runs one full video frame per snes_run_frame() call: S9xMainLoop()
 * exits when the emulated frame completes (SCAN_KEYS_FLAG). Audio is mixed
 * from SoundData after the frame and delivered through the audio callback. */

#include "snes9x.h"
#include "src/snes9x.h"
#include "src/memmap.h"
#include "src/cpuexec.h"
#include "src/apu.h"
#include "src/display.h"
#include "src/gfx.h"
#include "src/soundux.h"
#include "src/snapshot.h"
#include "src/srtc.h"

#include <string.h>

static void (*frame_video_cb)(void *pixels);
static void (*frame_audio_cb)(const int16_t *samples, int frames);

static int16_t *audio_buffer;
static uint32_t audio_buffer_samples;
static uint32_t audio_frame_remainder;
static uint32_t audio_fps;

/* Frame skipping controlled by the port: when set, the PPU output is not
 * rendered but the CPU/APU still run at full speed. */
static bool snes_skip_video;

uint32_t S9xReadJoypad(int32_t port)
{
    extern uint32_t snes_joypad_state; /* defined below */
    (void)port;
    return port == 0 ? (snes_joypad_state | 0xffff0000) : 0;
}

bool S9xReadMousePosition(int32_t which1, int32_t *x, int32_t *y, uint32_t *buttons)
{
    (void)which1; (void)x; (void)y; (void)buttons;
    return false;
}

bool S9xReadSuperScopePosition(int32_t *x, int32_t *y, uint32_t *buttons)
{
    (void)x; (void)y; (void)buttons;
    return false;
}

bool JustifierOffscreen(void)
{
    return true;
}

void JustifierButtons(uint32_t *justifiers)
{
    (void)justifiers;
}

uint32_t snes_joypad_state = 0;

bool S9xInitDisplay(void)
{
    GFX.Pitch = SNES_WIDTH * 2;
    GFX.RealPitch = GFX.Pitch2 = GFX.Pitch;
    GFX.ZPitch = SNES_WIDTH;
    GFX.Screen = NULL; /* set by snes_set_framebuffer */
    GFX.SubScreen = snes_malloc(GFX.Pitch * SNES_HEIGHT_EXTENDED);
    GFX.ZBuffer = snes_malloc(GFX.ZPitch * SNES_HEIGHT_EXTENDED);
    GFX.SubZBuffer = snes_malloc(GFX.ZPitch * SNES_HEIGHT_EXTENDED);
    return GFX.SubScreen && GFX.ZBuffer && GFX.SubZBuffer;
}

void S9xDeinitDisplay(void)
{
    snes_free(GFX.SubScreen);   GFX.SubScreen = NULL;
    snes_free(GFX.ZBuffer);     GFX.ZBuffer = NULL;
    snes_free(GFX.SubZBuffer);  GFX.SubZBuffer = NULL;
}

/* The legacy SPC700 core accumulates DSP output while the emulated frame
 * runs; after each frame the port mixes exactly one frame worth of samples
 * (~533 stereo frames at 32 kHz / 60 fps) and hands them to the audio cb. */
static void snes_audio_flush(void)
{
    if (!frame_audio_cb || !audio_buffer) {
        return;
    }

    uint32_t fps = Memory.ROMFramesPerSecond == 50 ? 50 : 60;
    if (audio_fps != fps) {
        audio_fps = fps;
        audio_frame_remainder = 0;
    }

    uint32_t total = Settings.SoundPlaybackRate + audio_frame_remainder;
    uint32_t frames = total / fps;
    audio_frame_remainder = total % fps;
    int32_t wanted = (int32_t)(frames * 2); /* interleaved stereo samples */
    S9xMixSamples(audio_buffer, wanted);
    frame_audio_cb(audio_buffer, wanted >> 1);
}

int snes_init(int audio_rate,
              void (*video_cb)(void *pixels),
              void (*audio_cb)(const int16_t *samples, int frames))
{
    frame_video_cb = video_cb;
    frame_audio_cb = audio_cb;

    Settings.CyclesPercentage = 100;
    Settings.H_Max = SNES_CYCLES_PER_SCANLINE;
    Settings.FrameTimePAL = 20000;
    Settings.FrameTimeNTSC = 16667;
    Settings.ControllerOption = SNES_JOYPAD;
    Settings.HBlankStart = (256 * Settings.H_Max) / SNES_HCOUNTER_MAX;
    Settings.SoundPlaybackRate = audio_rate;
    Settings.SoundInputRate = 32040; /* true SPC DSP rate; core resamples */
    Settings.DisableSoundEcho = false;
    Settings.InterpolatedSound = true;
    Settings.SoundSync = false;
    Settings.ApplyCheats = false;

    if (!S9xInitDisplay()) {
        return -1;
    }
    if (!S9xInitMemory()) {
        return -1;
    }
    if (!S9xInitAPU()) {
        return -1;
    }
    if (!S9xInitSound(0, 0)) {
        return -1;
    }
    if (!S9xInitGFX()) {
        return -1;
    }

    /* One PAL frame is the largest audio block. */
    audio_buffer_samples = ((uint32_t)audio_rate + 49) / 50 * 2;
    audio_buffer = snes_malloc_fast(audio_buffer_samples * sizeof(int16_t));
    if (!audio_buffer) {
        return -1;
    }

    S9xSetPlaybackRate(audio_rate);
    return 0;
}

void snes_set_framebuffer(uint16_t *pixels)
{
    GFX.Screen = (uint8_t *)pixels;
}

uint32_t snes_rom_frames_per_second(void)
{
    return (uint32_t)Memory.ROMFramesPerSecond;
}

int snes_load_rom_file(const char *rom_path)
{
    return LoadROM(rom_path) ? 0 : -1;
}

void snes_set_pad(uint32_t buttons)
{
    snes_joypad_state = buttons;
}

void snes_set_video_skip(bool skip)
{
    snes_skip_video = skip;
}

void snes_run_frame(void)
{
    IPPU.RenderThisFrame = !snes_skip_video;
    S9xMainLoop();
    snes_audio_flush();
    if (frame_video_cb && !snes_skip_video && GFX.Screen) {
        frame_video_cb(GFX.Screen);
    }
}

void snes_reset(void)
{
    S9xReset();
}

/* --- Save states (in-memory) --- */

size_t snes_state_size(void)
{
    return 16 + sizeof(CPU) + sizeof(ICPU) + sizeof(PPU) + sizeof(DMA) +
           VRAM_SIZE + RAM_SIZE + SRAM_SIZE + FILLRAM_SIZE +
           sizeof(APU) + sizeof(IAPU) + 0x10000 + sizeof(SoundData) + 256;
}

int snes_save_state_mem(uint8_t *buffer, size_t *written)
{
    return S9xSaveStateMem(buffer, snes_state_size(), written) ? 0 : -1;
}

int snes_load_state_mem(const uint8_t *buffer, size_t size)
{
    return S9xLoadStateMem(buffer, size) ? 0 : -1;
}
