#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool S9xSaveState(const char *filename);
bool S9xLoadState(const char *filename);

/* In-memory variants used by the rewind/save-state ring buffer. */
bool S9xSaveStateMem(uint8_t *buffer, size_t buffer_size, size_t *written);
bool S9xLoadStateMem(const uint8_t *buffer, size_t buffer_size);
