#include "esp_log.h"
#if CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif
#if CONFIG_ESP_WIFI_ENABLED
#include "esp_wifi.h"
#endif
#if CONFIG_BT_ENABLED
#include "esp_bt.h"
#include "esp_bt_main.h"
#endif
#include "freertos/task.h"

#include "audio.h"
#include "browser.h"
#include "buttons.h"
#include "display.h"
#include "emulator/emulator.h"
#include "sdcard.h"
#include "ui.h"

static const char *TAG = "nesboy-esp32";

static void radios_off(void)
{
#if CONFIG_ESP_WIFI_ENABLED
    esp_wifi_stop();
    esp_wifi_deinit();
#endif
#if CONFIG_BT_ENABLED
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    esp_bt_mem_release(ESP_BT_MODE_BTDM);
#endif
}

static void show_error(const char *message)
{
    ESP_LOGE(TAG, "%s", message);
    ui_init();
    ui_clear(UI_COLOR_BLACK);
    ui_draw_text(48, 100, message, UI_COLOR_RED);
    ui_draw_text(24, 124, "INSERT SD CARD AND RESET", UI_COLOR_WHITE);
    ui_flush();
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void emulation_task(void *arg)
{
    (void)arg;

    display_init();
    ui_init();
    if (!sdcard_mount()) {
        show_error("NO SD CARD");
    }

    char rom_path[160];
    if (browser_run(rom_path, sizeof(rom_path)) < 0) {
        show_error("NO ROM FOUND");
    }

    const emulator_t *emulator = emulator_find(rom_path);
    if (!emulator) {
        show_error("UNSUPPORTED ROM");
    }

    /* The browser is one-shot; release its 240x240 RGB565 framebuffer
     * before allocating the emulator and rewind buffers. */
    ui_deinit();

    if (emulator_run(emulator, rom_path) < 0) {
        show_error("ROM LOAD FAILED");
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "boot");

#if CONFIG_PM_ENABLE
    esp_pm_lock_handle_t cpu_lock;
    ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "emulator", &cpu_lock));
    ESP_ERROR_CHECK(esp_pm_lock_acquire(cpu_lock));
#endif

    radios_off();
    audio_init();
    buttons_init();

    xTaskCreatePinnedToCore(emulation_task, "emulation", 16384, NULL, 1, NULL, 1);
    vTaskDelete(NULL);
}
