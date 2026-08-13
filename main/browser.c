#include "browser.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bmp.h"
#include "buttons.h"
#include "emulator/emulator.h"
#include "ui.h"

static const char *TAG = "browser";

#define ROOT_PATH     "/sdcard"
#define MAX_ENTRIES   1024
#define MAX_NAME      128
#define MAX_PATH      160

/* Layout: header band, then the list, then the footer band. The number of
 * visible lines must fit between the header and footer or the last entries
 * end up hidden behind the footer / off-screen. */
#define LIST_TOP      (UI_FONT_H + 8)                    /* 24 */
#define FOOTER_TOP    (LCD_H - UI_FONT_H - 3)            /* 221 */
#define LINES_VISIBLE ((FOOTER_TOP - LIST_TOP) / UI_FONT_H)

/* ROM preview: box art in the bottom-right corner, above the footer. */
#define PREVIEW_SIZE 96
#define PREVIEW_X    (LCD_W - PREVIEW_SIZE - 6)          /* 138 */
#define PREVIEW_Y    (FOOTER_TOP - PREVIEW_SIZE - 7)     /* 118 */

typedef struct {
    char name[MAX_NAME];
    uint8_t is_dir;
} entry_t;

/* Preview cache: decoded box art for the currently selected ROM. */
static uint16_t *preview_px;
static char preview_path[MAX_PATH];
static int preview_ok;

/* ---- path helpers (in-place) ---- */
static void path_join(char *dst, int dstsz, const char *dir, const char *name)
{
    int dlen = (int)strlen(dir);
    int nlen = (int)strlen(name);
    /* bounded copy to avoid -Wformat-truncation: dir + '/' + name */
    if (dlen >= dstsz) {
        dlen = dstsz - 1;
    }
    memcpy(dst, dir, dlen);
    int sep = (dlen > 0 && dir[dlen - 1] == '/') ? 0 : 1;
    if (sep) {
        dst[dlen] = '/';
    }
    int remain = dstsz - dlen - sep - 1;
    if (remain < 0) {
        remain = 0;
    }
    if (nlen > remain) {
        nlen = remain;
    }
    memcpy(dst + dlen + sep, name, nlen);
    dst[dlen + sep + nlen] = '\0';
}

/* Strip the last path component; "/sdcard/a/b" -> "/sdcard/a".
 * Root ("/sdcard") stays unchanged. Returns 1 if it changed. */
static int path_parent(char *path)
{
    if (strcmp(path, ROOT_PATH) == 0) {
        return 0;
    }
    char *slash = strrchr(path, '/');
    if (!slash || slash == path) {
        strcpy(path, "/");
    } else {
        *slash = '\0';
    }
    return 1;
}

static int has_rom_ext(const char *name)
{
    return emulator_find(name) != NULL;
}

/* Load box art for a ROM entry: <rom-name-without-extension>.bmp beside the
 * ROM. Cached by image path so cursor moves don't re-read the SD card. */
static void preview_load(const char *dir, const char *name)
{
    char base[MAX_NAME];
    snprintf(base, sizeof(base), "%s", name);
    char *dot = strrchr(base, '.');
    if (dot) {
        *dot = '\0';
    }
    snprintf(base + strlen(base), sizeof(base) - strlen(base), ".bmp");

    char path[MAX_PATH];
    path_join(path, sizeof(path), dir, base);
    if (strcmp(path, preview_path) == 0) {
        return;
    }

    preview_ok = 0;
    strncpy(preview_path, path, sizeof(preview_path));
    preview_path[sizeof(preview_path) - 1] = '\0';

    if (!preview_px) {
        preview_px = heap_caps_malloc(PREVIEW_SIZE * PREVIEW_SIZE * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        if (!preview_px) {
            preview_px = heap_caps_malloc(PREVIEW_SIZE * PREVIEW_SIZE * sizeof(uint16_t),
                                          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
    }
    if (preview_px) {
        preview_ok = bmp_decode(path, preview_px, PREVIEW_SIZE, PREVIEW_SIZE) == 0;
    }
}

static int entry_is_dir(const char *path, const struct dirent *entry)
{
    if (entry->d_type == DT_DIR) {
        return 1;
    }
    if (entry->d_type != DT_UNKNOWN) {
        return 0;
    }

    char full_path[MAX_PATH];
    struct stat st;
    path_join(full_path, sizeof(full_path), path, entry->d_name);
    return stat(full_path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Scan a directory: collect subdirs + supported ROM files, sorted (dirs first then
 * files, each alphabetical). Returns entry count, or -1 on error. */
static int scan_dir(const char *path, entry_t *entries, int max)
{
    DIR *d = opendir(path);
    if (!d) {
        ESP_LOGE(TAG, "opendir(%s) failed", path);
        return -1;
    }
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') {
            continue;
        }
        int is_dir = entry_is_dir(path, e);
        if (!is_dir && !has_rom_ext(e->d_name)) {
            continue;
        }
        if (n >= max) {
            ESP_LOGW(TAG, "directory has more than %d supported entries; remaining files hidden", max);
            break;
        }
        strncpy(entries[n].name, e->d_name, MAX_NAME - 1);
        entries[n].name[MAX_NAME - 1] = '\0';
        entries[n].is_dir = (uint8_t)is_dir;
        n++;
    }
    closedir(d);

    /* simple insertion sort: dirs first, then by name */
    for (int i = 1; i < n; i++) {
        entry_t tmp = entries[i];
        int j = i - 1;
        while (j >= 0) {
            int cmp;
            if (entries[j].is_dir != tmp.is_dir) {
                cmp = entries[j].is_dir ? 1 : -1; /* dir < file */
            } else {
                cmp = strcasecmp(entries[j].name, tmp.name);
            }
            if (cmp > 0) {
                entries[j + 1] = entries[j];
                j--;
            } else {
                break;
            }
        }
        entries[j + 1] = tmp;
    }
    ESP_LOGI(TAG, "scan '%s' -> %d entries", path, n);
    return n;
}

/* ---- drawing ---- */
static void draw_browser(const char *path, entry_t *entries, int count, int cursor)
{
    ui_clear(UI_COLOR_BLACK);

    /* header: current path */
    int y = 2;
    ui_fill_rect(0, 0, LCD_W, UI_FONT_H + 4, UI_COLOR_DARK);
    /* clip path display to available width */
    char hdr[MAX_PATH];
    snprintf(hdr, sizeof(hdr), "%s", path);
    ui_draw_text(4, y, hdr, UI_COLOR_YELLOW);

    /* show box art while a ROM is selected */
    if (cursor < count && !entries[cursor].is_dir) {
        preview_load(path, entries[cursor].name);
    } else {
        preview_ok = 0;
    }

    /* list */
    int half = LINES_VISIBLE / 2;
    int offset = cursor - half; /* center cursor */
    if (offset < 0) {
        offset = 0;
    }
    if (offset + LINES_VISIBLE > count) {
        offset = count - LINES_VISIBLE;
    }
    if (offset < 0) {
        offset = 0;
    }

    /* When the preview is shown, keep list text clear of it. */
    int max_chars = (LCD_W - 12) / UI_FONT_W;
    if (preview_ok) {
        max_chars = (PREVIEW_X - 1 - 6) / UI_FONT_W;
    }

    for (int i = 0; i < LINES_VISIBLE; i++) {
        int idx = offset + i;
        if (idx >= count) {
            break;
        }
        int ry = LIST_TOP + i * UI_FONT_H;
        int selected = (idx == cursor);
        if (selected) {
            /* keep the highlight clear of the preview corner */
            int hl_w = preview_ok ? PREVIEW_X - 1 : LCD_W;
            ui_fill_rect(0, ry - 1, hl_w, UI_FONT_H + 1, UI_COLOR_BLUE);
        }

        char label[MAX_NAME + 4];
        char prefix = entries[idx].is_dir ? '/' : ' ';
        if ((int)strlen(entries[idx].name) > max_chars - 1) {
            snprintf(label, sizeof(label), "%c%.*s", prefix, max_chars - 1, entries[idx].name);
        } else {
            snprintf(label, sizeof(label), "%c%s", prefix, entries[idx].name);
        }

        uint16_t col = selected ? UI_COLOR_WHITE : (entries[idx].is_dir ? UI_COLOR_CYAN : UI_COLOR_GREY);
        ui_draw_text(6, ry, label, col);
    }

    /* footer: controls */
    int fy = FOOTER_TOP + 1;
    ui_fill_rect(0, fy - 1, LCD_W, UI_FONT_H + 3, UI_COLOR_DARK);
    ui_draw_text(2, fy, "NES GB GBC  A:open B:up", UI_COLOR_GREY);

    /* preview: bordered box art in the bottom-right corner */
    if (preview_ok) {
        ui_fill_rect(PREVIEW_X - 1, PREVIEW_Y - 1, PREVIEW_SIZE + 2, PREVIEW_SIZE + 2, UI_COLOR_GREY);
        ui_blit(PREVIEW_X, PREVIEW_Y, PREVIEW_SIZE, PREVIEW_SIZE, preview_px);
    }

    ui_flush();
}

/* ---- button helpers (edge + repeat) ---- */
static int wait_edge(int *held, int *repeat_cnt)
{
    /* poll buttons; return pressed-edge bits. Implements simple auto-repeat. */
    int cur = buttons_read();
    int pressed = cur & ~(*held);
    *held = cur;

    if (cur & (NES_PAD_UP | NES_PAD_DOWN)) {
        (*repeat_cnt)++;
        if (*repeat_cnt > 18) {         /* after ~300ms, auto-repeat */
            if (*repeat_cnt % 3 == 0) { /* every ~50ms */
                pressed |= cur & (NES_PAD_UP | NES_PAD_DOWN);
            }
        }
    } else {
        *repeat_cnt = 0;
    }
    return pressed;
}

int browser_run(char *out_path, int out_path_size)
{
    entry_t *ents = heap_caps_malloc(sizeof(entry_t) * MAX_ENTRIES, MALLOC_CAP_SPIRAM);
    if (!ents) {
        ESP_LOGE(TAG, "no memory for entries");
        return -1;
    }

    char path[MAX_PATH];
    strncpy(path, ROOT_PATH, sizeof(path));
    path[sizeof(path) - 1] = '\0';

    int cursor = 0;
    int count = scan_dir(path, ents, MAX_ENTRIES);
    if (count < 0) {
        free(ents);
        return -1;
    }

    int held = 0, repeat_cnt = 0;
    draw_browser(path, ents, count, cursor);

    for (;;) {
        int pressed = wait_edge(&held, &repeat_cnt);
        int redraw = 0;

        if (pressed & NES_PAD_UP) {
            if (cursor > 0) {
                cursor--;
            } else if (count > 0) {
                cursor = count - 1;
            }
            redraw = 1;
        }

        if (pressed & NES_PAD_DOWN) {
            if (cursor < count - 1) {
                cursor++;
            } else {
                cursor = 0;
            }
            redraw = 1;
        }

        if (pressed & (NES_PAD_A | NES_PAD_RIGHT)) {
            if (cursor < count && ents[cursor].is_dir) {
                char child[MAX_PATH];
                path_join(child, sizeof(child), path, ents[cursor].name);
                int n = scan_dir(child, ents, MAX_ENTRIES);
                if (n >= 0) {
                    strncpy(path, child, sizeof(path));
                    path[sizeof(path) - 1] = '\0';
                    count = n;
                    cursor = 0;
                    redraw = 1;
                }
            } else if (cursor < count) {
                /* supported ROM file selected */
                path_join(out_path, out_path_size, path, ents[cursor].name);
                ESP_LOGI(TAG, "selected: %s", out_path);
                free(ents);
                free(preview_px);
                return 0;
            }
        }

        if (pressed & (NES_PAD_B | NES_PAD_LEFT | NES_PAD_SELECT)) {
            if (path_parent(path)) {
                count = scan_dir(path, ents, MAX_ENTRIES);
                if (count < 0) {
                    count = 0;
                }
                cursor = 0;
                redraw = 1;
            }
            /* at root: B does nothing (per user spec) */
        }
        if (pressed & NES_PAD_START) {
            /* refresh */
            count = scan_dir(path, ents, MAX_ENTRIES);
            if (count < 0) {
                count = 0;
            }
            if (cursor >= count) {
                cursor = 0;
            }
            redraw = 1;
        }

        if (redraw) {
            draw_browser(path, ents, count, cursor);
        }

        vTaskDelay(pdMS_TO_TICKS(16));
    }
}
