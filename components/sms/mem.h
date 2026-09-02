#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sms.h"

/* Memory-mapped read/write for the Z80 address space, plus I/O ports
 * (VDP and PSG). Implemented in mem.c. */
uint8_t sms_read8(uint16_t addr);
void sms_write8(uint16_t addr, uint8_t value);
uint8_t sms_in(uint16_t port);
void sms_out(uint16_t port, uint8_t value);
