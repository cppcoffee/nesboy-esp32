#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "app_config.h"
#include "rewind.h"

#define REWIND_POINT_SECONDS   3
#define REWIND_DEBOUNCE_FRAMES 3
#define REWIND_TAG             "rewind"

/* --- Ring buffer of snapshots --- */
struct rewind_ring {
    uint8_t *slots[NES_REWIND_SLOTS]; /* max ring depth */
    int slot_count;
    size_t slot_size;
    int write_idx;  /* next slot to write into */
    int oldest_idx; /* oldest valid slot */
    int filled;     /* number of valid slots */
};

/* --- Snapshot timing --- */
struct rewind_timing {
    int frame_count;
    int frames_per_snapshot;
    int frames_per_step;
};

/* --- Rewind playback state --- */
struct rewind_playback {
    int count;
    int pos;
    bool active;
};

/* --- Input debounce --- */
struct rewind_input {
    bool prev_pressed;
    bool raw_pressed;
    bool stable_pressed;
    bool ready;
    int debounce_count;
};

/* Top-level rewind state */
static struct {
    rewind_backend_t backend;
    struct rewind_ring ring;
    struct rewind_timing timing;
    struct rewind_playback play;
    struct rewind_input input;
    bool paused;
    int requested_slots;
} rw;

static int oldest_slot(void)
{
    return rw.ring.oldest_idx;
}

static int newest_slot(void)
{
    if (rw.ring.filled == 0) {
        return rw.ring.oldest_idx;
    }
    return (rw.ring.oldest_idx + rw.ring.filled - 1) % rw.ring.slot_count;
}

static int ring_distance(int from, int to)
{
    if (to >= from) {
        return to - from;
    }
    return rw.ring.slot_count - from + to;
}

void rewind_init(const rewind_backend_t *backend)
{
    memset(&rw, 0, sizeof(rw));
    if (!backend || !backend->state_size || backend->refresh_rate <= 0 || !backend->save || !backend->load ||
        !backend->preview) {
        ESP_LOGE(REWIND_TAG, "invalid backend, rewind disabled");
        return;
    }

    rw.backend = *backend;
    rw.requested_slots = backend->slots > 0 ? backend->slots : NES_REWIND_SLOTS;
    if (rw.requested_slots > NES_REWIND_SLOTS) {
        rw.requested_slots = NES_REWIND_SLOTS;
    }
    rw.ring.slot_count = rw.requested_slots;
    rw.ring.slot_size = backend->state_size;
    rw.timing.frames_per_snapshot = backend->refresh_rate * REWIND_POINT_SECONDS;
    rw.timing.frames_per_step = backend->refresh_rate;

#if CONFIG_SPIRAM
    uint32_t caps = MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM;
    const char *heap_name = "psram";
#else
    uint32_t caps = MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL;
    const char *heap_name = "internal";
#endif

    for (int i = 0; i < rw.ring.slot_count; i++) {
        rw.ring.slots[i] = heap_caps_malloc(rw.ring.slot_size, caps);
        if (!rw.ring.slots[i]) {
#if CONFIG_SPIRAM
            if (caps & MALLOC_CAP_SPIRAM) {
                ESP_LOGW(REWIND_TAG, "psram allocation failed, retrying internal heap");
                for (int j = 0; j < i; j++) {
                    free(rw.ring.slots[j]);
                    rw.ring.slots[j] = NULL;
                }
                caps = MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL;
                heap_name = "internal";
                i = -1;
                continue;
            }
#endif
            ESP_LOGE(REWIND_TAG, "out of memory: slot %d (%u bytes), rewind disabled", i, (unsigned)rw.ring.slot_size);
            for (int j = 0; j < i; j++) {
                free(rw.ring.slots[j]);
                rw.ring.slots[j] = NULL;
            }
            rw.input.ready = false;
            return;
        }
    }

    rw.input.ready = true;

    ESP_LOGI(REWIND_TAG,
             "ready: %d slots x %u bytes every %ds in %s (%.1f KB total, %.1f KB PSRAM free, %.1f KB internal free)",
             rw.ring.slot_count, (unsigned)rw.ring.slot_size, REWIND_POINT_SECONDS, heap_name,
             (double)(rw.ring.slot_count * rw.ring.slot_size) / 1024.0,
             (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024.0,
             (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024.0);
}

static bool rewind_pressed(bool rewind_key)
{
    bool sampled_pressed = !rewind_key;

    if (sampled_pressed != rw.input.raw_pressed) {
        rw.input.raw_pressed = sampled_pressed;
        rw.input.debounce_count = 0;
    } else if (rw.input.debounce_count < REWIND_DEBOUNCE_FRAMES) {
        rw.input.debounce_count++;
    }

    if (rw.input.debounce_count >= REWIND_DEBOUNCE_FRAMES) {
        rw.input.stable_pressed = rw.input.raw_pressed;
    }

    return rw.input.stable_pressed;
}

rewind_action_t rewind_frame(bool rewind_key)
{
    if (!rw.input.ready) {
        return REWIND_ACTION_NORMAL;
    }

    bool pressed = rewind_pressed(rewind_key);

    if (rw.paused) {
        rw.input.prev_pressed = pressed;
        return REWIND_ACTION_NORMAL;
    }

    if (!rw.play.active) {
        if (rw.timing.frame_count >= rw.timing.frames_per_snapshot) {
            rw.timing.frame_count = 0;
            int written = rw.backend.save(rw.ring.slots[rw.ring.write_idx]);
            /* written may be the exact snapshot size (<= slot_size) or the
             * full slot size depending on the core; only negative values
             * indicate failure. */
            if (written < 0 || written > (int)rw.ring.slot_size) {
                ESP_LOGE(REWIND_TAG, "snapshot failed: %d/%u bytes", written, (unsigned)rw.ring.slot_size);
            } else {
                rw.ring.write_idx = (rw.ring.write_idx + 1) % rw.ring.slot_count;
                if (rw.ring.filled < rw.ring.slot_count) {
                    if (rw.ring.filled == 0) {
                        rw.ring.oldest_idx = 0;
                    }
                    rw.ring.filled++;
                } else {
                    rw.ring.oldest_idx = rw.ring.write_idx;
                }
            }
        }
        rw.timing.frame_count++;

        if (pressed && !rw.input.prev_pressed && rw.ring.filled > 0) {
            rw.play.active = true;
            rw.play.count = 0;
            rw.play.pos = newest_slot();
            rw.input.prev_pressed = pressed;
            if (rw.backend.load(rw.ring.slots[rw.play.pos]) == 0) {
                return REWIND_ACTION_STEP;
            }
            ESP_LOGE(REWIND_TAG, "restore failed");
            rw.play.active = false;
            return REWIND_ACTION_NORMAL;
        }

        rw.input.prev_pressed = pressed;
        return REWIND_ACTION_NORMAL;
    }

    if (!pressed && rw.input.prev_pressed) {
        rw.play.active = false;
        rw.ring.filled = ring_distance(rw.ring.oldest_idx, rw.play.pos) + 1;
        rw.ring.write_idx = (rw.play.pos + 1) % rw.ring.slot_count;
        rw.timing.frame_count = 0;
        rw.play.count = 0;
        rw.input.prev_pressed = pressed;
        return REWIND_ACTION_NORMAL;
    }

    rw.play.count++;
    if (rw.play.count < rw.timing.frames_per_step) {
        rw.input.prev_pressed = pressed;
        return REWIND_ACTION_SKIP;
    }

    rw.play.count = 0;
    int oldest = oldest_slot();
    if (rw.play.pos != oldest) {
        rw.play.pos = (rw.play.pos + rw.ring.slot_count - 1) % rw.ring.slot_count;
    } else {
        /* wrapped around: jump from the oldest slot back to the newest so
         * holding rewind keeps cycling through the ring instead of stopping */
        rw.play.pos = newest_slot();
    }
    if (rw.backend.load(rw.ring.slots[rw.play.pos]) < 0) {
        ESP_LOGE(REWIND_TAG, "restore failed");
        rw.play.active = false;
        rw.input.prev_pressed = pressed;
        return REWIND_ACTION_NORMAL;
    }
    ESP_LOGI(REWIND_TAG, "rewind step slot %d", rw.play.pos);
    rw.input.prev_pressed = pressed;
    return REWIND_ACTION_STEP;
}

void rewind_redraw(void)
{
    if (!rw.input.ready) {
        return;
    }

    rw.backend.preview();
    if (rw.backend.load(rw.ring.slots[rw.play.pos]) < 0) {
        ESP_LOGE(REWIND_TAG, "post-preview restore failed");
    }
}

void rewind_set_paused(bool paused)
{
    rw.paused = paused;
}

void rewind_clear(void)
{
    rw.ring.filled = 0;
    rw.ring.write_idx = 0;
    rw.ring.oldest_idx = 0;
    rw.timing.frame_count = 0;
    rw.play.active = false;
    rw.play.count = 0;
}
