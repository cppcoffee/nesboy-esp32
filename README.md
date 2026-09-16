# nesboy-esp32

Minimal ESP-IDF NES, Game Boy, Game Boy Color, and Doom launcher for ESP32-S3.

NES runs at a full **60 FPS** (NTSC), driven by audio-paced frame timing on the 240 MHz dual-core ESP32-S3. On boot, the ROM browser scans the SD card and accepts `.nes`, `.gb`, `.gbc`, and user-supplied Doom `.wad` files; there is no embedded fallback ROM. It uses the ST7789/I2S/button wiring defined in `main/pins.h` and reuses the `nofrendo`, `gnuboy`, and DoomGeneric cores.

## Playing Games from the SD Card

The ROM used at runtime is chosen from the SD card at boot. Changing games is now just a file copy — no rebuild needed.

### Setup

1. **Format the SD card as FAT32** (most cards come pre-formatted; a 32 GB card works fine). The ESP-IDF FATFS in this build does *not* support exFAT, so exFAT-formatted cards must be reformatted to FAT32 first.
2. **Copy `.nes`, `.gb`, `.gbc`, or your own Doom `.wad` files** to the card. NES files are read up to 2 MB and Game Boy ROM banks are loaded from the card into PSRAM as needed. Other files are hidden from the browser.
3. Insert the card and power on the board.

### Using the browser

The browser opens with a pixel-art emulator menu: NES, Game Boy / Color, and DOS / Doom. Press A to enter a system and see only its compatible ROMs; press B from the ROM list to return to the emulator menu. Directories remain available inside each system (Left or Select goes to the parent folder), with directories first and ROMs sorted alphabetically. Extension matching is case-insensitive, and up to 1024 entries are shown per folder. It is a one-shot startup picker: after a ROM is selected, reset/re-power the ESP32 to pick another game.

While a ROM is highlighted, the browser shows box art in the bottom-right corner: a 24/32-bit uncompressed BMP named after the ROM without its extension (e.g. `smb.nes` → `smb.bmp`). Any image can be converted with ImageMagick (`magick cover.png -resize 192x192 smb.bmp`) or `sips -s format bmp cover.png --out smb.bmp`; the browser scales it automatically. Missing or unreadable images are simply skipped.

| Button | Action |
| --- | --- |
| Up / Down | Move the cursor (holds auto-repeat) |
| A / Right | Enter an emulator, enter a folder, or start the highlighted ROM |
| B | Return from the ROM browser to the emulator menu |
| Left / Select | Go to the parent folder inside the ROM browser |
| Start | Refresh the current ROM folder |

### Wiring

| MicroSD breakout | ESP32-S3 GPIO |
| --- | ---: |
| CS | 14 |
| MOSI | 13 |
| CLK | 12 |
| MISO | 11 |
| 3.3 V | 3.3 V |
| GND | GND |

The SD card runs on a separate SPI bus (**SPI3_HOST**) from the LCD (**SPI2_HOST**), capped at 10 MHz for reliable sustained reads through breakout wiring (the negotiated speed is reported at boot). GPIO11–14 are plain GPIOs on the ESP32-S3, so the pull-up resistors on the breakout do not interfere with boot — the classic-ESP32 "GPIO12/MTDI strapping" warning does **not** apply to this board.

### No embedded ROM

There is no fallback ROM compiled into the firmware: a game must always be picked from the SD card. If the card is missing, unreadable, or the selection fails, the device shows an error on the screen and halts — insert the card and reset.

### Game Boy notes

- `.gb` and `.gbc` use the `gnuboy` core. CGB-capable cartridges automatically run in color mode.
- The native 160×144 image is scaled to 240×216 with an exact nearest-neighbor 3:2 expansion and centered on the 240×240 display. The scaler expands each two-pixel pair directly and copies duplicate rows instead of recalculating all 51,840 output pixels.
- Stereo audio uses the same 32 kHz I2S output and volume controls as NES.
- Battery RAM is not read from or periodically written to the SD card during gameplay; use a save state for persistence across power-off.
- The dedicated Rewind button works for GB and GBC games as well as NES.

### Doom notes

- Select **DOS / DOOM** in the launcher and choose a compatible `.wad` from the SD card. The firmware contains no game data and ignores non-WAD files.
- DoomGeneric runs the original Doom game clock at 35 Hz; the display is scaled from 320x200 to a centered 240x150 image. It is therefore intentionally separate from the 60 FPS NES/GB promise.
- `A` fires, `B` uses/open doors, the D-pad moves and turns, `Start` confirms, and `Select` opens the Doom menu.
- The current Doom port is video and input only; music and sound effects are disabled until the original sound backend is adapted to the project's I2S audio path.

## Flashing / Rebuilding

1. **Build** — With the ESP-IDF environment loaded, run:

   ```sh
   idf.py set-target esp32s3
   idf.py build
   ```

2. **Flash & monitor** — Replace the serial port with your actual device:

   ```sh
   idf.py -p /dev/tty.usbmodemXXXX flash monitor
   ```

### Notes

- Supported ROM formats are `.nes`, `.gb`, and `.gbc`; compressed archives are not supported.
- If the screen stays white or black after selecting a ROM, the ROM may be incompatible with the selected core.

## Build

Prerequisite: ESP-IDF environment loaded.

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/tty.usbmodemXXXX flash monitor
```

## Rewind

For NES, GB, and GBC games, a dedicated **Rewind** button (GPIO 8, active-low) scrubs the game backward while held. The longer you hold it, the farther back it goes, in roughly 1-second steps; releasing the button stops the rewind and continues from the restored point. At the oldest snapshot the playback wraps around and cycles back to the newest one, so holding the button keeps looping through the ring.

While the button is held, normal gameplay and audio output are paused. Each rewind step restores an older snapshot, previews a frame, and restores the snapshot again so gameplay continues from the selected point when the button is released.

Internally the emulator captures one in-memory state snapshot every 3 seconds into a ring buffer, so history length is slots × 3 seconds. NES snapshots are roughly 15 KB for mapper-0 CHR-ROM games with 8 KB PRG RAM; GB/GBC snapshots vary with cartridge RAM from about 28 KB to 180 KB each. Rewind slots live in PSRAM (octal PSRAM is enabled in this build) via explicit `MALLOC_CAP_SPIRAM` allocation; if PSRAM runs short, as many slots as fit are allocated and the history shortens instead of rewind disabling itself. The log after a ROM loads reports the exact slot size, total rewind allocation, and remaining PSRAM/internal RAM.

### Memory lifecycle

The boot-time browser supports up to 1024 directories/ROMs per folder and allocates about 129 KiB for that entry list plus a 112.5 KiB 240×240 RGB565 UI framebuffer in PSRAM. Both allocations are freed when a ROM is selected, before emulator and rewind buffers are allocated. The UI framebuffer is recreated only if a later ROM-load error must be displayed.

Runtime allocations are intentionally retained: framebuffer/audio memory, emulated WRAM/VRAM/cartridge RAM, active rewind slots, and gnuboy's ROM-bank cache are all used while playing. GBC ROM banks are cached in PSRAM (up to 128 preloaded 16 KB banks, about 2 MB) to avoid SD-card stalls; reducing that cache would trade memory for possible frame/audio hiccups rather than being a safe deallocation.

## Save States

The emulator cores expose save-state serializers for CPU, RAM, video, mapper, and battery-backed cartridge state. Two button combos persist that state to the SD card and restore it:

| Combo | Action | OSD feedback |
| --- | --- | --- |
| **Select + Start** | Save the current state to `<rom>.state` beside the ROM | `SAVED` |
| **Select + A** | Load the state back from `<rom>.state` | `LOADED` / `NO SAVE` |

Both combos are debounced for ~5 frames and trigger exactly once per press, regardless of which key was pressed first. The OSD text appears in the middle of the screen for about 1 second with a typewriter reveal animation.

Details:

- The state file is written next to the ROM on the SD card (e.g. `smb.nes` → `smb.nes.state`). It is overwritten on each save and kept until the next save, so you can load it any number of times — a fresh game is always one *not* pressing the load combo away. `.state` files are hidden from the ROM browser.
- Loading replaces the running game with the saved snapshot and clears the rewind history, so holding Rewind afterwards cannot step back into pre-load gameplay. Both cores store the same complete in-memory snapshot format used by rewind, so a load restores the exact machine state the save captured and gameplay continues bit-identically from the saved point. NES snapshots are ~7–16 KB; GB/GBC snapshots are 28–180 KB depending on cartridge RAM, so a brief audio pause may be audible during the worst-case write.
- **NES note:** NES games currently have no `.sav` battery file — the save-state combo is the *only* persistence for battery-backed games (e.g. Zelda) across power-off. The state snapshot includes PRG RAM, so a `SAVED` snapshot captures it and can be loaded after a reboot.
- **GB/GBC note:** the state includes battery RAM. There is no periodic `.sav` sync, so use the state-save combo to persist it across power-off.
- If the save and load combos are held together (**Select + Start + A**), the load takes priority.
- The Rewind button itself is independent of the save/load combos: it always rewinds while held.

## On-Screen Controls

Hold **Select** and press a direction to adjust settings in real time. An on-screen indicator (red bar = volume, blue bar = brightness) appears at the bottom of the screen for about 1 second after each change.

| Hold Select + | Adjustment | Step |
| --- | --- | ---: |
| Up | Volume up | +1% |
| Down | Volume down | –1% |
| Left | Brightness down | –2% |
| Right | Brightness up | +2% |

Volume and brightness both clamp to 0–100%. The default volume is set in `main/app_config.h` (`AUDIO_VOLUME_PERCENT`, default 15). Brightness defaults to 85%.

The backlight brightness is implemented with LEDC PWM (5 kHz, 8-bit). The volume scaling is applied sample-by-sample in the audio DMA task with no audible artifacts.

Holding a direction continuously repeats the adjustment after a short delay (~330 ms) at a rate of roughly 100 ms per step.

## Frame Statistics

User-configurable build macros are kept in `main/app_config.h`. FPS logging is disabled by default so the timing calls do not add overhead. To enable it, change the macro there and rebuild:

```c
#define NES_ENABLE_FRAME_STATS 1
```

When enabled, one summary is printed every 600 outer-loop frames for every emulator. The original Game Boy timing is about 59.73 FPS, so a healthy GB/GBC result is approximately 59.7 FPS rather than exactly 60.0.

The log reports emulation `fps`, rendered `display` FPS, average and maximum `emulate` work time, and `audio_wait`. Audio-queue blocking is measured where it actually occurs and excluded from `emulate`, so the values can be used to tell CPU/GPU work from normal audio pacing.

The full 240×240 screen is filled each frame (NES overscan is not cropped) using 20-line DMA chunks with two alternating internal-RAM buffers. NES and GB/GBC also use two emulator framebuffers, so CPU1 can emulate the next frame while CPU0 converts and sends the previous one. This keeps the SPI/GDMA pipeline busy while releasing about 94 KiB of scarce internal RAM for emulator hot memory.

Set `NES_ENABLE_FRAME_STATS` back to `0` for normal builds. The counters, timing, and log formatting are implemented separately in `main/frame_stats.c`.

## Hardware

The current wiring is defined in `main/pins.h`:

| Device | Signal | ESP32-S3 GPIO |
| --- | --- | ---: |
| ST7789 | SCLK | 4 |
| ST7789 | MOSI / SDA | 5 |
| ST7789 | RESET | 6 |
| ST7789 | DC | 7 |
| ST7789 | CS | 15 |
| ST7789 | Backlight (PWM) | 16 |
| HT517 DIx mode C | DI2 / BCLK | 40 |
| HT517 DIx mode C | DI0 / WS / LRCK | 41 |
| HT517 DIx mode C | DI1 / SDIN | 42 |
| microSD (SPI3) | CS | 14 |
| microSD (SPI3) | MOSI | 13 |
| microSD (SPI3) | CLK | 12 |
| microSD (SPI3) | MISO | 11 |
| Button | A | 1 |
| Button | B | 2 |
| Button | Left | 19 |
| Button | Up | 21 |
| Button | Start | 39 |
| Button | Select | 38 |
| Button | Right | 45 |
| Button | Down | 47 |
| Button | Rewind | 8 |

The HT517 `EN` pin is tied to 3.3 V. All buttons are active-low, use the ESP32-S3 internal pull-ups, and connect to GND when pressed.

> [!IMPORTANT]
> Octal PSRAM is enabled and reserves GPIO33–37 (D4–D7/DQS) plus the MSPI shared bus (GPIO26–32). Start/Select use GPIO38/39 — GPIO38 is an FSPIWP mux (plain GPIO by default), GPIO39 is MTCK/JTAG TMS (plain GPIO by default). GPIO45 is a strapping pin, so do not hold Right during power-on. GPIO19 is USB D− and may interfere with the native USB connection after button initialization.

LCD frame data uses the ESP-IDF `esp_lcd` SPI/GDMA path at 80 MHz. Audio uses the native I2S DMA driver at 32 kHz. The SD card is driven in SPI mode on `SPI3_HOST` (separate from the LCD's `SPI2_HOST`) at up to 40 MHz. The app runs at 240 MHz; WiFi and Bluetooth are disabled in `sdkconfig.defaults`.

## Scope

This is intentionally the small version: no launcher beyond the boot-time SD card picker — once a ROM is running you must reset/re-power the board to pick another one.
