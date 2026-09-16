#include "emulator_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "doomgeneric.h"
#include "doomkeys.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nofrendo.h"

#include "buttons.h"

static const char *TAG = "emulator-doom";

enum {
    DOOM_SCREEN_TOP = 45,
    DOOM_SCREEN_BOTTOM = DOOM_SCREEN_TOP + 150,
    KEY_QUEUE_SIZE = 16,
};

typedef struct {
    unsigned char key;
    bool pressed;
} key_event_t;

static key_event_t key_queue[KEY_QUEUE_SIZE];
static unsigned int key_read;
static unsigned int key_write;
static int previous_buttons;

static const struct {
    int button;
    unsigned char key;
} key_bindings[] = {
    {NES_PAD_UP, KEY_UPARROW},       {NES_PAD_DOWN, KEY_DOWNARROW},
    {NES_PAD_LEFT, KEY_LEFTARROW},   {NES_PAD_RIGHT, KEY_RIGHTARROW},
    {NES_PAD_A, KEY_FIRE},           {NES_PAD_B, KEY_USE},
    {NES_PAD_START, KEY_ENTER},      {NES_PAD_SELECT, KEY_ESCAPE},
};

static void queue_key(bool pressed, unsigned char key)
{
    unsigned int next = (key_write + 1) % KEY_QUEUE_SIZE;
    if (next == key_read) {
        ESP_LOGW(TAG, "input queue full");
        return;
    }
    key_queue[key_write] = (key_event_t){.key = key, .pressed = pressed};
    key_write = next;
}

static void poll_buttons(void)
{
    int buttons = buttons_read();
    int changed = buttons ^ previous_buttons;

    for (size_t i = 0; i < sizeof(key_bindings) / sizeof(key_bindings[0]); i++) {
        if (changed & key_bindings[i].button) {
            queue_key((buttons & key_bindings[i].button) != 0, key_bindings[i].key);
        }
    }
    previous_buttons = buttons;
}

static void fill_display_row(uint16_t *dst, const uint16_t *previous, int screen_y,
                             const void *frame)
{
    (void)previous;
    if (screen_y < DOOM_SCREEN_TOP || screen_y >= DOOM_SCREEN_BOTTOM) {
        memset(dst, 0, LCD_W * sizeof(*dst));
        return;
    }

    const uint32_t *src = frame;
    int source_y = (screen_y - DOOM_SCREEN_TOP) * 4 / 3;
    src += source_y * DOOMGENERIC_RESX;
    for (int x = 0; x < LCD_W; x++) {
        uint32_t pixel = src[x * 4 / 3];
        dst[x] = ((pixel & 0x00F80000) >> 8) | ((pixel & 0x0000FC00) >> 5) |
                 ((pixel & 0x000000F8) >> 3);
    }
}

int emulator_doom_run(const char *wad_path)
{
    if (!wad_path) {
        return -1;
    }

    static char *argv[] = {"doom", "-iwad", NULL, "-nomusic", "-nosfx", NULL};
    argv[2] = (char *)wad_path;
    key_read = 0;
    key_write = 0;
    previous_buttons = buttons_read();

    emulator_video_start(DOOMGENERIC_RESX * DOOMGENERIC_RESY * sizeof(pixel_t),
                         MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM, fill_display_row);
    doomgeneric_Create(5, argv);
    return -1;
}

void DG_Init(void)
{
    ESP_LOGI(TAG, "starting Doom at %dx%d", DOOMGENERIC_RESX, DOOMGENERIC_RESY);
}

void DG_DrawFrame(void)
{
    poll_buttons();
    DG_ScreenBuffer = emulator_video_present(DG_ScreenBuffer);
}

void DG_SleepMs(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms == 0 ? 1 : ms));
}

uint32_t DG_GetTicksMs(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

int DG_GetKey(int *pressed, unsigned char *key)
{
    if (key_read == key_write) {
        return 0;
    }

    key_event_t event = key_queue[key_read];
    key_read = (key_read + 1) % KEY_QUEUE_SIZE;
    *pressed = event.pressed;
    *key = event.key;
    return 1;
}

void DG_SetWindowTitle(const char *title)
{
    (void)title;
}
