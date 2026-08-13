#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    size_t state_size;
    int refresh_rate;
    int (*save)(uint8_t *buffer);
    int (*load)(const uint8_t *buffer);
    void (*preview)(void);
} rewind_backend_t;

typedef enum {
    REWIND_ACTION_NORMAL = 0,
    REWIND_ACTION_SKIP = 1,
    REWIND_ACTION_STEP = 2,
} rewind_action_t;

/* Allocate the rewind ring buffer. Call once after the cart is inserted. */
void rewind_init(const rewind_backend_t *backend);

/* Call once per UI frame.
 *
 * Returns:
 * - REWIND_ACTION_NORMAL: run the emulator and audio normally.
 * - REWIND_ACTION_SKIP: skip emulator/audio for a held rewind frame.
 * - REWIND_ACTION_STEP: restore one older snapshot, then redraw the frame.
 */
rewind_action_t rewind_frame(bool rewind_key);

/* Preview the restored frame, then undo emulation side effects. */
void rewind_redraw(void);

/* When paused, snapshot recording is suppressed. */
void rewind_set_paused(bool paused);

/* Drop all buffered snapshots and reset playback. Call after loading a state
 * from disk so rewind cannot step back into pre-load gameplay. */
void rewind_clear(void);
