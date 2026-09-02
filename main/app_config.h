#pragma once

/* User-configurable build options. */
#define NES_ENABLE_FRAME_STATS 0
#define AUDIO_RATE             48000
#define AUDIO_VOLUME_PERCENT   15
#define LCD_ROTATE_180         1

/* Rewind ring buffer depth. Each slot is one full in-memory state snapshot
 * (roughly 7 KB for a mapper-0 NES game, 28-180 KB for GB/GBC depending on
 * cartridge RAM, 416 KB for GBA and 357 KB for SNES); one slot is captured
 * every 3 seconds, so history length = slots * 3 s. Slots are allocated in
 * PSRAM and fall back to internal RAM if needed. Slot counts are tuned to
 * the PSRAM budget: NES 6 slots (~90 KB, 18 s), GB 6 (18 s), GBA 6 (2.5 MB,
 * 18 s — ROMs larger than ~4 MB leave too little PSRAM and rewind disables
 * itself), SNES 2 (714 KB, 6 s — the 6 MB ROM buffer plus core buffers
 * leave no room for more). */
#define NES_REWIND_SLOTS       6
#define GB_REWIND_SLOTS        6
#define GBA_REWIND_SLOTS       6
#define SNES_REWIND_SLOTS      2
