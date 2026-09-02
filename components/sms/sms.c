/* SMS/GG core top level: init, ROM loading, frame loop, save states. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
#else
/* host build stubs for syntax checking */
#define ESP_LOGE(...)
#define ESP_LOGI(...)
#define heap_caps_malloc(size, caps) malloc(size)
#define MALLOC_CAP_SPIRAM            0
#define MALLOC_CAP_8BIT              0
#endif

#include "mem.h"
#include "psg.h"
#include "sms_internal.h"
#include "vdp.h"
#include "z80.h"

static const char *TAG = "sms";

struct sms_state sms;

#define CYCLES_PER_FRAME (262 * 228)

int sms_init(sms_hw_t hw, int sample_rate, sms_video_cb_t video_cb, sms_audio_cb_t audio_cb)
{
    memset(&sms, 0, sizeof(sms));
    sms.hw = hw;
    sms.sample_rate = sample_rate;
    sms.video_cb = video_cb;
    sms.audio_cb = audio_cb;
    return 0;
}

void sms_set_framebuffer(uint16_t *buffer)
{
    sms.framebuffer = buffer;
}

void sms_set_soundbuffer(int16_t *buffer, size_t capacity)
{
    psg_set_buffer(buffer, capacity);
}

int sms_load_rom(const uint8_t *data, size_t size)
{
    if (!data || size < 0x400) {
        return -1;
    }

    /* Round up to a whole number of 8 KB pages so page masking works. */
    size_t pages = (size + 0x1FFF) & ~((size_t)0x1FFF);
    uint8_t *rom = heap_caps_malloc(pages, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rom) {
        rom = malloc(pages);
    }
    if (!rom) {
        ESP_LOGE(TAG, "no memory for %u-byte ROM", (unsigned)pages);
        return -2;
    }
    memcpy(rom, data, size);
    memset(rom + size, 0xFF, pages - size);

    sms.rom = rom;
    sms.rom_size = pages;

    /* Mapper detection: TMR SEGA header or Codemasters signature. */
    if (size >= 0x7FF0 && memcmp(rom + 0x7FF0, "TMR SEGA", 8) == 0) {
        sms.mapper = MAPPER_SEGA;
    } else if (memcmp(rom + 0x100, "COPYRIGHT BY", 12) == 0 || memcmp(rom + 0x200, "CODEMASTERS", 11) == 0) {
        sms.mapper = MAPPER_CODEMASTERS;
    } else if (size > 48 * 1024) {
        sms.mapper = MAPPER_SEGA;
    } else {
        sms.mapper = MAPPER_NONE;
    }

    /* Battery RAM: some Sega-mapper games use the 0x8000 slot SRAM. */
    static const struct {
        const char *name;
        size_t sram_size;
    } sram_games[] = {
        {"Y's", 0x8000},        {"GOLVELLIOUS", 0x8000},      {"PHANTASY STAR", 0x8000},
        {"WONDER BOY", 0x8000}, {"MIRACLE WARRIORS", 0x8000},
    };
    char title[17];
    memcpy(title, rom + 0x7FF0, 16);
    title[16] = '\0';
    for (size_t i = 0; i < sizeof(sram_games) / sizeof(sram_games[0]); i++) {
        if (strncmp(title, sram_games[i].name, strlen(sram_games[i].name)) == 0) {
            sms.sram_size = sram_games[i].sram_size;
            break;
        }
    }
    if (sms.sram_size) {
        sms.sram = calloc(1, sms.sram_size);
    }

    ESP_LOGI(TAG, "ROM loaded: %.16s (%u KB, mapper=%d)", title, (unsigned)(pages / 1024), sms.mapper);
    return 0;
}

void sms_free_rom(void)
{
    free(sms.rom);
    free(sms.sram);
    sms.rom = NULL;
    sms.sram = NULL;
    sms.rom_size = 0;
    sms.sram_size = 0;
}

void sms_reset(void)
{
    memset(sms.ram, 0, sizeof(sms.ram));
    sms.page[0] = 0;
    sms.page[1] = 1;
    sms.page[2] = 2;
    sms.sram_enabled = false;
    sms.sram_bank = 0;
    sms.frame = 0;
    vdp_reset();
    psg_reset();
    sms_z80_reset();
}

void sms_set_pad(int pad)
{
    sms.pad = pad;
}

void sms_run_frame(void)
{
    int cycles = 0;
    while (cycles < CYCLES_PER_FRAME) {
        int chunk = CYCLES_PER_FRAME - cycles;
        if (chunk > 2000) {
            chunk = 2000;
        }
        int ran = sms_z80_run(chunk);
        cycles += ran;
        vdp_tick(ran);
        psg_tick(ran);
    }
    vdp_end_frame();
    sms.frame++;

    if (sms.video_cb && sms.framebuffer) {
        sms.video_cb(sms.framebuffer);
    }
    if (sms.audio_cb) {
        int samples = (int)(psg_samples_ready() / 2);
        if (samples > 0) {
            sms.audio_cb(psg_buffer(), samples);
        }
    }
}

/* ---- Save states ----
 *
 * Layout: header (regs + mapper state), Z80 regs, RAM, VRAM, CRAM,
 * SAT shadow, PSG state, SRAM.
 */

struct sms_save_header {
    uint32_t magic; /* 'SMSs' */
    uint32_t version;
    uint32_t ram_crc_placeholder;
    uint8_t page[3];
    uint8_t flags; /* bit0: sram_enabled, bit1: sram_bank */
};

#define SAVE_MAGIC   0x73534D53u /* little-endian 'SMss' */
#define SAVE_VERSION 1

size_t sms_state_size(void)
{
    return sizeof(struct sms_save_header) + sizeof(z80) + sizeof(sms.ram) + 0x4000 + 32 + 64 + psg_state_size() +
           sms.sram_size;
}

int sms_save_state_mem(void *buffer, size_t size)
{
    size_t expected = sms_state_size();
    if (!buffer || size < expected) {
        return -1;
    }

    uint8_t *p = buffer;
    struct sms_save_header header = {
        .magic = SAVE_MAGIC,
        .version = SAVE_VERSION,
        .page = {sms.page[0], sms.page[1], sms.page[2]},
        .flags = (uint8_t)((sms.sram_enabled ? 1 : 0) | (sms.sram_bank ? 2 : 0)),
    };
    memcpy(p, &header, sizeof(header));
    p += sizeof(header);

    memcpy(p, &z80, sizeof(z80));
    p += sizeof(z80);
    memcpy(p, sms.ram, sizeof(sms.ram));
    p += sizeof(sms.ram);
    memcpy(p, vdp_vram(), 0x4000);
    p += 0x4000;
    memcpy(p, vdp_cram(), 32);
    p += 32;
    memcpy(p, vdp_sat(), 64);
    p += 64;
    psg_state_save(p);
    p += psg_state_size();
    if (sms.sram_size) {
        memcpy(p, sms.sram, sms.sram_size);
        p += sms.sram_size;
    }

    return (int)(p - (uint8_t *)buffer);
}

int sms_load_state_mem(const void *buffer, size_t size)
{
    size_t expected = sms_state_size();
    if (!buffer || size != expected) {
        return -1;
    }

    const uint8_t *p = buffer;
    struct sms_save_header header;
    memcpy(&header, p, sizeof(header));
    if (header.magic != SAVE_MAGIC || header.version != SAVE_VERSION) {
        return -1;
    }
    p += sizeof(header);

    memcpy(&z80, p, sizeof(z80));
    p += sizeof(z80);
    memcpy(sms.ram, p, sizeof(sms.ram));
    p += sizeof(sms.ram);
    memcpy(vdp_vram(), p, 0x4000);
    p += 0x4000;
    memcpy(vdp_cram(), p, 32);
    p += 32;
    memcpy(vdp_sat(), p, 64);
    p += 64;
    psg_state_load(p);
    p += psg_state_size();
    if (sms.sram_size) {
        memcpy(sms.sram, p, sms.sram_size);
        p += sms.sram_size;
    }

    sms.page[0] = header.page[0];
    sms.page[1] = header.page[1];
    sms.page[2] = header.page[2];
    sms.sram_enabled = (header.flags & 1) != 0;
    sms.sram_bank = (header.flags & 2) != 0;
    return 0;
}

bool sms_sram_dirty(void)
{
    return sms.sram_size > 0 && sms.sram_dirty;
}

int sms_load_sram(const char *file)
{
    if (!sms.sram_size || !file) {
        return -1;
    }
    FILE *f = fopen(file, "rb");
    if (!f) {
        return -1;
    }
    size_t n = fread(sms.sram, 1, sms.sram_size, f);
    fclose(f);
    return n == sms.sram_size ? 0 : -1;
}

int sms_save_sram(const char *file)
{
    if (!sms.sram_size || !file) {
        return -1;
    }
    FILE *f = fopen(file, "wb");
    if (!f) {
        return -1;
    }
    size_t n = fwrite(sms.sram, 1, sms.sram_size, f);
    fclose(f);
    if (n == sms.sram_size) {
        sms.sram_dirty = false;
        return 0;
    }
    return -1;
}
