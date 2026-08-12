#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "buttons.h"
#include "display.h"
#include "savestate.h"

#define SAVESTATE_TAG         "savestate"
#define STATE_PATH_MAX        192
#define COMBO_DEBOUNCE_FRAMES 5

static bool combo_edge(bool held, int *count)
{
    if (!held) {
        *count = 0;
        return false;
    }
    if (*count >= COMBO_DEBOUNCE_FRAMES) {
        return false;
    }
    (*count)++;
    return *count == COMBO_DEBOUNCE_FRAMES;
}

static int make_state_path(const char *rom_path, const char *suffix, char *path, size_t size)
{
    int written = snprintf(path, size, "%s.state%s", rom_path, suffix);
    return written >= 0 && (size_t)written < size ? 0 : -1;
}

static bool file_exists(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        return false;
    }
    fclose(file);
    return true;
}

static int commit_state_file(const char *temp_path, const char *state_path, const char *backup_path)
{
    bool had_state = file_exists(state_path);
    if (had_state) {
        remove(backup_path);
        if (rename(state_path, backup_path) < 0) {
            return -1;
        }
    }

    if (rename(temp_path, state_path) < 0) {
        if (had_state) {
            rename(backup_path, state_path);
        }
        return -1;
    }

    remove(backup_path);
    return 0;
}

/* State snapshots are up to ~180 KB for GB/GBC: prefer PSRAM, fall back to
 * internal RAM if PSRAM allocation fails. */
static uint8_t *alloc_buffer(size_t size)
{
    uint8_t *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return buf;
}

static int savestate_write(const char *rom_path, const rewind_backend_t *backend)
{
    if (!rom_path || !backend || !backend->save || backend->state_size == 0) {
        return -1;
    }

    char state_path[STATE_PATH_MAX];
    char temp_path[STATE_PATH_MAX];
    char backup_path[STATE_PATH_MAX];
    if (make_state_path(rom_path, "", state_path, sizeof(state_path)) < 0 ||
        make_state_path(rom_path, ".tmp", temp_path, sizeof(temp_path)) < 0 ||
        make_state_path(rom_path, ".bak", backup_path, sizeof(backup_path)) < 0) {
        return -1;
    }
    remove(temp_path);

    if (backend->save_file) {
        if (backend->save_file(temp_path) < 0) {
            remove(temp_path);
            return -1;
        }
        if (commit_state_file(temp_path, state_path, backup_path) < 0) {
            remove(temp_path);
            return -1;
        }
        return 0;
    }

    uint8_t *buf = alloc_buffer(backend->state_size);
    if (!buf) {
        ESP_LOGE(SAVESTATE_TAG, "out of memory: %u bytes", (unsigned)backend->state_size);
        return -1;
    }

    int written = backend->save(buf);
    if (written != (int)backend->state_size) {
        ESP_LOGE(SAVESTATE_TAG, "state save failed: %d/%u bytes", written, (unsigned)backend->state_size);
        free(buf);
        return -1;
    }

    FILE *file = fopen(temp_path, "wb");
    if (!file || fwrite(buf, 1, (size_t)written, file) != (size_t)written) {
        ESP_LOGE(SAVESTATE_TAG, "failed to write %s", temp_path);
        if (file) {
            fclose(file);
        }
        free(buf);
        remove(temp_path);
        return -1;
    }
    if (fclose(file) != 0) {
        free(buf);
        remove(temp_path);
        return -1;
    }
    free(buf);
    if (commit_state_file(temp_path, state_path, backup_path) < 0) {
        remove(temp_path);
        return -1;
    }
    ESP_LOGI(SAVESTATE_TAG, "saved: %s (%d bytes)", state_path, written);
    return 0;
}

static int savestate_read(const char *rom_path, const rewind_backend_t *backend)
{
    if (!rom_path || !backend || !backend->load || backend->state_size == 0) {
        return -1;
    }

    char state_path[STATE_PATH_MAX];
    char backup_path[STATE_PATH_MAX];
    if (make_state_path(rom_path, "", state_path, sizeof(state_path)) < 0 ||
        make_state_path(rom_path, ".bak", backup_path, sizeof(backup_path)) < 0) {
        return -1;
    }
    const char *load_path = file_exists(state_path) ? state_path : backup_path;

    if (backend->load_file) {
        uint8_t *rollback = alloc_buffer(backend->state_size);
        if (!rollback || backend->save(rollback) != (int)backend->state_size) {
            free(rollback);
            return -1;
        }
        int result = backend->load_file(load_path);
        if (result < 0) {
            backend->load(rollback);
        }
        free(rollback);
        return result;
    }

    FILE *file = fopen(load_path, "rb");
    if (!file) {
        return -1; /* no save yet — normal */
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size != (long)backend->state_size) {
        ESP_LOGW(SAVESTATE_TAG, "size mismatch: %s is %ld bytes, expected %u", load_path, size,
                 (unsigned)backend->state_size);
        fclose(file);
        return -1;
    }

    uint8_t *buf = alloc_buffer((size_t)size);
    if (!buf) {
        ESP_LOGE(SAVESTATE_TAG, "out of memory: %ld bytes", size);
        fclose(file);
        return -1;
    }
    if (fread(buf, 1, (size_t)size, file) != (size_t)size) {
        ESP_LOGE(SAVESTATE_TAG, "read failed: %s", load_path);
        fclose(file);
        free(buf);
        return -1;
    }
    fclose(file);

    if (backend->load(buf) < 0) {
        ESP_LOGE(SAVESTATE_TAG, "state load failed: %s", load_path);
        free(buf);
        return -1;
    }
    free(buf);
    ESP_LOGI(SAVESTATE_TAG, "loaded: %s (%ld bytes)", load_path, size);
    return 0;
}

static void load_on_combo(const char *rom_path, const rewind_backend_t *backend)
{
    if (savestate_read(rom_path, backend) == 0) {
        rewind_clear();
        display_osd_text("LOADED");
    } else {
        display_osd_text("NO SAVE");
    }
}

static void save_on_combo(const char *rom_path, const rewind_backend_t *backend)
{
    display_osd_text(savestate_write(rom_path, backend) == 0 ? "SAVED" : "SAVE FAIL");
}

int savestate_handle_buttons(const char *rom_path, const rewind_backend_t *backend, int buttons)
{
    static int load_count;
    static int save_count;

    int rewind_key = buttons_rewind_read();
    bool load_held = rewind_key == 0 && (buttons & NES_PAD_SELECT) != 0;
    bool save_held = (buttons & (NES_PAD_START | NES_PAD_SELECT)) == (NES_PAD_START | NES_PAD_SELECT);
    bool load_edge = combo_edge(load_held, &load_count);
    bool save_edge = combo_edge(save_held, &save_count);

    if (load_edge) {
        load_on_combo(rom_path, backend);
    } else if (save_edge && !load_held) {
        save_on_combo(rom_path, backend);
    }

    return load_held ? 1 : rewind_key;
}
