/*
** Nofrendo (c) 1998-2000 Matthew Conte (matt@conte.com)
**
**
** This program is free software; you can redistribute it and/or
** modify it under the terms of version 2 of the GNU Library General
** Public License as published by the Free Software Foundation.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
** Library General Public License for more details.  To obtain a
** copy of the GNU Library General Public License, write to the Free
** Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
**
** Any permitted reproduction of these routines, in whole or in part,
** must bear this legend.
**
**
** nes/state.c: Save state support
**
*/

#include "nes.h"

/**
 * Save file format:
 *
 * The file has an 8 byte header followed by a series of blocks.
 * Integer values are big endian.
 *
 *    Header format:
 *       magic (4 bytes), always "SNSS"
 *       block count (4 bytes), int32be
 *
 *    Block format:
 *       name (4 bytes)
 *       block version (4 bytes), int32be
 *       block length (4 bytes), int32be
 *       data ("block length" bytes)
 *
 * In actuality, most blocks have a fixed size and all are at version 1.
 * - BASR: 6449 bytes
 * - INFO: 256 bytes
 * - SOUN: 22 bytes
 * - SRAM: prg-ram + 1 bytes
 * - VRAM: chr-ram bytes
 * - MPRD: 152 bytes
 */

typedef struct {
    uint8 magic[4];
    uint32 blocks;
} header_t;

typedef struct {
    uint8 name[4];
    uint32 version;
    uint32 length;
    uint8 data[];
} block_t;

#define _fread(buffer, size)                                                                                           \
    {                                                                                                                  \
        if (fread(buffer, size, 1, file) != 1) {                                                                       \
            MESSAGE_ERROR("state_load: fread failed.\n");                                                              \
            goto _error;                                                                                               \
        }                                                                                                              \
    }

#define _fwrite(buffer, size)                                                                                          \
    {                                                                                                                  \
        if (fwrite(buffer, size, 1, file) != 1) {                                                                      \
            MESSAGE_ERROR("state_save: fwrite failed.\n");                                                             \
            goto _error;                                                                                               \
        }                                                                                                              \
    }

#ifndef IS_BIG_ENDIAN
static inline uint16 swap16(uint16 x)
{
    return (x << 8) | (x >> 8);
}

static inline uint32 swap32(uint32 x)
{
    return ((x >> 24) & 0xff) | ((x << 8) & 0xff0000) | ((x >> 8) & 0xff00) | ((x << 24) & 0xff000000);
}
#else
#define swap16(x) (x)
#define swap32(x) (x)
#endif

static bool write_block_header(FILE *file, const char name[4], uint32 length)
{
    uint8 header[12];
    uint32 version = swap32(1);
    length = swap32(length);
    memcpy(header, name, 4);
    memcpy(header + 4, &version, sizeof(version));
    memcpy(header + 8, &length, sizeof(length));
    return fwrite(header, sizeof(header), 1, file) == 1;
}

static bool memory_zone_dirty(const void *ptr, size_t size)
{
    size_t pos = 0;

    while (size - pos >= 4) {
        if (*((uint32 *)(ptr + pos)) != 0) {
            return true;
        }
        pos += 4;
    }

    while (pos < size) {
        if (*((uint8 *)(ptr + pos)) != 0) {
            return true;
        }
        pos += 1;
    }

    return false;
}

int state_save(const char *fn)
{
    uint32 numberOfBlocks = 0;
    uint8 buffer[600];
    nes_t *machine = nes_getptr();
    FILE *file;

    if (!(file = fopen(fn, "wb"))) {
        MESSAGE_ERROR("state_save: file '%s' could not be opened.\n", fn);
        return -1; // goto _error;
    }

    MESSAGE_INFO("state_save: file '%s' opened.\n", fn);

    _fwrite("SNSS\x00\x00\x00\x05", 8);

    /****************************************************/

    MESSAGE_INFO("  - Saving base block\n");

    buffer[0] = machine->cpu->a_reg;
    buffer[1] = machine->cpu->x_reg;
    buffer[2] = machine->cpu->y_reg;
    buffer[3] = machine->cpu->p_reg;
    buffer[4] = machine->cpu->s_reg;
    buffer[5] = machine->cpu->pc_reg / 256;
    buffer[6] = machine->cpu->pc_reg % 256;
    buffer[7] = machine->ppu->ctrl0;
    buffer[8] = machine->ppu->ctrl1;

    if (!write_block_header(file, "BASR", 0x1931)) {
        goto _error;
    }
    _fwrite(&buffer, 9);
    _fwrite(machine->mem->ram, 0x800);
    _fwrite(machine->ppu->oam, 0x100);
    _fwrite(machine->ppu->nametab, 0x1000);

    /* Mask off priority color bits */
    for (int i = 0; i < 32; i++) {
        buffer[i] = machine->ppu->palette[i] & 0x3F;
    }

    buffer[32] = machine->ppu->nt_map[0];
    buffer[33] = machine->ppu->nt_map[1];
    buffer[34] = machine->ppu->nt_map[2];
    buffer[35] = machine->ppu->nt_map[3];
    buffer[36] = machine->ppu->vaddr / 256;
    buffer[37] = machine->ppu->vaddr % 256;
    buffer[38] = machine->ppu->oam_addr;
    buffer[39] = machine->ppu->tile_xofs;

    _fwrite(&buffer, 40);
    numberOfBlocks++;

    /****************************************************/

    MESSAGE_INFO("  - Saving info block\n");

    if (!write_block_header(file, "INFO", 0x100)) {
        goto _error;
    }
    _fwrite(&buffer, 0x100);
    numberOfBlocks++;

    /****************************************************/

    MESSAGE_INFO("  - Saving sound block\n");

    buffer[0x00] = machine->apu->rectangle[0].regs[0];
    buffer[0x01] = machine->apu->rectangle[0].regs[1];
    buffer[0x02] = machine->apu->rectangle[0].regs[2];
    buffer[0x03] = machine->apu->rectangle[0].regs[3];
    buffer[0x04] = machine->apu->rectangle[1].regs[0];
    buffer[0x05] = machine->apu->rectangle[1].regs[1];
    buffer[0x06] = machine->apu->rectangle[1].regs[2];
    buffer[0x07] = machine->apu->rectangle[1].regs[3];
    buffer[0x08] = machine->apu->triangle.regs[0];
    buffer[0x0A] = machine->apu->triangle.regs[1];
    buffer[0x0B] = machine->apu->triangle.regs[2];
    buffer[0X0C] = machine->apu->noise.regs[0];
    buffer[0X0E] = machine->apu->noise.regs[1];
    buffer[0x0F] = machine->apu->noise.regs[2];
    buffer[0x10] = machine->apu->dmc.regs[0];
    buffer[0x11] = machine->apu->dmc.regs[1];
    buffer[0x12] = machine->apu->dmc.regs[2];
    buffer[0x13] = machine->apu->dmc.regs[3];
    buffer[0x15] = machine->apu->control_reg;

    if (!write_block_header(file, "SOUN", 0x16)) {
        goto _error;
    }
    _fwrite(&buffer, 0x16);
    numberOfBlocks++;

    /****************************************************/

    uint32 vram_size = ROM_CHR_BANK_SIZE * machine->cart->chr_ram_banks;
    if (machine->cart->chr_rom_banks == 0 && memory_zone_dirty(machine->cart->chr_ram, vram_size)) {
        MESSAGE_INFO("  - Saving VRAM block\n");

        if (!write_block_header(file, "VRAM", vram_size)) {
            goto _error;
        }
        _fwrite(machine->cart->chr_ram, vram_size);
        numberOfBlocks++;
    }

    /****************************************************/

    uint32 sram_size = ROM_PRG_BANK_SIZE * machine->cart->prg_ram_banks;
    if (memory_zone_dirty(machine->cart->prg_ram, sram_size)) {
        MESSAGE_INFO("  - Saving SRAM block\n");

        if (!write_block_header(file, "SRAM", sram_size + 1)) {
            goto _error;
        }
        _fwrite("\x01", 1); // SRAM enabled (unused)
        _fwrite(machine->cart->prg_ram, sram_size);
        numberOfBlocks++;
    }

    /****************************************************/

    if (machine->mapper->number > 0) {
        MESSAGE_INFO("  - Saving mapper block\n");

        memset(buffer, 0, sizeof(buffer));

        for (int i = 0; i < 4; i++) {
            uint16 temp = (mem_getpage((i + 4) * 4) - machine->cart->prg_rom) >> 13;
            temp = swap16(temp);
            buffer[(i * 2) + 0] = ((uint8 *)&temp)[0];
            buffer[(i * 2) + 1] = ((uint8 *)&temp)[1];
        }

        for (int i = 0; i < 8; i++) {
            uint16 temp = (machine->cart->chr_rom_banks) ? ((ppu_getpage(i) - machine->cart->chr_rom) >> 10) : (i);
            temp = swap16(temp);
            buffer[8 + (i * 2) + 0] = ((uint8 *)&temp)[0];
            buffer[8 + (i * 2) + 1] = ((uint8 *)&temp)[1];
        }

        if (machine->mapper->get_state) {
            machine->mapper->get_state(buffer + 0x18);
        }

        if (!write_block_header(file, "MPRD", 0x218)) {
            goto _error;
        }
        _fwrite(&buffer, 0x218);
        numberOfBlocks++;
    }

    /****************************************************/

    // Update number of blocks
    fseek(file, 4, SEEK_SET);
    numberOfBlocks = swap32(numberOfBlocks);
    _fwrite(&numberOfBlocks, 4);

    fclose(file);

    MESSAGE_INFO("state_save: Game saved!\n");

    return 0;

_error:
    MESSAGE_ERROR("state_save: Save failed!\n");
    fclose(file);
    return -1;
}

int state_load(const char *fn)
{
    uint8 buffer[600];

    nes_t *machine = nes_getptr();
    FILE *file;

    if (!(file = fopen(fn, "rb"))) {
        MESSAGE_ERROR("state_load: file '%s' could not be opened.\n", fn);
        return -1; // goto _error;
    }

    _fread(buffer, 8);

    if (memcmp(buffer, "SNSS", 4) != 0) {
        MESSAGE_ERROR("state_load: file '%s' is not a save file.\n", fn);
        goto _error;
    }

    uint32 numberOfBlocks = swap32(*((uint32 *)&buffer[4]));
    uint32 nextBlock = 8;
    bool base_loaded = false;
    bool vram_loaded = false;
    bool sram_loaded = false;

    MESSAGE_INFO("state_load: file '%s' opened, blocks=%u.\n", fn, numberOfBlocks);

    for (uint32 blk = 0; blk < numberOfBlocks; blk++) {
        fseek(file, nextBlock, SEEK_SET);
        _fread(buffer, 12);

        uint32 blockVersion = swap32(*((uint32 *)&buffer[4]));
        uint32 blockLength = swap32(*((uint32 *)&buffer[8]));
        if (blockVersion != 1) {
            goto _error;
        }

        nextBlock += 12 + blockLength;

        /****************************************************/

        if (memcmp(buffer, "BASR", 4) == 0) {
            MESSAGE_INFO("  - Found base block (%u bytes)\n", blockLength);
            if (blockLength != 0x1931) {
                goto _error;
            }

            _fread(buffer, 9);

            machine->cpu->a_reg = buffer[0x0];
            machine->cpu->x_reg = buffer[0x1];
            machine->cpu->y_reg = buffer[0x2];
            machine->cpu->p_reg = buffer[0x3];
            machine->cpu->s_reg = buffer[0x4];
            machine->cpu->pc_reg = swap16(*((uint16 *)&buffer[0x5]));
            machine->ppu->ctrl0 = buffer[0x7];
            machine->ppu->ctrl1 = buffer[0x8];

            _fread(machine->mem->ram, 0x800);
            _fread(machine->ppu->oam, 0x100);
            _fread(machine->ppu->nametab, 0x1000);
            _fread(machine->ppu->palette, 0x20);

            /* TODO: argh, this is to handle nofrendo's filthy sprite priority method */
            for (int i = 0; i < 8; i++) {
                machine->ppu->palette[i << 2] = machine->ppu->palette[0] | 0x80; // BG_TRANS;
            }

            _fread(buffer, 8);

            machine->ppu->vaddr = swap16(*((uint16 *)&buffer[0x4]));
            machine->ppu->oam_addr = buffer[0x6];
            machine->ppu->tile_xofs = buffer[0x7];

            /* do some extra handling */
            machine->ppu->flipflop = 0;
            machine->ppu->strikeflag = false;

            ppu_setnametable(0, buffer[0]);
            ppu_setnametable(1, buffer[1]);
            ppu_setnametable(2, buffer[2]);
            ppu_setnametable(3, buffer[3]);

            ppu_write(PPU_CTRL0, machine->ppu->ctrl0);
            ppu_write(PPU_CTRL1, machine->ppu->ctrl1);
            ppu_write(PPU_VADDR, machine->ppu->vaddr >> 8);
            ppu_write(PPU_VADDR, machine->ppu->vaddr & 0xFF);
            base_loaded = true;
        }

        /****************************************************/

        else if (memcmp(buffer, "VRAM", 4) == 0) {
            MESSAGE_INFO("  - Found VRAM block (%u bytes)\n", blockLength);

            uint32 capacity = ROM_CHR_BANK_SIZE * machine->cart->chr_ram_banks;
            if (machine->cart->chr_rom_banks != 0 || blockLength != capacity) {
                goto _error;
            }

            _fread(machine->cart->chr_ram, blockLength);
            vram_loaded = true;
        }

        /****************************************************/

        else if (memcmp(buffer, "SRAM", 4) == 0) {
            MESSAGE_INFO("  - Found SRAM block (%u bytes)\n", blockLength);

            uint32 capacity = ROM_PRG_BANK_SIZE * machine->cart->prg_ram_banks;
            if (blockLength != capacity + 1) {
                goto _error;
            }

            _fread(buffer, 1); // SRAM enabled (always true)
            _fread(machine->cart->prg_ram, blockLength - 1);
            sram_loaded = true;
        }

        /****************************************************/

        else if (memcmp(buffer, "MPRD", 4) == 0) {
            MESSAGE_INFO("  - Found mapper block (%u bytes)\n", blockLength);
            if (blockLength != 0x218) {
                goto _error;
            }

            _fread(buffer, blockLength);

            for (int i = 0; i < 4; i++) {
                mmc_bankprg(8, 0x8000 + (i * 0x2000), swap16(((uint16 *)buffer)[i]), PRG_ROM);
            }

            if (machine->cart->chr_rom_banks) {
                for (int i = 0; i < 8; i++) {
                    mmc_bankchr(1, i * 0x400, swap16(((uint16 *)buffer)[4 + i]), CHR_ROM);
                }
            } else if (machine->cart->chr_ram) {
                for (int i = 0; i < 8; i++) {
                    mmc_bankchr(1, i * 0x400, i, CHR_RAM);
                }
            }

            if (machine->mapper->set_state) {
                machine->mapper->set_state(buffer + 0x18);
            }
        }

        /****************************************************/

        else if (memcmp(buffer, "SOUN", 4) == 0) {
            MESSAGE_INFO("  - Found sound block (%u bytes)\n", blockLength);
            if (blockLength != 0x16) {
                goto _error;
            }

            _fread(buffer, blockLength);

            apu_reset();

            for (int i = 0; i < 0x16; i++) {
                apu_write(0x4000 + i, buffer[i]);
            }
        }

        /****************************************************/

        else if (memcmp(buffer, "INFO", 4) == 0) {
            MESSAGE_INFO("  - Found info block (%u bytes)\n", blockLength);
            if (blockLength != 0x100) {
                goto _error;
            }

            _fread(buffer, blockLength);

            // We don't currently do anything with it, it's just to help report bugs to me :)
        }

        /****************************************************/

        else {
            MESSAGE_ERROR("Found unknown block type!\n");
        }
    }

    if (!base_loaded) {
        goto _error;
    }
    if (!vram_loaded && machine->cart->chr_rom_banks == 0 && machine->cart->chr_ram_banks > 0) {
        memset(machine->cart->chr_ram, 0, ROM_CHR_BANK_SIZE * machine->cart->chr_ram_banks);
    }
    if (!sram_loaded && machine->cart->prg_ram_banks > 0) {
        memset(machine->cart->prg_ram, 0, ROM_PRG_BANK_SIZE * machine->cart->prg_ram_banks);
    }

    /* close file, we're done */
    fclose(file);

    MESSAGE_INFO("state_load: Game restored\n");

    return 0;

_error:
    MESSAGE_ERROR("state_load: Load failed!\n");
    fclose(file);
    return -1;
}

/* ==================================================================
 *   In-memory snapshots (rewind)
 *
 *   These snapshots only live within the current process, so copying the
 *   complete runtime structs is both smaller and safer than reconstructing
 *   timing-sensitive CPU/PPU/APU state from register values.
 * ================================================================== */

#define MEM_STATE_FIXED_SIZE                                                                                           \
    (1 + (2 * sizeof(int)) + sizeof(nes6502_t) + sizeof(ppu_t) + sizeof(apu_t) + MEM_RAMSIZE + (PPU_PAGESIZE * 4))

size_t state_mem_size(void)
{
    nes_t *machine = nes_getptr();
    size_t size = MEM_STATE_FIXED_SIZE;

    if (machine->cart->chr_rom_banks == 0 && machine->cart->chr_ram_banks > 0) {
        size += 0x2000 * machine->cart->chr_ram_banks;
    }
    if (machine->cart->prg_ram_banks > 0) {
        size += 0x2000 * machine->cart->prg_ram_banks;
    }
    if (machine->mapper->number > 0) {
        size += 0x218;
    }

    return size;
}

int state_save_mem(uint8 *buf)
{
    nes_t *machine = nes_getptr();
    uint8 *p = buf;
    uint8 flags = 0;

    if (machine->cart->chr_rom_banks == 0 && machine->cart->chr_ram_banks > 0) {
        flags |= 0x01;
    }
    if (machine->cart->prg_ram_banks > 0) {
        flags |= 0x02;
    }
    if (machine->mapper->number > 0) {
        flags |= 0x04;
    }

    *p++ = flags;

    memcpy(p, &machine->scanline, sizeof(machine->scanline));
    p += sizeof(machine->scanline);
    memcpy(p, &machine->cycles, sizeof(machine->cycles));
    p += sizeof(machine->cycles);
    memcpy(p, machine->cpu, sizeof(*machine->cpu));
    p += sizeof(*machine->cpu);
    memcpy(p, machine->ppu, sizeof(*machine->ppu));
    p += sizeof(*machine->ppu);
    memcpy(p, machine->apu, sizeof(*machine->apu));
    p += sizeof(*machine->apu);
    memcpy(p, machine->mem->ram, MEM_RAMSIZE);
    p += MEM_RAMSIZE;
    memcpy(p, machine->ppu->nametab, PPU_PAGESIZE * 4);
    p += PPU_PAGESIZE * 4;

    /* CHR RAM */
    if (flags & 0x01) {
        memcpy(p, machine->cart->chr_ram, 0x2000 * machine->cart->chr_ram_banks);
        p += 0x2000 * machine->cart->chr_ram_banks;
    }

    /* PRG RAM */
    if (flags & 0x02) {
        memcpy(p, machine->cart->prg_ram, 0x2000 * machine->cart->prg_ram_banks);
        p += 0x2000 * machine->cart->prg_ram_banks;
    }

    /* Mapper state */
    if (flags & 0x04) {
        memset(p, 0, 0x218);

        for (int i = 0; i < 4; i++) {
            uint16 temp = (uint16)((mem_getpage((i + 4) * 4) - machine->cart->prg_rom) >> 13);
            p[(i * 2) + 0] = temp & 0xFF;
            p[(i * 2) + 1] = (temp >> 8) & 0xFF;
        }

        for (int i = 0; i < 8; i++) {
            uint16 temp = (machine->cart->chr_rom_banks) ? (uint16)((ppu_getpage(i) - machine->cart->chr_rom) >> 10)
                                                         : (uint16)(i);
            p[8 + (i * 2) + 0] = temp & 0xFF;
            p[8 + (i * 2) + 1] = (temp >> 8) & 0xFF;
        }

        if (machine->mapper->get_state) {
            machine->mapper->get_state(p + 0x18);
        }

        p += 0x218;
    }

    return (int)(p - buf);
}

int state_load_mem(const uint8 *buf)
{
    nes_t *machine = nes_getptr();
    const uint8 *p = buf;
    uint8 flags = *p++;

    memcpy(&machine->scanline, p, sizeof(machine->scanline));
    p += sizeof(machine->scanline);
    memcpy(&machine->cycles, p, sizeof(machine->cycles));
    p += sizeof(machine->cycles);
    memcpy(machine->cpu, p, sizeof(*machine->cpu));
    p += sizeof(*machine->cpu);
    memcpy(machine->ppu, p, sizeof(*machine->ppu));
    p += sizeof(*machine->ppu);
    memcpy(machine->apu, p, sizeof(*machine->apu));
    p += sizeof(*machine->apu);
    memcpy(machine->mem->ram, p, MEM_RAMSIZE);
    p += MEM_RAMSIZE;
    memcpy(machine->ppu->nametab, p, PPU_PAGESIZE * 4);
    p += PPU_PAGESIZE * 4;

    /* CHR RAM */
    if (flags & 0x01) {
        memcpy(machine->cart->chr_ram, p, 0x2000 * machine->cart->chr_ram_banks);
        p += 0x2000 * machine->cart->chr_ram_banks;
    }

    /* PRG RAM */
    if (flags & 0x02) {
        memcpy(machine->cart->prg_ram, p, 0x2000 * machine->cart->prg_ram_banks);
        p += 0x2000 * machine->cart->prg_ram_banks;
    }

    /* Mapper state */
    if (flags & 0x04) {
        for (int i = 0; i < 4; i++) {
            uint16 bank = p[(i * 2)] | (p[(i * 2) + 1] << 8);
            mmc_bankprg(8, 0x8000 + (i * 0x2000), bank, PRG_ROM);
        }

        if (machine->cart->chr_rom_banks) {
            for (int i = 0; i < 8; i++) {
                uint16 bank = p[8 + (i * 2)] | (p[8 + (i * 2) + 1] << 8);
                mmc_bankchr(1, i * 0x400, bank, CHR_ROM);
            }
        } else if (machine->cart->chr_ram) {
            for (int i = 0; i < 8; i++) {
                mmc_bankchr(1, i * 0x400, i, CHR_RAM);
            }
        }

        if (machine->mapper->set_state) {
            machine->mapper->set_state((uint8 *)(p + 0x18));
        }

        p += 0x218;
    }

    return 0;
}
