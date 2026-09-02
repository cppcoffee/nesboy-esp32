# nesboy-esp32

Minimal ESP-IDF NES, Game Boy, Game Boy Color, Game Boy Advance, and Super Nintendo emulator for ESP32-S3.

NES runs at a full **60 FPS** (NTSC), driven by audio-paced frame timing on the 240 MHz dual-core ESP32-S3. GBA and SNES target **30 display FPS**; SNES preserves the ROM's native 50/60 Hz emulation and audio rate while scheduling rendered frames at 30 Hz. On boot, the ROM browser scans the SD card and accepts `.nes`, `.gb`, `.gbc`, `.gba`, and `.sfc`/`.smc`/`.swc`/`.fig` files; there is no embedded fallback ROM. It uses the ST7789/I2S/button wiring defined in `main/pins.h`, and reuses the `nofrendo` and `gnuboy` cores from `retro-goretro-go`, a `gpSP` interpreter core (`components/gba`), and a trimmed `snes9x` core (`components/snes9x`).

## Playing Games from the SD Card

The ROM used at runtime is chosen from the SD card at boot. Changing games is now just a file copy — no rebuild needed.

### Setup

1. **Format the SD card as FAT32** (most cards come pre-formatted; a 32 GB card works fine). The ESP-IDF FATFS in this build does *not* support exFAT, so exFAT-formatted cards must be reformatted to FAT32 first.
2. **Copy `.nes`, `.gb`, `.gbc`, `.gba`, or `.sfc`/`.smc` files** to the card. NES files are read up to 2 MB; Game Boy ROM banks are loaded from the card into PSRAM as needed; GBA ROMs stream from the card with an 8 MB PSRAM cache; SNES ROMs up to 6 MB are loaded into PSRAM. Other files are hidden from the browser.
3. Insert the card and power on the board.

### Using the browser

The browser appears at boot, listing directories and `.nes`, `.gb`, `.gbc`, `.gba`, and `.sfc`/`.smc`/`.swc`/`.fig` ROM files (directories first, then files, alphabetically; extension matching is case-insensitive). It handles FAT filesystems that report directory entries as `DT_UNKNOWN` and supports up to 1024 visible entries per folder. It is a one-shot startup picker: after a ROM is selected, the browser is gone for the rest of the session — to pick another game, reset/re-power the ESP32.

While a ROM is highlighted, the browser shows box art in the bottom-right corner: a 24/32-bit uncompressed BMP named after the ROM without its extension (e.g. `smb.nes` → `smb.bmp`). Any image can be converted with ImageMagick (`magick cover.png -resize 192x192 smb.bmp`) or `sips -s format bmp cover.png --out smb.bmp`; the browser scales it automatically. Missing or unreadable images are simply skipped.

| Button | Action |
| --- | --- |
| Up / Down | Move the cursor (holds auto-repeat) |
| A / Right | Enter a folder, or select the highlighted ROM and start the game |
| B / Left / Select | Go up to the parent folder (no action at the root) |
| Start | Refresh the current folder |

### Wiring

| MicroSD breakout | ESP32-S3 GPIO |
| --- | ---: |
| CS | 14 |
| MOSI | 13 |
| CLK | 12 |
| MISO | 11 |
| 3.3 V | 3.3 V |
| GND | GND |

The SD card runs on a separate SPI bus (**SPI3_HOST**) from the LCD (**SPI2_HOST**), at up to 40 MHz (the exact speed is negotiated with the card and reported at boot). GPIO11–14 are plain GPIOs on the ESP32-S3, so the pull-up resistors on the breakout do not interfere with boot — the classic-ESP32 "GPIO12/MTDI strapping" warning does **not** apply to this board.

### No embedded ROM

There is no fallback ROM compiled into the firmware: a game must always be picked from the SD card. If the card is missing, unreadable, or the selection fails, the device shows an error on the screen and halts — insert the card and reset.

### Game Boy notes

- `.gb` and `.gbc` use the `gnuboy` core. CGB-capable cartridges automatically run in color mode.
- The native 160×144 image is scaled to 240×216 with an exact nearest-neighbor 3:2 expansion and centered on the 240×240 display. The scaler expands each two-pixel pair directly and copies duplicate rows instead of recalculating all 51,840 output pixels.
- Stereo audio uses the same 48 kHz I2S output and volume controls as NES.
- Battery RAM is not read from or periodically written to the SD card during gameplay; use a save state for persistence across power-off.
- The dedicated Rewind button works for GB and GBC games as well as NES.

### Game Boy Advance notes

- `.gba` uses the `gpSP` interpreter core (no BIOS file needed — an open-source BIOS is compiled in).
- The native 240×160 image is drawn 1:1 and centered vertically (40 black rows top and bottom).
- The core renders at 60 Hz internally; each 30 Hz display frame runs two emulated frames with the audio from both merged and resampled 32.768 kHz → 48 kHz (linear interpolation, phase-continuous).
- The interpreter is single-core; demanding 3D titles may occasionally drop below 30 FPS. Frameskip is not automatic.
- Battery RAM is not written to the SD card during gameplay; use a save state for persistence across power-off.
- Save states and rewind work; each GBA snapshot is 416 KB, so rewind keeps 5 slots (15 s of history). The ROM cache tops out at 6 MB; larger ROMs stream 32 KB pages from the SD card and, if PSRAM runs short, rewind disables itself gracefully.

### Super Nintendo notes

- `.sfc`, `.smc`, `.swc`, and `.fig` use a trimmed `snes9x` interpreter core (Snes9x license, see `components/snes9x/src/LICENSE`).
- The native 256×224 image is scaled horizontally to 240 columns with nearest-neighbor (15:16 — one source column dropped per 16) and centered vertically (8 black rows top and bottom). Mode 5/6 output is reduced to 256 columns inside the renderer, and interlaced output uses one field to fit the fixed 256×239 framebuffer safely.
- Rendering targets 30 FPS while emulation and 32 kHz stereo audio retain the ROM's native 60 Hz (NTSC) or 50 Hz (PAL) timing; audio is resampled continuously to the 48 kHz output.
- Special-chip games are **not** supported by this trimmed core: no SuperFX (Star Fox, Yoshi's Island), no SA-1 (Super Mario RPG), no SDD-1, no SPC7110, and no DSP-1 (Mario Kart's OK/only partly). Standard LoROM/HiROM games work.
- Battery RAM is not written to the SD card during gameplay; use a save state for persistence across power-off.
- Save states and rewind work; each SNES snapshot is ~357 KB, so rewind keeps 5 slots (15 s of history). The ROM buffer is capped at 4 MB to make room, so 6 MB cartridges (Tales of Phantasia, Star Ocean, ...) cannot load.

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

- Supported ROM formats are `.nes`, `.gb`, `.gbc`, `.gba`, and `.sfc`/`.smc`/`.swc`/`.fig`; compressed archives are not supported.
- If the screen stays white or black after selecting a ROM, the ROM may be incompatible with the selected core.

## Build

Prerequisite: ESP-IDF environment loaded.

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/tty.usbmodemXXXX flash monitor
```

## Rewind

For NES, GB, GBC, GBA, and SNES games, a dedicated **Rewind** button (GPIO 8, active-low) scrubs the game backward while held. The longer you hold it, the farther back it goes, in roughly 1-second steps; releasing the button stops the rewind and continues from the restored point. At the oldest snapshot the playback wraps around and cycles back to the newest one, so holding the button keeps looping through the ring.

While the button is held, normal gameplay and audio output are paused. Each rewind step restores an older snapshot, previews a frame, and restores the snapshot again so gameplay continues from the selected point when the button is released.

Internally the emulator captures one in-memory state snapshot every 3 seconds into a ring buffer, so history length is slots × 3 seconds. NES snapshots are roughly 15 KB for mapper-0 CHR-ROM games with 8 KB PRG RAM; GB/GBC snapshots vary with cartridge RAM from about 28 KB to 180 KB each; GBA snapshots are 416 KB each and SNES snapshots ~357 KB. To bound the PSRAM budget, the ring depth is 5 slots for every core (15 s of history); the SNES ROM buffer is capped at 4 MB and the GBA ROM cache at 6 MB to make that fit. Rewind slots live in PSRAM (octal PSRAM is enabled in this build) via explicit `MALLOC_CAP_SPIRAM` allocation; if PSRAM runs short (large GBA ROMs), as many slots are allocated as fit and the history shortens instead of rewind disabling itself — only a total allocation failure turns rewind off. The log after a ROM loads reports the exact slot size, total rewind allocation, and remaining PSRAM/internal RAM.

### Memory lifecycle

The boot-time browser supports up to 1024 directories/ROMs per folder and allocates about 129 KiB for that entry list plus a 112.5 KiB 240×240 RGB565 UI framebuffer in PSRAM. Both allocations are freed when a ROM is selected, before emulator and rewind buffers are allocated. The UI framebuffer is recreated only if a later ROM-load error must be displayed.

Runtime allocations are intentionally retained: framebuffer/audio memory, emulated WRAM/VRAM/cartridge RAM, active rewind slots, and gnuboy's ROM-bank cache are all used while playing. GBC ROM banks are cached in PSRAM (up to 128 preloaded 16 KB banks, about 2 MB) to avoid SD-card stalls; reducing that cache would trade memory for possible frame/audio hiccups rather than being a safe deallocation.

## Save States

Both emulator cores expose save-state serializers for CPU, RAM, video, mapper, and battery-backed cartridge state. Two button combos persist that state to the SD card and restore it:

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

When enabled, one summary is printed every 600 outer-loop frames for every emulator. The original Game Boy timing is about 59.73 FPS, so a healthy GB/GBC result is approximately 59.7 FPS rather than exactly 60.0; GBA and SNES target 30.0 display FPS.

The log reports `fps`, average and maximum `emulate` work time, and `audio_wait`. Audio-queue blocking is measured where it actually occurs and excluded from `emulate`, so the two values can be used to tell CPU/GPU work from normal audio pacing.

The full 240×240 screen is filled each frame (NES overscan is not cropped) using two 120-line DMA chunks with double buffering; keeping the chunk count at two is what preserves the 60 FPS budget.

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

LCD frame data uses the ESP-IDF `esp_lcd` SPI/GDMA path at 80 MHz. Audio uses the native I2S DMA driver at 48 kHz. The SD card is driven in SPI mode on `SPI3_HOST` (separate from the LCD's `SPI2_HOST`) at up to 40 MHz. The app runs at 240 MHz; WiFi and Bluetooth are disabled in `sdkconfig.defaults`.

## Scope

This is intentionally the small version: no launcher beyond the boot-time SD card picker — once a ROM is running you must reset/re-power the board to pick another one.
