// =============================================================
// pins.h - board pinout (all ESP32-S3 GPIO assignments)
// =============================================================
//
// Single source of truth for the hardware wiring shared with the
// badapple-esp32 reference project. Active-low buttons use internal
// pull-ups (button shorts the pin to GND).
//
//   - Display:  ST7789 SPI LCD 240x240 RGB565 (8-pin module)
//                SCLK=4, SDA=5, RES=6, DC=7, CS=15, BLK=16
//   - Audio:    HT517 I2S amplifier (DIx mode C)
//                BCLK=40, WS=41, DIN=42  (EN tied to 3V3, always on)
//   - Input:    8 active-low buttons, to GND
//                L=19 R=45 U=21 D=47 A=1 B=2 SEL=38 STA=39
// =============================================================
#ifndef PINS_H
#define PINS_H

// ---- SPI LCD (8-pin ST7789 240x240 module) ----
#define LCD_SCL_PIN (4)  // SCL / SCLK
#define LCD_SDA_PIN (5)  // SDA / MOSI
#define LCD_RES_PIN (6)  // RES
#define LCD_DC_PIN  (7)  // DC
#define LCD_CS_PIN  (15) // SC / CS
#define LCD_BLK_PIN (16) // BLK / backlight

// ---- I2S amplifier (HT517) ----
// Physical wiring: DI2=GPIO40, DI0=GPIO41, DI1=GPIO42.
// HT517 DIx mode C maps DI2=BCLK, DI0=LRCK, DI1=SDIN and mixes L/2+R/2.
#define AMP_BCLK_PIN (40) // DI2 / BCLK
#define AMP_WS_PIN   (41) // DI0 / LRCK
#define AMP_DOUT_PIN (42) // DI1 / SDIN
#define AMP_EN_PIN   (-1) // EN tied directly to 3V3 in hardware

// ---- GPIO button pad (active-low, internal pull-up) ----
// NOTE on special ESP32-S3 pins used here:
//   GPIO33..37      - octal-PSRAM D4..D7/DQS on N16R8, reserved by PSRAM.
//   GPIO38 (Select) - FSPIWP/SUBSPIWP mux, plain GPIO by default, fine for buttons.
//   GPIO39 (Start)  - MTCK (JTAG TMS) mux, plain GPIO by default, fine for buttons.
//   GPIO45 (Right)  - strapping pin (VDD_SPI voltage: 0=3.3V, 1=1.8V, weak
//                     pull-down at reset). The active-low button can only pull
//                     it LOW (= 3.3V flash, the safe state), so holding Right
//                     during power-on is harmless.
//   GPIO19 (Left)   - USB D-. If COM3 is the native USB port, using this pin
//                     breaks post-boot serial monitor (reflash still works).
#define BTN_LEFT_PIN   (19)
#define BTN_RIGHT_PIN  (45)
#define BTN_UP_PIN     (21)
#define BTN_DOWN_PIN   (47)
#define BTN_A_PIN      (1)
#define BTN_B_PIN      (2)
#define BTN_SELECT_PIN (38)
#define BTN_START_PIN  (39)

// ---- Rewind button (active-low, internal pull-up) ----
// Hold to step backward through recent snapshots.
#define BTN_REWIND_PIN (8)

// ---- microSD card breakout (SPI mode, on SPI3_HOST) ----
// Separate SPI bus from the LCD (SPI2_HOST). GPIO11..14 are plain GPIOs on
// the ESP32-S3 (NOT strapping pins), so on-board pull-ups on the breakout do
// not interfere with boot. The classic-ESP32 GPIO12/MTDI warning does NOT
// apply to this S3 board.
#define SD_CS_PIN   (14)
#define SD_MOSI_PIN (13)
#define SD_CLK_PIN  (12)
#define SD_MISO_PIN (11)

#endif // PINS_H
