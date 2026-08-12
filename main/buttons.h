#pragma once

#include "nofrendo.h"

/* Initialize GPIO for the 8 active-low buttons plus the rewind button */
void buttons_init(void);

/* Read current button state as a NES pad bitmask */
int buttons_read(void);

/* Read the rewind button GPIO level (active-low: 0 = pressed). */
int buttons_rewind_read(void);
