#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sms.h"

/* VDP mode-4 emulation. */
void vdp_reset(void);
uint8_t vdp_read(uint8_t port);
void vdp_write(uint8_t port, uint8_t value);
uint8_t vdp_vcounter(void);

/* Advance the VDP by `cycles` t-cycles; renders scanlines as they complete.
 * Returns the number of cycles until the next interesting event (unused). */
void vdp_tick(int cycles);

/* Render any remaining lines at end of frame. */
void vdp_end_frame(void);

/* Accessors for save states. */
uint8_t *vdp_vram(void);
uint8_t *vdp_cram(void);
uint8_t *vdp_sat(void);
