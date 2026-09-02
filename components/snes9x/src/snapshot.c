/* This file is part of Snes9x. See LICENSE file. */

#include <stdio.h>

#include "snes9x.h"
#include "memmap.h"
#include "ppu.h"
#include "cpuexec.h"
#include "apu.h"
#include "dma.h"
#include "display.h"
#include "srtc.h"
#include "soundux.h"
#include "cpuops.h"

const char snes_state_header[17] = "SNES9X_000000002";


bool S9xSaveState(const char *filename)
{
   int chunks = 0;
   FILE *fp = NULL;

   if (!(fp = fopen(filename, "wb")))
      return false;

   chunks += fwrite(snes_state_header, 16, 1, fp);
   chunks += fwrite(&CPU, sizeof(CPU), 1, fp);
   chunks += fwrite(&ICPU, sizeof(ICPU), 1, fp);
   chunks += fwrite(&PPU, sizeof(PPU), 1, fp);
   chunks += fwrite(&DMA, sizeof(DMA), 1, fp);
   chunks += fwrite(Memory.VRAM, VRAM_SIZE, 1, fp);
   chunks += fwrite(Memory.RAM, RAM_SIZE, 1, fp);
   chunks += fwrite(Memory.SRAM, SRAM_SIZE, 1, fp);
   chunks += fwrite(Memory.FillRAM, FILLRAM_SIZE, 1, fp);
   chunks += fwrite(&APU, sizeof(APU), 1, fp);
   chunks += fwrite(&IAPU, sizeof(IAPU), 1, fp);
   chunks += fwrite(IAPU.RAM, 0x10000, 1, fp);
   chunks += fwrite(&SoundData, sizeof(SoundData), 1, fp);

   printf("Saved chunks = %d\n", chunks);

   fclose(fp);

   return chunks == 13;
}

bool S9xLoadState(const char *filename)
{
   uint8_t buffer[512];
   int chunks = 0;
   FILE *fp = NULL;

   if (!(fp = fopen(filename, "rb")))
      return false;

   if (!fread(buffer, 16, 1, fp) || memcmp(snes_state_header, buffer, 16) != 0)
   {
      printf("Wrong header found\n");
      goto fail;
   }

   // At this point we can't go back and a failure will corrupt the state anyway
   S9xReset();

   uint8_t *IAPU_RAM = IAPU.RAM;

   chunks += fread(&CPU, sizeof(CPU), 1, fp);
   chunks += fread(&ICPU, sizeof(ICPU), 1, fp);
   chunks += fread(&PPU, sizeof(PPU), 1, fp);
   chunks += fread(&DMA, sizeof(DMA), 1, fp);
   chunks += fread(Memory.VRAM, VRAM_SIZE, 1, fp);
   chunks += fread(Memory.RAM, RAM_SIZE, 1, fp);
   chunks += fread(Memory.SRAM, SRAM_SIZE, 1, fp);
   chunks += fread(Memory.FillRAM, FILLRAM_SIZE, 1, fp);
   chunks += fread(&APU, sizeof(APU), 1, fp);
   chunks += fread(&IAPU, sizeof(IAPU), 1, fp);
   chunks += fread(IAPU.RAM, 0x10000, 1, fp);
   chunks += fread(&SoundData, sizeof(SoundData), 1, fp);

   printf("Loaded chunks = %d\n", chunks);

   // Fixing up registers and pointers:

   IAPU.PC = IAPU.PC - IAPU.RAM + IAPU_RAM;
   IAPU.DirectPage = IAPU.DirectPage - IAPU.RAM + IAPU_RAM;
   IAPU.WaitAddress1 = IAPU.WaitAddress1 - IAPU.RAM + IAPU_RAM;
   IAPU.WaitAddress2 = IAPU.WaitAddress2 - IAPU.RAM + IAPU_RAM;
   IAPU.RAM = IAPU_RAM;

   FixROMSpeed();
   IPPU.ColorsChanged = true;
   IPPU.OBJChanged = true;
   CPU.InDMA = false;
   S9xFixColourBrightness();
   S9xAPUUnpackStatus();
   S9xFixSoundAfterSnapshotLoad();
   ICPU.ShiftedPB = ICPU.Registers.PB << 16;
   ICPU.ShiftedDB = ICPU.Registers.DB << 16;
   S9xSetPCBase(ICPU.ShiftedPB + ICPU.Registers.PC);
   S9xUnpackStatus();
   S9xFixCycles();
   S9xReschedule();

   fclose(fp);
   return true;

fail:
   fclose(fp);
   return false;
}

/* --- In-memory snapshot support (nesboy-esp32) --- */

bool S9xSaveStateMem(uint8_t *buffer, size_t buffer_size, size_t *written)
{
   uint8_t *ptr = buffer;
   size_t used = 0;
   bool ok = true;

#define PUT(v, n) do {       if (used + (n) > buffer_size) { ok = false; }       else { memcpy(ptr, &(v), (n)); ptr += (n); used += (n); }    } while (0)

   PUT(snes_state_header, 16);
   PUT(CPU, sizeof(CPU));
   PUT(ICPU, sizeof(ICPU));
   PUT(PPU, sizeof(PPU));
   PUT(DMA, sizeof(DMA));
   if (ok) { if (used + VRAM_SIZE > buffer_size) ok = false; else { memcpy(ptr, Memory.VRAM, VRAM_SIZE); ptr += VRAM_SIZE; used += VRAM_SIZE; } }
   if (ok) { if (used + RAM_SIZE > buffer_size) ok = false; else { memcpy(ptr, Memory.RAM, RAM_SIZE); ptr += RAM_SIZE; used += RAM_SIZE; } }
   if (ok) { if (used + SRAM_SIZE > buffer_size) ok = false; else { memcpy(ptr, Memory.SRAM, SRAM_SIZE); ptr += SRAM_SIZE; used += SRAM_SIZE; } }
   if (ok) { if (used + FILLRAM_SIZE > buffer_size) ok = false; else { memcpy(ptr, Memory.FillRAM, FILLRAM_SIZE); ptr += FILLRAM_SIZE; used += FILLRAM_SIZE; } }
   PUT(APU, sizeof(APU));
   PUT(IAPU, sizeof(IAPU));
   if (ok) { if (used + 0x10000 > buffer_size) ok = false; else { memcpy(ptr, IAPU.RAM, 0x10000); ptr += 0x10000; used += 0x10000; } }
   PUT(SoundData, sizeof(SoundData));
#undef PUT

   if (written)
      *written = used;
   return ok;
}

bool S9xLoadStateMem(const uint8_t *buffer, size_t buffer_size)
{
   const uint8_t *ptr = buffer;
   size_t used = 0;
   bool ok = true;

#define GET(v, n) do {       if (used + (n) > buffer_size) { ok = false; }       else { memcpy(&(v), ptr, (n)); ptr += (n); used += (n); }    } while (0)

   {
      uint8_t hdr[16];
      GET(hdr, sizeof(hdr));
      if (!ok || memcmp(snes_state_header, hdr, 16) != 0)
         return false;
   }

   /* Past this point a failure corrupts the state anyway (same as file load) */
   S9xReset();

   uint8_t *IAPU_RAM = IAPU.RAM;

   GET(CPU, sizeof(CPU));
   GET(ICPU, sizeof(ICPU));
   GET(PPU, sizeof(PPU));
   GET(DMA, sizeof(DMA));
   if (ok) { if (used + VRAM_SIZE > buffer_size) ok = false; else { memcpy(Memory.VRAM, ptr, VRAM_SIZE); ptr += VRAM_SIZE; used += VRAM_SIZE; } }
   if (ok) { if (used + RAM_SIZE > buffer_size) ok = false; else { memcpy(Memory.RAM, ptr, RAM_SIZE); ptr += RAM_SIZE; used += RAM_SIZE; } }
   if (ok) { if (used + SRAM_SIZE > buffer_size) ok = false; else { memcpy(Memory.SRAM, ptr, SRAM_SIZE); ptr += SRAM_SIZE; used += SRAM_SIZE; } }
   if (ok) { if (used + FILLRAM_SIZE > buffer_size) ok = false; else { memcpy(Memory.FillRAM, ptr, FILLRAM_SIZE); ptr += FILLRAM_SIZE; used += FILLRAM_SIZE; } }
   GET(APU, sizeof(APU));
   GET(IAPU, sizeof(IAPU));
   if (ok) { if (used + 0x10000 > buffer_size) ok = false; else { memcpy(IAPU.RAM, ptr, 0x10000); ptr += 0x10000; used += 0x10000; } }
   GET(SoundData, sizeof(SoundData));
#undef GET

   if (!ok)
      return false;

   /* Fixing up registers and pointers (same as the file path) */
   IAPU.PC = IAPU.PC - IAPU.RAM + IAPU_RAM;
   IAPU.DirectPage = IAPU.DirectPage - IAPU.RAM + IAPU_RAM;
   IAPU.WaitAddress1 = IAPU.WaitAddress1 - IAPU.RAM + IAPU_RAM;
   IAPU.WaitAddress2 = IAPU.WaitAddress2 - IAPU.RAM + IAPU_RAM;
   IAPU.RAM = IAPU_RAM;

   FixROMSpeed();
   IPPU.ColorsChanged = true;
   IPPU.OBJChanged = true;
   CPU.InDMA = false;
   S9xFixColourBrightness();
   S9xAPUUnpackStatus();
   S9xFixSoundAfterSnapshotLoad();
   ICPU.ShiftedPB = ICPU.Registers.PB << 16;
   ICPU.ShiftedDB = ICPU.Registers.DB << 16;
   S9xSetPCBase(ICPU.ShiftedPB + ICPU.Registers.PC);
   S9xUnpackStatus();
   S9xFixCycles();
   S9xReschedule();

   return true;
}
