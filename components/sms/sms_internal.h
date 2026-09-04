#pragma once

/* Internal state shared between the core's translation units. */

#include <stdbool.h>
#include <stdint.h>

#include "sms.h"

typedef enum {
    MAPPER_NONE,
    MAPPER_SEGA,
    MAPPER_CODEMASTERS,
} sms_mapper_t;

struct sms_state {
    sms_hw_t hw;
    int sample_rate;
    sms_video_cb_t video_cb;
    sms_audio_cb_t audio_cb;

    uint8_t *rom;
    size_t rom_size;
    sms_mapper_t mapper;

    /* Sega mapper page registers (8 KB units) */
    uint8_t page[3];
    bool sram_enabled;
    int sram_bank; /* 0: 0x8000-0xBFFF, 1: 0x4000-0x7FFF */

    const uint8_t *map[4]; /* 4 x 16 KB slots, 8 KB granularity via offset */

    uint8_t ram[0x2000];
    uint8_t *sram;
    size_t sram_size;

    uint16_t frame; /* frame counter for timing */

    uint16_t *framebuffer;
    int pad;
};

extern struct sms_state *sms_state;
#define sms (*sms_state)
