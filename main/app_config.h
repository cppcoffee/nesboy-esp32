#pragma once

/* User-configurable build options. */
#define NES_ENABLE_FRAME_STATS 1
#define AUDIO_RATE 32000
#define AUDIO_VOLUME_PERCENT 15
#define LCD_ROTATE_180 1

/* ---- Ablation switches ---- */
/* Flip one switch to 0, rebuild, flash, and compare the frame-stats line
 * against the all-1 baseline. Each switch removes exactly one optimization. */
#define EMU_VIDEO_ASYNC 1 /* CPU0 blit worker + spare framebuffer */
#define GB_SCALE_FAST   1 /* pair-expand + row-dup 3:2 GB scaler */
#define REWIND_ENABLE   1 /* rewind snapshot ring (save/load combos stay on) */

/* Rewind ring buffer depth. Each slot is one full in-memory state snapshot
 * (roughly 7 KB for a mapper-0 NES game, 28-180 KB for GB/GBC depending on
 * cartridge RAM); one slot is captured every 3 seconds, so history length =
 * slots * 3 s (5 slots = 15 s). Slots are allocated in PSRAM and fall back to
 * internal RAM if needed. */
#define NES_REWIND_SLOTS 5
#define GB_REWIND_SLOTS 5
