#include <stdlib.h>
#include <string.h>
#include "gnuboy.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif
#include "hw.h"
#include "cpu.h"
#include "sound.h"
#include "lcd.h"

#define hw GB

// Set in the far future for VBA-M support
#define RTC_BASE 1893456000

#define BANK_SIZE 0x4000

static void *alloc_rom_bank(void)
{
#ifdef ESP_PLATFORM
    void *bank = heap_caps_malloc(BANK_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (bank) {
        return bank;
    }
#endif
    return malloc(BANK_SIZE);
}

static void *alloc_cart_ram(size_t banks)
{
#ifdef ESP_PLATFORM
    if (banks * 0x2000 >= 0x8000) {
        void *ram = heap_caps_calloc(banks, 0x2000, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ram) {
            return ram;
        }
    }
#endif
    return calloc(banks, 0x2000);
}

// Note: Eventually we'll just pass a gb_host_t to init...
// But for now assume it's been configured before we were alled!
int gnuboy_init(int samplerate, gb_audio_fmt_t audio_fmt, gb_video_fmt_t video_fmt, gb_video_cb_t *video_callback,
                gb_audio_cb_t *audio_callback)
{
    GB = (gb_t){
        .video.colorize = GB_PALETTE_CGB,
        .video.format = video_fmt,
        .video.callback = video_callback,
        .audio.samplerate = samplerate,
        .audio.format = audio_fmt,
        .audio.callback = audio_callback,
    };
    if (!gb_hw_init()) {
        return -1;
    }
    return 0;
}

/*
 * gnuboy_reset is called to initialize the state of the emulated
 * system. It should set cpu registers, hardware registers, etc. to
 * their appropriate values at power up time.
 */
void gnuboy_reset(bool hard)
{
    gb_hw_reset(hard);
    gb_lcd_reset(hard);
    gb_cpu_reset(hard);
    gb_sound_reset(hard);
}

void gnuboy_set_framebuffer(void *buffer)
{
    GB.video.buffer = buffer;
}

void gnuboy_set_soundbuffer(void *buffer, size_t length)
{
    GB.audio.buffer = buffer;
    GB.audio.len = length;
}

/*
    Time intervals throughout the code, unless otherwise noted, are
    specified in double-speed machine cycles (2MHz), each unit
    roughly corresponds to 0.477us.

    For CPU each cycle takes 2dsc (0.954us) in single-speed mode
    and 1dsc (0.477us) in double speed mode.

    Although hardware gbc LCDC would operate at completely different
    and fixed frequency, for emulation purposes timings for it are
    also specified in double-speed cycles.

    line = 228 dsc (109us)
    frame (154 lines) = 35112 dsc (16.7ms)
    of which
        visible lines x144 = 32832 dsc (15.66ms)
        vblank lines x10 = 2280 dsc (1.08ms)
*/
void gnuboy_run(bool draw)
{
    GB.video.enabled = draw;
    GB.audio.pos = 0;

    int cycles = 0;

    // LCD is powered down, it won't touch LY or do vblank
    if (!(R_LCDC & 0x80)) {
        cycles += 154 * 228;
        cycles -= gb_cpu_emulate(cycles);
        return;
    }

    // We emulate until vblank (0..144)
    while (R_LY <= 144) {
        cycles += 228;
        cycles -= gb_cpu_emulate(cycles);
    }

    /* When using GB_PIXEL_PALETTED, the host should draw the frame in this callback
       because the palette can be modified below before gnuboy_run returns. */
    if (draw && GB.video.callback) {
        (GB.video.callback)(GB.video.buffer);
    }

    gb_hw_vblank();

    // Emulate vblank (145...0)
    while (R_LY > 0) {
        cycles += 228;
        cycles -= gb_cpu_emulate(cycles);
    }

    if (GB.audio.callback && GB.audio.pos > 0) {
        (GB.audio.callback)(GB.audio.buffer, GB.audio.pos);
    }
}

void gnuboy_set_pad(int pad)
{
    if (hw.pad != pad) {
        gb_hw_setpad(pad);
    }
}

int gnuboy_load_bios(const byte *data, size_t size)
{
    if (size > 0x900) {
        MESSAGE_ERROR("Invalid BIOS size.\n");
        return -1;
    }

    if (hw.bios == NULL) {
        hw.bios = malloc(0x900);
    }

    if (!hw.bios) {
        MESSAGE_ERROR("Mem alloc failed.\n");
        return -2;
    }

    memcpy(hw.bios, data, size);

    return 0;
}

void gnuboy_free_bios(void)
{
    free(hw.bios);
    hw.bios = NULL;
}

int gnuboy_load_bios_file(const char *file)
{
    MESSAGE_INFO("Loading BIOS file: '%s'\n", file);
    byte buffer[0x900];

    FILE *fp = fopen(file, "rb");
    if (!fp || fread(buffer, 1, 0x900, fp) < 0x100) {
        MESSAGE_ERROR("File read failed.\n");
        fclose(fp);
        return -1;
    }
    fclose(fp);

    return gnuboy_load_bios(buffer, sizeof(buffer));
}

int gnuboy_load_bank(int bank)
{
    if (!cart.romFile || bank < 0 || bank >= cart.romsize) {
        return -1;
    }
    if (cart.rombanks[bank]) {
        return 0;
    }

    byte *buffer = alloc_rom_bank();
    int victim = -1;
    if (!buffer) {
        int active = cart.rombank & (cart.romsize - 1);
        for (int i = 1; i < cart.romsize; i++) {
            if (i != bank && i != active && cart.rombanks[i]) {
                victim = i;
                buffer = cart.rombanks[i];
                break;
            }
        }
    }
    if (!buffer) {
        MESSAGE_ERROR("No memory or reclaimable slot for ROM bank %d.\n", bank);
        return -2;
    }

    MESSAGE_INFO("loading bank %d.\n", bank);
    clearerr(cart.romFile);
    if (fseek(cart.romFile, (long)bank * BANK_SIZE, SEEK_SET) != 0 ||
        fread(buffer, 1, BANK_SIZE, cart.romFile) != BANK_SIZE) {
        MESSAGE_ERROR("ROM bank %d read failed.\n", bank);
        if (victim >= 0) {
            cart.rombanks[victim] = NULL;
        }
        free(buffer);
        return -3;
    }

    if (victim >= 0) {
        MESSAGE_INFO("reclaiming bank %d.\n", victim);
        cart.rombanks[victim] = NULL;
    }
    cart.rombanks[bank] = buffer;
    return 0;
}

int gnuboy_load_rom(const byte *data, size_t size)
{
    // Memory Bank Controller names
    const char *mbc_names[16] = {
        "MBC_NONE", "MBC_MBC1",  "MBC_MBC2", "MBC_MBC3", "MBC_MBC5", "MBC_MBC6", "MBC_MBC7", "MBC_HUC1",
        "MBC_HUC3", "MBC_MMM01", "INVALID",  "INVALID",  "INVALID",  "INVALID",  "INVALID",  "INVALID",
    };
    const char *hw_types[] = {"DMG", "CGB", "SGB", "AGB", "???"};

    // We need at least the header
    if (size < 0x200) {
        return -1;
    }

    const byte *header = data;
    int type = header[0x0147];
    int romsize = header[0x0148];
    int ramsize = header[0x0149];

    if (header[0x0143] == 0x80 || header[0x0143] == 0xC0) {
        hw.hwtype = GB_HW_CGB; // Game supports CGB mode so we go for that
    } else if (header[0x0146] == 0x03) {
        hw.hwtype = GB_HW_SGB; // Game supports SGB features
    } else {
        hw.hwtype = GB_HW_DMG; // Games supports DMG only
    }

    memcpy(&cart.checksum, header + 0x014E, 2);
    memcpy(&cart.name, header + 0x0134, 16);
    cart.name[16] = 0;

    cart.has_battery = (type == 3 || type == 6 || type == 9 || type == 13 || type == 15 || type == 16 || type == 19 ||
                        type == 27 || type == 30 || type == 255);
    cart.has_rtc = (type == 15 || type == 16);
    cart.has_rumble = (type == 28 || type == 29 || type == 30);
    cart.has_sensor = (type == 34);
    cart.colorize = 0;

    if (type >= 1 && type <= 3) {
        cart.mbc = MBC_MBC1;
    } else if (type >= 5 && type <= 6) {
        cart.mbc = MBC_MBC2;
    } else if (type >= 11 && type <= 13) {
        cart.mbc = MBC_MMM01;
    } else if (type >= 15 && type <= 19) {
        cart.mbc = MBC_MBC3;
    } else if (type >= 25 && type <= 30) {
        cart.mbc = MBC_MBC5;
    } else if (type == 32) {
        cart.mbc = MBC_MBC6;
    } else if (type == 34) {
        cart.mbc = MBC_MBC7;
    } else if (type == 254) {
        cart.mbc = MBC_HUC3;
    } else if (type == 255) {
        cart.mbc = MBC_HUC1;
    } else {
        cart.mbc = MBC_NONE;
    }

    if (romsize < 9) {
        cart.romsize = (2 << romsize);
    } else if (romsize > 0x51 && romsize < 0x55) {
        cart.romsize = 128; // (2 << romsize) + 64;
    } else {
        MESSAGE_ERROR("Invalid ROM size: %d\n", romsize);
        return -2;
    }

    if (ramsize < 6) {
        const byte ramsize_table[] = {1, 1, 1, 4, 16, 8};
        cart.ramsize = ramsize_table[ramsize];
    } else {
        MESSAGE_ERROR("Invalid RAM size: %d\n", ramsize);
        cart.ramsize = 1;
    }

    cart.rambanks = alloc_cart_ram(cart.ramsize);
    cart.rombanks = calloc(cart.romsize, sizeof(byte *));

    if (!cart.rambanks || !cart.rombanks) {
        MESSAGE_ERROR("Memory allocation failed.");
        return -3;
    }

    for (size_t pos = 0; size - pos >= BANK_SIZE; pos += BANK_SIZE) {
        // FIXME: We need a way to tag this as non-freeable...
        cart.rombanks[pos / BANK_SIZE] = (byte *)(data + pos);
    }

    // Detect colorization palette that the real GBC would be using
    if (!IS_CGB) {
        //
        // The following algorithm was adapted from visualboyadvance-m at
        // https://github.com/visualboyadvance-m/visualboyadvance-m/blob/master/src/gb/GB.cpp
        //

        // Title checksums that are treated specially by the CGB boot ROM
        const uint8_t col_checksum[79] = {
            0x00, 0x88, 0x16, 0x36, 0xD1, 0xDB, 0xF2, 0x3C, 0x8C, 0x92, 0x3D, 0x5C, 0x58, 0xC9, 0x3E, 0x70,
            0x1D, 0x59, 0x69, 0x19, 0x35, 0xA8, 0x14, 0xAA, 0x75, 0x95, 0x99, 0x34, 0x6F, 0x15, 0xFF, 0x97,
            0x4B, 0x90, 0x17, 0x10, 0x39, 0xF7, 0xF6, 0xA2, 0x49, 0x4E, 0x43, 0x68, 0xE0, 0x8B, 0xF0, 0xCE,
            0x0C, 0x29, 0xE8, 0xB7, 0x86, 0x9A, 0x52, 0x01, 0x9D, 0x71, 0x9C, 0xBD, 0x5D, 0x6D, 0x67, 0x3F,
            0x6B, 0xB3, 0x46, 0x28, 0xA5, 0xC6, 0xD3, 0x27, 0x61, 0x18, 0x66, 0x6A, 0xBF, 0x0D, 0xF4};

        // The fourth character of the game title for disambiguation on collision.
        const uint8_t col_disambig_chars[29] = {'B', 'E', 'F', 'A', 'A', 'R', 'B', 'E', 'K', 'E',
                                                'K', ' ', 'R', '-', 'U', 'R', 'A', 'R', ' ', 'I',
                                                'N', 'A', 'I', 'L', 'I', 'C', 'E', ' ', 'R'};

        // Palette ID | (Flags << 5)
        const uint8_t col_palette_info[94] = {
            0x7C, 0x08, 0x12, 0xA3, 0xA2, 0x07, 0x87, 0x4B, 0x20, 0x12, 0x65, 0xA8, 0x16, 0xA9, 0x86, 0xB1,
            0x68, 0xA0, 0x87, 0x66, 0x12, 0xA1, 0x30, 0x3C, 0x12, 0x85, 0x12, 0x64, 0x1B, 0x07, 0x06, 0x6F,
            0x6E, 0x6E, 0xAE, 0xAF, 0x6F, 0xB2, 0xAF, 0xB2, 0xA8, 0xAB, 0x6F, 0xAF, 0x86, 0xAE, 0xA2, 0xA2,
            0x12, 0xAF, 0x13, 0x12, 0xA1, 0x6E, 0xAF, 0xAF, 0xAD, 0x06, 0x4C, 0x6E, 0xAF, 0xAF, 0x12, 0x7C,
            0xAC, 0xA8, 0x6A, 0x6E, 0x13, 0xA0, 0x2D, 0xA8, 0x2B, 0xAC, 0x64, 0xAC, 0x6D, 0x87, 0xBC, 0x60,
            0xB4, 0x13, 0x72, 0x7C, 0xB5, 0xAE, 0xAE, 0x7C, 0x7C, 0x65, 0xA2, 0x6C, 0x64, 0x85};

        uint8_t infoIdx = 0;
        uint8_t checksum = 0;

        // Calculate the checksum over 16 title bytes.
        for (int i = 0; i < 16; i++) {
            checksum += header[0x0134 + i];
        }

        // Check if the checksum is in the list.
        for (size_t idx = 0; idx < 79; idx++) {
            if (col_checksum[idx] == checksum) {
                infoIdx = idx;

                // Indexes above 0x40 have to be disambiguated.
                if (idx <= 0x40) {
                    break;
                }

                // No idea how that works. But it works.
                for (size_t i = idx - 0x41, j = 0; i < 29; i += 14, j += 14) {
                    if (header[0x0137] == col_disambig_chars[i]) {
                        infoIdx += j;
                        break;
                    }
                }
                break;
            }
        }

        cart.colorize = col_palette_info[infoIdx];
    }

    MESSAGE_INFO("Cart loaded: name='%s', hw=%s, mbc=%s, romsize=%dK, ramsize=%dK, colorize=%d\n", cart.name,
                 hw_types[hw.hwtype], mbc_names[cart.mbc], cart.romsize * 16, cart.ramsize * 8, cart.colorize);

    // Apply game-specific hacks
    if (memcmp(cart.name, "SIREN GB2 ", 10) == 0 || memcmp(cart.name, "DONKEY KONG", 12) == 0) {
        MESSAGE_INFO("HACK: Window offset hack enabled (12)\n");
        hw.compat.window_offset = 12;
    } else if (memcmp(cart.name, "RES EVIL GD", 11) == 0 || memcmp(cart.name, "BIOHAZARDGDB", 12) == 0) {
        MESSAGE_INFO("HACK: Window offset hack enabled (10)\n");
        hw.compat.window_offset = 10;
    } else {
        hw.compat.window_offset = 0;
    }

    return 0;
}

int gnuboy_load_rom_file(const char *file)
{
    MESSAGE_INFO("Loading file: '%s'\n", file);

    byte header[0x200];

    cart.romFile = fopen(file, "rb");
    if (cart.romFile == NULL) {
        MESSAGE_ERROR("ROM fopen failed\n");
        return -1;
    }

    if (fread(&header, 0x200, 1, cart.romFile) != 1) {
        MESSAGE_ERROR("ROM fread failed\n");
        fclose(cart.romFile);
        cart.romFile = NULL;
        return -1;
    }

    int ret = gnuboy_load_rom(header, 0x200);
    if (ret != 0) {
        MESSAGE_ERROR("ROM setup failed\n");
        gnuboy_free_rom();
        return ret;
    }

    // Gameboy color games can be very large so we preload a maximum of 128 banks for faster boot
    // Also 4/8MB games do not fully fit anyway, we need to leave room for our bank manager's swapping.

    int preload = cart.romsize < 128 ? cart.romsize : 128;

    if (cart.romsize > 64 && (strncmp(cart.name, "RAYMAN", 6) == 0 || strncmp(cart.name, "NONAME", 6) == 0)) {
        MESSAGE_INFO("Special preloading for Rayman 1/2\n");
        preload = cart.romsize - 40;
    }

    MESSAGE_INFO("Preloading the first %d banks\n", preload);
    for (int i = 0; i < preload; i++) {
        if (gnuboy_load_bank(i) < 0) {
            gnuboy_free_rom();
            return -5;
        }
    }

    return 0;
}

void gnuboy_free_rom(void)
{
    // If cart.romFile isn't NULL it indicates that we haven't allocated those buffers, don't free them.
    if (cart.romFile && cart.rombanks) {
        for (int i = 0; i < cart.romsize; i++) {
            free(cart.rombanks[i]);
        }
    }

    free(cart.rombanks);
    cart.rombanks = NULL;

    free(cart.rambanks);
    cart.rambanks = NULL;

    if (cart.romFile) {
        fclose(cart.romFile);
        cart.romFile = NULL;
    }

    if (cart.sramFile) {
        fclose(cart.sramFile);
        cart.sramFile = NULL;
    }

    memset(&cart, 0, sizeof(cart));
}

void gnuboy_get_time(int *day, int *hour, int *minute, int *second)
{
    if (day) {
        *day = cart.rtc.d;
    }
    if (hour) {
        *hour = cart.rtc.h;
    }
    if (minute) {
        *minute = cart.rtc.m;
    }
    if (second) {
        *second = cart.rtc.s;
    }
}

void gnuboy_set_time(int day, int hour, int minute, int second)
{
    cart.rtc.d = day % 365;
    cart.rtc.h = hour % 24;
    cart.rtc.m = minute % 60;
    cart.rtc.s = second % 60;
    cart.rtc.ticks = 0;
    cart.rtc.dirty = 0;
}

int gnuboy_get_hwtype(void)
{
    return hw.hwtype;
}

void gnuboy_set_hwtype(gb_hwtype_t type)
{
    // nothing for now
}

int gnuboy_get_palette(void)
{
    return GB.video.colorize;
}

void gnuboy_set_palette(gb_palette_t pal)
{
    GB.video.colorize = pal;
    gb_lcd_pal_dirty();
}

bool gnuboy_sram_dirty(void)
{
    return cart.has_battery && (cart.sram_dirty != 0 || cart.rtc.dirty != 0);
}

int gnuboy_load_sram(const char *file)
{
    if (!cart.has_battery || !cart.ramsize || !file || !*file) {
        return -1;
    }

    FILE *f = fopen(file, "rb");
    if (!f) {
        return -2;
    }

    MESSAGE_INFO("Loading SRAM from '%s'\n", file);

    cart.sram_dirty = 0;
    cart.sram_saved = 0;
    cart.rtc.dirty = 0;

    for (int i = 0; i < cart.ramsize; i++) {
        if (fseek(f, i * 8192, SEEK_SET) == 0 && fread(cart.rambanks[i], 8192, 1, f) == 1) {
            MESSAGE_INFO("Loaded SRAM bank %d.\n", i);
            cart.sram_saved |= (1 << i);
        }
    }

    if (cart.has_rtc) {
        uint32_t rtc_buf[12];

        if (fseek(f, cart.ramsize * 8192, SEEK_SET) == 0 && fread(&rtc_buf, 48, 1, f) == 1) {
            cart.rtc = (gb_rtc_t){
                .s = rtc_buf[0],
                .m = rtc_buf[1],
                .h = rtc_buf[2],
                .d = rtc_buf[3],
                .flags = rtc_buf[4],
                .regs = {rtc_buf[5], rtc_buf[6], rtc_buf[7], rtc_buf[8], rtc_buf[9]},
            };
            MESSAGE_INFO("Loaded RTC section %03d %02d:%02d:%02d.\n", cart.rtc.d, cart.rtc.h, cart.rtc.m, cart.rtc.s);
        }
    }

    fclose(f);

    return cart.sram_saved ? 0 : -1;
}

/**
 * If quick_save is set to true, sram_save will only save the sectors that
 * changed + the rtc. If set to false then a full sram file is created.
 */
int gnuboy_save_sram(const char *file, bool quick_save)
{
    if (!cart.has_battery || !cart.ramsize || !file || !*file) {
        return -1;
    }

    FILE *f = fopen(file, "wb");
    if (!f) {
        return -2;
    }

    MESSAGE_INFO("Saving SRAM to '%s'...\n", file);

    // Mark everything as dirty and unsaved (do a full save)
    if (!quick_save) {
        cart.sram_dirty = (1 << cart.ramsize) - 1;
        cart.sram_saved = 0;
    }

    for (int i = 0; i < cart.ramsize; i++) {
        if (!(cart.sram_saved & (1 << i)) || (cart.sram_dirty & (1 << i))) {
            if (fseek(f, i * 8192, SEEK_SET) == 0 && fwrite(cart.rambanks[i], 8192, 1, f) == 1) {
                MESSAGE_INFO("Saved SRAM bank %d.\n", i);
                cart.sram_dirty &= ~(1 << i);
                cart.sram_saved |= (1 << i);
            }
        }
    }

    if (cart.has_rtc) {
        uint64_t rt = RTC_BASE + cart.rtc.s + (cart.rtc.m * 60) + (cart.rtc.h * 3600) + (cart.rtc.d * 86400);
        uint32_t *rtp = (uint32_t *)&rt;
        uint32_t rtc_buf[12] = {
            cart.rtc.s,       cart.rtc.m,       cart.rtc.h,       cart.rtc.d,       cart.rtc.flags, cart.rtc.regs[0],
            cart.rtc.regs[1], cart.rtc.regs[2], cart.rtc.regs[3], cart.rtc.regs[4], rtp[0],         rtp[1],
        };
        if (fseek(f, cart.ramsize * 8192, SEEK_SET) == 0 && fwrite(&rtc_buf, 48, 1, f) == 1) {
            MESSAGE_INFO("Saved RTC section.\n");
            cart.rtc.dirty = 0;
        } else {
            cart.rtc.dirty = 1;
        }
    }

    fclose(f);

    return (cart.sram_dirty || cart.rtc.dirty) ? -1 : 0;
}

/**
 * Save state file format is:
 * GB:
 * 0x0000 - 0x0BFF: svars
 * 0x0CF0 - 0x0CFF: hw.snd->wave
 * 0x0D00 - 0x0DFF: hw.ioregs
 * 0x0E00 - 0x0E80: lcd.pal
 * 0x0F00 - 0x0FFF: lcd.oam
 * 0x1000 - 0x2FFF: RAM
 * 0x3000 - 0x4FFF: VRAM
 * 0x5000 - 0x...:  SRAM
 *
 * GBC:
 * 0x0000 - 0x0BFF: svars
 * 0x0CF0 - 0x0CFF: hw.snd->wave
 * 0x0D00 - 0x0DFF: hw.ioregs
 * 0x0E00 - 0x0EFF: lcd.pal
 * 0x0F00 - 0x0FFF: lcd.oam
 * 0x1000 - 0x8FFF: RAM
 * 0x9000 - 0xCFFF: VRAM
 * 0xD000 - 0x...:  SRAM
 *
 */

#ifndef IS_BIG_ENDIAN
#define LIL(x) (x)
#else
#define LIL(x) ((x << 24) | ((x & 0xff00) << 8) | ((x >> 8) & 0xff00) | (x >> 24))
#endif

#define SAVE_VERSION 0x107

#define I1(s, p) {1, s, p}
#define I2(s, p) {2, s, p}
#define I4(s, p) {4, s, p}

enum {
    STATE_BLOCK_SIZE = 4096,
    STATE_HEADER_SIZE = 4096,
    STATE_HEADER_DATA_END = 0xCF0,
    STATE_VARIABLE_CAPACITY = 64,
};

typedef struct {
    size_t len;
    char key[4];
    void *ptr;
} svar_t;

typedef struct {
    void *ptr;
    size_t len;
} sblock_t;

static size_t state_variables(svar_t variables[STATE_VARIABLE_CAPACITY])
{
    const svar_t values[] = {
        I2("PC  ", &W(hw.cpu->pc)),
        I2("SP  ", &W(hw.cpu->sp)),
        I2("BC  ", &W(hw.cpu->bc)),
        I2("DE  ", &W(hw.cpu->de)),
        I2("HL  ", &W(hw.cpu->hl)),
        I2("AF  ", &W(hw.cpu->af)),

        I4("IME ", &hw.cpu->ime),
        I4("ima ", &hw.cpu->ima),
        I4("spd ", &hw.cpu->double_speed),
        I4("halt", &hw.cpu->halted),
        I4("div ", &hw.cpu->div),
        I4("tim ", &hw.cpu->timer),
        I4("lcdc", &hw.cycles),
        I4("snd ", &hw.snd->cycles),

        I4("ints", &hw.ilines),
        I4("pad ", &hw.pad),
        I4("hdma", &hw.hdma),
        I4("seri", &hw.serial),

        I4("mbcm", &hw.cart->bankmode),
        I4("romb", &hw.cart->rombank),
        I4("ramb", &hw.cart->rambank),
        I4("enab", &hw.cart->enableram),

        I4("rtcR", &hw.cart->rtc.sel),
        I4("rtcL", &hw.cart->rtc.latch),
        I4("rtcF", &hw.cart->rtc.flags),
        I4("rtcd", &hw.cart->rtc.d),
        I4("rtch", &hw.cart->rtc.h),
        I4("rtcm", &hw.cart->rtc.m),
        I4("rtcs", &hw.cart->rtc.s),
        I4("rtct", &hw.cart->rtc.ticks),
        I1("rtR8", &hw.cart->rtc.regs[0]),
        I1("rtR9", &hw.cart->rtc.regs[1]),
        I1("rtRA", &hw.cart->rtc.regs[2]),
        I1("rtRB", &hw.cart->rtc.regs[3]),
        I1("rtRC", &hw.cart->rtc.regs[4]),

        I4("S1on", &hw.snd->ch[0].on),
        I4("S1p ", &hw.snd->ch[0].pos),
        I4("S1c ", &hw.snd->ch[0].cnt),
        I4("S1ec", &hw.snd->ch[0].encnt),
        I4("S1sc", &hw.snd->ch[0].swcnt),
        I4("S1sf", &hw.snd->ch[0].swfreq),
        I4("S1ev", &hw.snd->ch[0].envol),

        I4("S2on", &hw.snd->ch[1].on),
        I4("S2p ", &hw.snd->ch[1].pos),
        I4("S2c ", &hw.snd->ch[1].cnt),
        I4("S2ec", &hw.snd->ch[1].encnt),
        I4("S2ev", &hw.snd->ch[1].envol),

        I4("S3on", &hw.snd->ch[2].on),
        I4("S3p ", &hw.snd->ch[2].pos),
        I4("S3c ", &hw.snd->ch[2].cnt),

        I4("S4on", &hw.snd->ch[3].on),
        I4("S4p ", &hw.snd->ch[3].pos),
        I4("S4c ", &hw.snd->ch[3].cnt),
        I4("S4ec", &hw.snd->ch[3].encnt),
        I4("S4ev", &hw.snd->ch[3].envol),
    };

    _Static_assert(sizeof(values) / sizeof(values[0]) <= STATE_VARIABLE_CAPACITY, "state variable capacity too small");
    memcpy(variables, values, sizeof(values));
    return sizeof(values) / sizeof(values[0]);
}

static uint32_t state_key(const char key[4])
{
    uint32_t value;
    memcpy(&value, key, sizeof(value));
    return value;
}

static uint32_t state_variable_value(const svar_t *variable)
{
    switch (variable->len) {
    case 1:
        return *(uint8_t *)variable->ptr;
    case 2:
        return *(uint16_t *)variable->ptr;
    case 4:
        return *(uint32_t *)variable->ptr;
    default:
        return 0;
    }
}

static void state_set_variable(const svar_t *variable, uint32_t value)
{
    switch (variable->len) {
    case 1:
        *(uint8_t *)variable->ptr = value;
        break;
    case 2:
        *(uint16_t *)variable->ptr = value;
        break;
    case 4:
        *(uint32_t *)variable->ptr = value;
        break;
    }
}

static bool state_find_value(const byte *header, uint32_t wanted, uint32_t *value)
{
    for (size_t i = 1; i < STATE_HEADER_DATA_END / 8; i++) {
        uint32_t key;
        memcpy(&key, header + i * 8, sizeof(key));
        if (key == 0) {
            return false;
        }
        if (key == wanted) {
            memcpy(value, header + i * 8 + 4, sizeof(*value));
            *value = LIL(*value);
            return true;
        }
    }
    return false;
}

static bool state_header_version(const byte *header, uint32_t *version)
{
    uint32_t magic;
    memcpy(&magic, header, sizeof(magic));
    memcpy(version, header + 4, sizeof(*version));
    *version = LIL(*version);
    return magic == state_key("GbSs");
}

static bool state_header_valid(const byte *header)
{
    uint32_t version;
    if (!state_header_version(header, &version) || version != SAVE_VERSION) {
        return false;
    }

    svar_t variables[STATE_VARIABLE_CAPACITY];
    size_t count = state_variables(variables);
    for (size_t i = 0; i < count; i++) {
        uint32_t value;
        if (!state_find_value(header, state_key(variables[i].key), &value)) {
            return false;
        }
    }
    return true;
}

static void state_write_header(byte *header)
{
    memset(header, 0, STATE_HEADER_SIZE);

    uint32_t magic = state_key("GbSs");
    uint32_t version = LIL(SAVE_VERSION);
    memcpy(header, &magic, sizeof(magic));
    memcpy(header + 4, &version, sizeof(version));

    svar_t variables[STATE_VARIABLE_CAPACITY];
    size_t count = state_variables(variables);
    for (size_t i = 0; i < count; i++) {
        uint32_t key = state_key(variables[i].key);
        uint32_t value = LIL(state_variable_value(&variables[i]));
        memcpy(header + (i + 1) * 8, &key, sizeof(key));
        memcpy(header + (i + 1) * 8 + 4, &value, sizeof(value));
    }

    memcpy(header + 0xCF0, hw.snd->wave, 16);
    memcpy(header + 0xD00, hw.ioregs, 256);
    memcpy(header + 0xE00, hw.pal, 128);
    memcpy(header + 0xF00, hw.oam, 256);
}

static void state_restore_header(const byte *header, bool file_compat)
{
    svar_t variables[STATE_VARIABLE_CAPACITY];
    size_t count = state_variables(variables);
    for (size_t i = 0; i < count; i++) {
        uint32_t value = 0;
        state_find_value(header, state_key(variables[i].key), &value);
        state_set_variable(&variables[i], value);
    }

    memcpy(hw.snd->wave, header + 0xCF0, 16);
    memcpy(hw.ioregs, header + 0xD00, 256);
    memcpy(hw.pal, header + 0xE00, 128);
    memcpy(hw.oam, header + 0xF00, 256);

    if (file_compat) {
        R_BIOS = 0x1;
    }

    cart.rambank &= (cart.ramsize - 1);
    GB.audio.pos = 0;
    gb_lcd_pal_dirty();
    gb_sound_dirty();

    uint32_t envelope;
    if (state_find_value(header, state_key("S1ev"), &envelope)) {
        hw.snd->ch[0].envol = envelope;
    }
    if (state_find_value(header, state_key("S2ev"), &envelope)) {
        hw.snd->ch[1].envol = envelope;
    }
    if (state_find_value(header, state_key("S4ev"), &envelope)) {
        hw.snd->ch[3].envol = envelope;
    }

    gb_hw_updatemap();
}

static bool rtc_state_equal(const gb_rtc_t *a, const gb_rtc_t *b)
{
    if (a->sel != b->sel || a->flags != b->flags || a->latch != b->latch || a->ticks != b->ticks || a->d != b->d ||
        a->h != b->h || a->m != b->m || a->s != b->s) {
        return false;
    }
    for (int i = 0; i < 5; i++) {
        if (a->regs[i] != b->regs[i]) {
            return false;
        }
    }
    return true;
}

size_t gnuboy_state_size(void)
{
    if (!hw.cpu || !hw.snd || !hw.rambanks || !hw.vbanks || !cart.rambanks || cart.ramsize < 1) {
        return 0;
    }

    size_t wram_size = (IS_CGB ? 8 : 2) * STATE_BLOCK_SIZE;
    size_t vram_size = (IS_CGB ? 4 : 2) * STATE_BLOCK_SIZE;
    return STATE_HEADER_SIZE + wram_size + vram_size + (size_t)cart.ramsize * 8192;
}

int gnuboy_save_state_mem(void *buffer, size_t size)
{
    size_t expected = gnuboy_state_size();
    if (!buffer || expected == 0 || size < expected) {
        return -1;
    }

    byte *p = buffer;
    state_write_header(p);
    p += STATE_HEADER_SIZE;

    size_t wram_size = (IS_CGB ? 8 : 2) * STATE_BLOCK_SIZE;
    size_t vram_size = (IS_CGB ? 4 : 2) * STATE_BLOCK_SIZE;
    size_t cart_ram_size = (size_t)cart.ramsize * 8192;
    memcpy(p, hw.rambanks, wram_size);
    p += wram_size;
    memcpy(p, hw.vbanks, vram_size);
    p += vram_size;
    memcpy(p, cart.rambanks, cart_ram_size);
    p += cart_ram_size;

    return (int)(p - (byte *)buffer);
}

int gnuboy_load_state_mem(const void *buffer, size_t size)
{
    size_t expected = gnuboy_state_size();
    if (!buffer || expected == 0 || size != expected || !state_header_valid(buffer)) {
        return -1;
    }

    const byte *p = (const byte *)buffer + STATE_HEADER_SIZE;
    size_t wram_size = (IS_CGB ? 8 : 2) * STATE_BLOCK_SIZE;
    size_t vram_size = (IS_CGB ? 4 : 2) * STATE_BLOCK_SIZE;
    size_t cart_ram_size = (size_t)cart.ramsize * 8192;

    memcpy(hw.rambanks, p, wram_size);
    p += wram_size;
    memcpy(hw.vbanks, p, vram_size);
    p += vram_size;

    unsigned changed_banks = 0;
    if (cart.has_battery) {
        for (int i = 0; i < cart.ramsize; i++) {
            if (memcmp(cart.rambanks[i], p + (size_t)i * 8192, 8192) != 0) {
                changed_banks |= 1u << i;
            }
        }
    }
    memcpy(cart.rambanks, p, cart_ram_size);

    gb_rtc_t previous_rtc = cart.rtc;
    state_restore_header(buffer, false);
    cart.sram_dirty |= changed_banks;
    if (cart.has_rtc && !rtc_state_equal(&previous_rtc, &cart.rtc)) {
        cart.rtc.dirty = 1;
    }

    return 0;
}

static int do_save_load(const char *file, bool save)
{
    byte *buf = calloc(1, STATE_HEADER_SIZE);
    if (!buf) {
        return -2;
    }

    sblock_t blocks[] = {
        {buf, 1},  {hw.rambanks, IS_CGB ? 8 : 2}, {hw.vbanks, IS_CGB ? 4 : 2}, {cart.rambanks, cart.ramsize * 2},
        {NULL, 0},
    };

    FILE *fp = NULL;

    if (save) {
        if (!(fp = fopen(file, "wb"))) {
            goto _error;
        }

        state_write_header(buf);
        for (int i = 0; blocks[i].ptr != NULL; i++) {
            if (fwrite(blocks[i].ptr, STATE_BLOCK_SIZE, blocks[i].len, fp) != blocks[i].len) {
                MESSAGE_ERROR("Write error in block %d\n", i);
                goto _error;
            }
        }
    } else {
        if (!(fp = fopen(file, "rb"))) {
            goto _error;
        }

        for (int i = 0; blocks[i].ptr != NULL; i++) {
            if (fread(blocks[i].ptr, STATE_BLOCK_SIZE, blocks[i].len, fp) != blocks[i].len) {
                MESSAGE_ERROR("Read error in block %d\n", i);
                goto _error;
            }
            if (i == 0) {
                uint32_t version;
                if (!state_header_version(buf, &version)) {
                    MESSAGE_ERROR("Save file header mismatch!\n");
                    goto _error;
                }
                if (version != SAVE_VERSION) {
                    MESSAGE_WARN("Save file version mismatch: %u != %u\n", (unsigned)version, SAVE_VERSION);
                }
            }
        }

        state_restore_header(buf, true);
    }

    fclose(fp);
    free(buf);
    return 0;

_error:
    if (fp) {
        fclose(fp);
    }
    free(buf);
    return -1;
}

int gnuboy_save_state(const char *file)
{
    return do_save_load(file, true);
}

int gnuboy_load_state(const char *file)
{
    return do_save_load(file, false);
}
