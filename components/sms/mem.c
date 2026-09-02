/* SMS/GG memory map, cartridge paging, and I/O ports. */
#include <string.h>

#include "mem.h"
#include "sms_internal.h"
#include "vdp.h"
#include "psg.h"

/* ---- Cartridge paging ----
 *
 * Sega mapper: three 8 KB control registers select which 8 KB page of the
 * ROM appears at 0x0000/0x0400/0x0800/0x1000... Actually the map is:
 *   0x0000-0x03FF: fixed to first page (or page selected by reg 0 on 512K+)
 *   0x0400-0x3FFF: page[1] (reg 0)
 *   0x4000-0x7FFF: page[2] (reg 1)
 *   0x8000-0xBFFF: page[3] (reg 2), lower bits also select SRAM bank
 *   0xC000-0xFFFF: 8 KB system RAM (mirrored through 0xDFFF)
 *
 * Codemasters mapper uses writes to 0x0000/0x2000/0x6000/0x8000 instead.
 */
#define PAGE_SIZE  0x4000 /* we track in 16 KB units internally? no: 8 KB */
#define PAGE_SHIFT 13

static const uint8_t *page_ptr(int index)
{
    int pages = (int)(sms.rom_size >> PAGE_SHIFT); /* 8 KB pages */
    if (pages <= 0) {
        return sms.rom;
    }
    index &= pages - 1;
    return sms.rom + ((size_t)index << PAGE_SHIFT);
}

static void update_map(void)
{
    sms.map[0] = page_ptr(sms.page[0]);
    sms.map[1] = page_ptr(sms.page[1]);
    sms.map[2] = page_ptr(sms.page[2]);
    sms.map[3] = sms.sram_enabled ? (const uint8_t *)sms.sram : page_ptr(sms.page[2] + 1);
}

uint8_t sms_read8(uint16_t addr)
{
    addr &= 0xFFFF;
    if (addr < 0xC000) {
        uint8_t slot = (uint8_t)(addr >> PAGE_SHIFT);
        return sms.map[slot][addr];
    }
    return sms.ram[addr & 0x1FFF];
}

void sms_write8(uint16_t addr, uint8_t value)
{
    addr &= 0xFFFF;
    if (addr >= 0xC000) {
        sms.ram[addr & 0x1FFF] = value;
        return;
    }

    switch (sms.mapper) {
    case MAPPER_SEGA:
        switch (addr) {
        case 0xFFFC:
            sms.sram_enabled = (value & 0x08) != 0;
            sms.sram_bank = value & 0x04;
            update_map();
            break;
        case 0xFFFD:
            sms.page[0] = value;
            update_map();
            break;
        case 0xFFFE:
            sms.page[1] = value;
            update_map();
            break;
        case 0xFFFF:
            sms.page[2] = value;
            update_map();
            break;
        default:
            break;
        }
        break;

    case MAPPER_CODEMASTERS:
        switch (addr & 0xE000) {
        case 0x0000:
            sms.page[1] = value;
            break;
        case 0x2000:
            sms.page[2] = value;
            break;
        case 0x6000:
            sms.sram_bank = value & 0x40;
            sms.sram_enabled = true;
            break;
        case 0x8000:
            sms.page[0] = value;
            break;
        default:
            break;
        }
        update_map();
        break;

    case MAPPER_NONE:
    default:
        break;
    }
}

/* ---- I/O ports ---- */

uint8_t sms_in(uint16_t port)
{
    port &= 0xFF;

    if ((port & 0xC1) == 0x40) { /* V counter: F2/F0/BE/FC etc */
        return vdp_vcounter();
    }
    if ((port & 0xC1) == 0x41) { /* H counter */
        return 0x00;
    }

    if ((port & 0xC0) == 0x80) { /* VDP data / control */
        return vdp_read(port);
    }

    if ((port & 0xC0) == 0xC0) {
        switch (port & 0xFE) {
        case 0xC0: /* I/O port A/B */
        case 0xC1: {
            uint8_t v = (uint8_t)(sms.pad & 0x3F);
            if (sms.hw == SMS_HW_GG) {
                /* GG start button shares bit 5 of port DC */
                v |= 0x80; /* N/A high */
            }
            return v | 0x40; /* bits 6-7 read high */
        }
        case 0xC2: /* I/O port B / misc */
        case 0xC3:
            return 0x00;
        case 0xC4: /* GG: parallel port low */
        case 0xC5:
        case 0xC6:
        case 0xC7:
            return 0x00;
        default:
            return 0xFF;
        }
    }

    return 0xFF;
}

void sms_out(uint16_t port, uint8_t value)
{
    port &= 0xFF;

    if ((port & 0xC0) == 0x80) {
        vdp_write(port, value);
        return;
    }

    if ((port & 0xC1) == 0x01) { /* PSG write: 0x7E/0x7F/0xBE/0xBF */
        psg_write(value);
        return;
    }

    if (sms.hw == SMS_HW_GG && port == 0x06) {
        /* GG stereo select — ignored (mono output mixed by the amp) */
        return;
    }
}
