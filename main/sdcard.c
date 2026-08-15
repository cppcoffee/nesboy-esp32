#include "sdcard.h"

#include <stdio.h>

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include "pins.h"

static const char *TAG = "sdcard";

#define MOUNT_POINT "/sdcard"

/* host.slot used by both mount and the (eventual) unmount path. */
static sdmmc_host_t s_host;
static sdmmc_card_t *s_card = NULL;

bool sdcard_mount(char *err_out, size_t err_out_len)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT; /* 20 MHz; safer than 40 MHz for SPI-mode breakouts */
    s_host = host;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_MOSI_PIN,
        .miso_io_num = SD_MISO_PIN,
        .sclk_io_num = SD_CLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4092,
    };
    esp_err_t ret = spi_bus_initialize(s_host.slot, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        if (err_out && err_out_len) {
            snprintf(err_out, err_out_len, "%s", esp_err_to_name(ret));
        }
        return false;
    }

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.host_id = s_host.slot;
    slot_cfg.gpio_cs = SD_CS_PIN;

    esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 0,
    };
    ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &s_host, &slot_cfg, &mount_cfg, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "-> check wiring, card insertion, and 3.3V supply");
        if (err_out && err_out_len) {
            snprintf(err_out, err_out_len, "%s", esp_err_to_name(ret));
        }
        spi_bus_free(s_host.slot);
        s_card = NULL;
        return false;
    }

    ESP_LOGI(TAG, "mount OK:");
    sdmmc_card_print_info(stdout, s_card);
    return true;
}
