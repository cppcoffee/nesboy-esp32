#pragma once

#include <stdint.h>

/* SN76489 Programmable Sound Generator. */
void psg_reset(void);
void psg_write(uint8_t value);

/* Advance the PSG by `cycles` t-cycles, writing interleaved stereo samples
 * into the frame sound buffer. */
void psg_tick(int cycles);

void psg_set_buffer(int16_t *buffer, size_t capacity);
size_t psg_samples_ready(void);
const int16_t *psg_buffer(void);
size_t psg_state_size(void);
void psg_state_save(void *out);
void psg_state_load(const void *in);
