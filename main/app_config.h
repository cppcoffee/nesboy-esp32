#pragma once

/* User-configurable build options. */
#define NES_ENABLE_FRAME_STATS 0
#define AUDIO_RATE 32000
#define AUDIO_VOLUME_PERCENT 15
#define LCD_ROTATE_180 1

/* Rewind ring buffer depth. Each slot is one full in-memory state snapshot
 * (roughly 7 KB for a mapper-0 NES game, 28-180 KB for GB/GBC depending on
 * cartridge RAM, 357 KB for SNES, and 25-57 KB for SMS/GG depending on
 * cartridge RAM); one slot is captured every 3 seconds, so history length =
 * slots * 3 s (5 slots = 15 s). Slots
 * are allocated in PSRAM and fall back to internal RAM if needed. The SNES
 * ROM buffer is sized to the selected cartridge up to 6 MB. Large SNES ROMs
 * can leave room for fewer rewind slots; rewind shortens its history rather
 * than disabling itself. */
#define NES_REWIND_SLOTS 5
#define GB_REWIND_SLOTS 5
#define SNES_REWIND_SLOTS 5
#define SMS_REWIND_SLOTS 5
