idf_component_register(
    SRCS "main.c" "emulator/emulator.c" "emulator/emulator_nes.c" "emulator/emulator_gb.c" "emulator/emulator_snes.c" "display.c" "audio.c" "buttons.c" "frame_stats.c" "rewind.c" "savestate.c" "sdcard.c" "browser.c" "bmp.c" "ui.c" "font8x8.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_driver_i2s esp_driver_ledc esp_driver_spi esp_driver_sdspi esp_lcd esp_timer nofrendo gnuboy snes9x sdmmc fatfs
    EMBED_FILES "assets/emulator_icons.rgb565"
)
