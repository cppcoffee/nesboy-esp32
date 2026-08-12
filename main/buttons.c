#include "driver/gpio.h"
#include "esp_err.h"
#include "nofrendo.h"
#include "pins.h"

#include "buttons.h"

void buttons_init(void)
{
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << BTN_LEFT_PIN) | (1ULL << BTN_RIGHT_PIN) | (1ULL << BTN_UP_PIN) |
                        (1ULL << BTN_DOWN_PIN) | (1ULL << BTN_A_PIN) | (1ULL << BTN_B_PIN) | (1ULL << BTN_SELECT_PIN) |
                        (1ULL << BTN_START_PIN) | (1ULL << BTN_REWIND_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in));
}

int buttons_read(void)
{
    int b = 0;
    if (!gpio_get_level(BTN_A_PIN)) {
        b |= NES_PAD_A;
    }
    if (!gpio_get_level(BTN_B_PIN)) {
        b |= NES_PAD_B;
    }
    if (!gpio_get_level(BTN_SELECT_PIN)) {
        b |= NES_PAD_SELECT;
    }
    if (!gpio_get_level(BTN_START_PIN)) {
        b |= NES_PAD_START;
    }
    if (!gpio_get_level(BTN_UP_PIN)) {
        b |= NES_PAD_UP;
    }
    if (!gpio_get_level(BTN_DOWN_PIN)) {
        b |= NES_PAD_DOWN;
    }
    if (!gpio_get_level(BTN_LEFT_PIN)) {
        b |= NES_PAD_LEFT;
    }
    if (!gpio_get_level(BTN_RIGHT_PIN)) {
        b |= NES_PAD_RIGHT;
    }
    return b;
}

int buttons_rewind_read(void)
{
    return gpio_get_level(BTN_REWIND_PIN);
}
