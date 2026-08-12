#pragma once

/* User-configurable build options. */
#define NES_ENABLE_FRAME_STATS 0
#define AUDIO_RATE             48000
#define AUDIO_VOLUME_PERCENT   15
#define LCD_ROTATE_180         1

/* Rewind ring buffer depth. Each slot is one full in-memory state snapshot:
 * roughly 7 KB for a mapper-0 NES game and 28-180 KB for GB/GBC depending on
 * cartridge RAM. Six slots give ~18 s of 3-second-interval history. Slots are
 * allocated in PSRAM and fall back to internal RAM if needed. */
#define NES_REWIND_SLOTS 6
