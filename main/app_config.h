#pragma once

/* User-configurable build options. */
#define NES_ENABLE_FRAME_STATS 0
#define AUDIO_RATE             48000
#define AUDIO_VOLUME_PERCENT   15
#define LCD_ROTATE_180         1

/* Rewind ring buffer depth. Each slot is one full in-memory state snapshot
 * (roughly 7 KB for a mapper-0 NES game, 28-180 KB for GB/GBC depending on
 * cartridge RAM, 416 KB for GBA and 357 KB for SNES); one slot is captured
 * every 3 seconds, so history length = slots * 3 s (5 slots = 15 s). Slots
 * are allocated in PSRAM and fall back to internal RAM if needed. The SNES
 * ROM buffer is capped at 4 MB to leave room for its 5 slots; GBA's ROM
 * cache tops out at 6 MB, so ROMs larger than that stream pages from the
 * SD card and, on PSRAM pressure, rewind disables itself gracefully. */
#define NES_REWIND_SLOTS       5
#define GB_REWIND_SLOTS        5
#define GBA_REWIND_SLOTS       5
#define SNES_REWIND_SLOTS      5
