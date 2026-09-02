#pragma once

#include <stdint.h>

#include "sms.h"

typedef struct {
    union {
        struct {
            uint8_t c, b, e, d, l, h, f, a; /* little endian pairs */
        };
        uint16_t bc, de, hl, af;
    };
    union {
        struct {
            uint8_t _c2, b2, _e2, d2, _l2, h2, _f2, a2;
        };
        uint16_t bc2, de2, hl2, af2;
    };
    uint16_t ix, iy, sp, pc;
    uint8_t i, r, im;
    bool iff1, iff2, halted;
    /* cycle accounting: t-cycles consumed by the last executed instruction */
    int cycles;
} sms_z80_t;

extern sms_z80_t z80;

void sms_z80_reset(void);
/* Execute instructions until at least `cycles` t-cycles have passed.
 * Returns the actual number of cycles consumed (may overshoot slightly). */
int sms_z80_run(int cycles);
