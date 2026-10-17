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
** map5.c
**
** mapper 5 interface
** $Id: map005.c,v 1.2 2001/04/27 14:37:11 neil Exp $
*/
/*
** Modifie depuis la version d'origine de nofrendo : portages ESP32
** (esp32-nesemu d'Espressif, puis Esp32-S3-nes-emulator-by-DSN) et projet
** Console NES ESP32 (2026). Origines et licences : fichier NOTICE.
*/

/* map005.c - MMC5 (Nintendo ExROM)
 *
 * Refait. Le nofrendo d'origine n'en avait qu'une ebauche (pas de modes de
 * programme, pas de RAM de programme en banques, pas de multiplication,
 * compteur de lignes faux) : tous les jeux MMC5 essayes restaient sur un ecran
 * fige (jeux Koei - Uncharted Waters, Nobunaga's Ambition 2, Romance of the
 * Three Kingdoms 2, Bandit Kings, Gemfire, L'Empereur -, Laser Invasion,
 * Castlevania 3 americain...).
 *
 * Ce qui est fait (d'apres la description du circuit sur nesdev) :
 *   - 4 modes de programme ($5100), banques ROM ou RAM ($5113-$5117),
 *     64 Ko de RAM de programme (en PSRAM sur la console) ;
 *   - 4 modes graphiques ($5101), deux jeux de banques ($5120-$5127 et
 *     $5128-$512B) : le dernier jeu ecrit est utilise pour tout l'ecran
 *     (simplification courante : les sprites 8x16 avec des banques
 *     differentes du decor peuvent etre faux) ;
 *   - ExRAM ($5C00-$5FFF, 1 Ko) : memoire du programme, ou ecran de decor
 *     choisi par $5105, ou "attributs etendus" (mode 1 : banque graphique et
 *     palette par tuile, jeux Koei - dessine par nes_ppu.c) ; mode
 *     "remplissage" non fait ;
 *   - compteur de lignes ($5203/$5204, drapeau "dans l'image") ;
 *   - multiplication 8 x 8 bits ($5205/$5206) ;
 *   - son : mmc5_snd.c.
 * Pas fait : ecran partage vertical ($5200-$5202), rarissime.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "noftypes.h"
#include "nes_mmc.h"
#include "nes.h"
#include "nes6502.h"
#include "nes_ppu.h"
#include "log.h"
#include "mmc5_snd.h"
#include "savestate.h"
#include "nes_rom.h"

#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#endif

#define MMC5_PRGRAM_SIZE 0x10000   /* 64 Ko (8 banques de 8 Ko) */

static uint8 *prgram = NULL;       /* alloue une fois, garde d'un jeu a l'autre */
static uint8 *exram = NULL;        /* 1 Ko, juste apres la RAM de programme (PSRAM) */

static struct
{
   uint8 prg_mode, chr_mode, exram_mode, nt_map;
   uint8 prg[5];        /* $5113 (RAM $6000), $5114-$5117 */
   uint8 chr_a[8], chr_b[4];
   uint8 chr_last_b;    /* dernier jeu de banques ecrit : B ($5128-$512B) */
   uint8 chr_upper;     /* $5130 : bits hauts des banques graphiques */
   uint8 mul_a, mul_b;
   uint8 irq_compare, irq_enabled, irq_pending, in_frame;
   int irq_line;
   uint8 ram_slot[4];   /* 1 : banque de RAM en $8000/$A000/$C000/$E000 */
   uint8 *slot_ptr[4];
} m5;

static void set_cpu_pages(uint32 address, uint8 *p, int pages)
{
   nes6502_context ctx;
   int i, page = address >> 12;
   nes6502_getcontext(&ctx);
   for (i = 0; i < pages; i++)
      ctx.mem_page[page + i] = p + (i << 12);
   nes6502_setcontext(&ctx);
}

/* 8 Ko en address : ROM (bit 7 = 1) ou RAM (bit 7 = 0) */
static void map_8k(int slot, uint8 value, bool rom_only)
{
   const uint32 address = 0x8000 + (slot << 13);
   if (rom_only || (value & 0x80))
   {
      mmc_bankrom(8, address, value & 0x7F);
      m5.ram_slot[slot] = 0;
   }
   else
   {
      uint8 *p = prgram + ((value & 0x07) << 13);
      set_cpu_pages(address, p, 2);
      m5.ram_slot[slot] = 1;
      m5.slot_ptr[slot] = p;
   }
}

static void map5_sync_prg(void)
{
   const uint8 *r = m5.prg;   /* r[1..4] = $5114..$5117 */
   if (prgram)
      set_cpu_pages(0x6000, prgram + ((r[0] & 0x07) << 13), 2);
   switch (m5.prg_mode & 3)
   {
   case 0:   /* 32 Ko : $5117 */
      mmc_bankrom(32, 0x8000, (r[4] & 0x7F) >> 2);
      m5.ram_slot[0] = m5.ram_slot[1] = m5.ram_slot[2] = m5.ram_slot[3] = 0;
      break;
   case 1:   /* 16 Ko : $5115 ($8000), $5117 ($C000) */
      if (r[2] & 0x80 || !prgram)
      {
         mmc_bankrom(16, 0x8000, (r[2] & 0x7F) >> 1);
         m5.ram_slot[0] = m5.ram_slot[1] = 0;
      }
      else
      {
         map_8k(0, r[2] & 0xFE, false);
         map_8k(1, r[2] | 0x01, false);
      }
      mmc_bankrom(16, 0xC000, (r[4] & 0x7F) >> 1);
      m5.ram_slot[2] = m5.ram_slot[3] = 0;
      break;
   case 2:   /* 16 Ko ($5115) + 8 Ko ($5116) + 8 Ko ($5117) */
      if (r[2] & 0x80 || !prgram)
      {
         mmc_bankrom(16, 0x8000, (r[2] & 0x7F) >> 1);
         m5.ram_slot[0] = m5.ram_slot[1] = 0;
      }
      else
      {
         map_8k(0, r[2] & 0xFE, false);
         map_8k(1, r[2] | 0x01, false);
      }
      map_8k(2, r[3], !prgram);
      map_8k(3, r[4], true);
      break;
   default:  /* 4 x 8 Ko */
      map_8k(0, r[1], !prgram);
      map_8k(1, r[2], !prgram);
      map_8k(2, r[3], !prgram);
      map_8k(3, r[4], true);
      break;
   }
}

static void map5_sync_chr(void)
{
   const uint8 *a = m5.chr_a, *b = m5.chr_b;
   if (!m5.chr_last_b)
   {
      switch (m5.chr_mode & 3)
      {
      case 0: mmc_bankvrom(8, 0x0000, a[7]); break;
      case 1: mmc_bankvrom(4, 0x0000, a[3]); mmc_bankvrom(4, 0x1000, a[7]); break;
      case 2:
         mmc_bankvrom(2, 0x0000, a[1]); mmc_bankvrom(2, 0x0800, a[3]);
         mmc_bankvrom(2, 0x1000, a[5]); mmc_bankvrom(2, 0x1800, a[7]);
         break;
      default:
      {
         int i;
         for (i = 0; i < 8; i++)
            mmc_bankvrom(1, i << 10, a[i]);
         break;
      }
      }
   }
   else
   {
      switch (m5.chr_mode & 3)
      {
      case 0: mmc_bankvrom(8, 0x0000, b[3]); break;
      case 1: mmc_bankvrom(4, 0x0000, b[3]); mmc_bankvrom(4, 0x1000, b[3]); break;
      case 2:
         mmc_bankvrom(2, 0x0000, b[1]); mmc_bankvrom(2, 0x0800, b[3]);
         mmc_bankvrom(2, 0x1000, b[1]); mmc_bankvrom(2, 0x1800, b[3]);
         break;
      default:
      {
         int i;
         for (i = 0; i < 4; i++)
         {
            mmc_bankvrom(1, i << 10, b[i]);
            mmc_bankvrom(1, 0x1000 + (i << 10), b[i]);
         }
         break;
      }
      }
   }
}

/* Ecrans de decor ($5105) : 0/1 = memoire de la console, 2 = ExRAM,
   3 = remplissage (non fait : ecran 0). */
static void map5_sync_nt(void)
{
   const uint8 v = m5.nt_map;
   int i;
   ppu_mirror(v & 1, (v >> 2) & 1, (v >> 4) & 1, (v >> 6) & 1);
   for (i = 0; i < 4; i++)
   {
      if (exram && ((v >> (i * 2)) & 3) == 2)
      {
         ppu_setpage(1, 8 + i, exram - (0x2000 + (i << 10)));
         ppu_setpage(1, 12 + i, exram - (0x3000 + (i << 10)));   /* miroir $3000 */
      }
   }
}

/* mode 1 de l'ExRAM : attributs etendus, faits par le PPU (nes_ppu.c) */
static void map5_sync_exattr(void)
{
   if (exram && 1 == (m5.exram_mode & 3) && mmc_getinfo()->vrom_banks)
      ppu_set_exattr(exram, mmc_getinfo()->vrom, mmc_getinfo()->vrom_banks * 2, m5.chr_upper);
   else
      ppu_set_exattr(NULL, NULL, 0, 0);
}

static void map5_hblank(int vblank)
{
   const int line = nes_getcontextptr()->scanline;
   UNUSED(vblank);
   if (line >= 240 || !ppu_enabled())
   {
      m5.in_frame = 0;
      return;
   }
   if (!m5.in_frame)
   {
      m5.in_frame = 1;
      m5.irq_line = 0;
   }
   else
   {
      m5.irq_line++;
   }
   if (m5.irq_compare && m5.irq_line == m5.irq_compare)
   {
      m5.irq_pending = 1;
      if (m5.irq_enabled)
         nes_irq();
   }
}

static void map5_write(uint32 address, uint8 value)
{
   if (address >= 0x8000)
   {
      const int slot = (address - 0x8000) >> 13;
      if (m5.ram_slot[slot])
         m5.slot_ptr[slot][address & 0x1FFF] = value;
      return;
   }
   if (address >= 0x5C00 && address <= 0x5FFF)
   {
      if (exram && (m5.exram_mode & 3) != 3)
         exram[address & 0x3FF] = value;
      return;
   }

#ifdef NOFRENDO_PC_TEST
   if (getenv("M5LOG")) printf("MMC5 $%04X <- %02X\n", (unsigned)address, value);
#endif
   switch (address)
   {
   case 0x5100: m5.prg_mode = value & 3; map5_sync_prg(); break;
   case 0x5101: m5.chr_mode = value & 3; map5_sync_chr(); break;
   case 0x5102:
   case 0x5103: break;   /* protection de la RAM : ignoree */
   case 0x5104:
      m5.exram_mode = value & 3;
      map5_sync_exattr();
      break;
   case 0x5130:
      m5.chr_upper = value & 3;
      map5_sync_exattr();
      break;
   case 0x5105: m5.nt_map = value; map5_sync_nt(); break;
   case 0x5106:
   case 0x5107: break;   /* remplissage : non fait */
   case 0x5113: case 0x5114: case 0x5115: case 0x5116: case 0x5117:
      m5.prg[address - 0x5113] = value;
      map5_sync_prg();
      break;
   case 0x5120: case 0x5121: case 0x5122: case 0x5123:
   case 0x5124: case 0x5125: case 0x5126: case 0x5127:
      m5.chr_a[address - 0x5120] = value;
      m5.chr_last_b = 0;
      map5_sync_chr();
      break;
   case 0x5128: case 0x5129: case 0x512A: case 0x512B:
      m5.chr_b[address - 0x5128] = value;
      m5.chr_last_b = 1;
      map5_sync_chr();
      break;
   case 0x5203: m5.irq_compare = value; break;
   case 0x5204:
      m5.irq_enabled = (value & 0x80) ? 1 : 0;
      if (!m5.irq_enabled)
         nes6502_irq_clear();
      else if (m5.irq_pending)
         nes_irq();
      break;
   case 0x5205: m5.mul_a = value; break;
   case 0x5206: m5.mul_b = value; break;
   default: break;
   }
}

static uint8 map5_read(uint32 address)
{
   if (address >= 0x5C00 && address <= 0x5FFF)
      return (exram && (m5.exram_mode & 3) >= 2) ? exram[address & 0x3FF] : 0xFF;
   switch (address)
   {
   case 0x5204:
   {
      const uint8 v = (uint8)((m5.irq_pending << 7) | (m5.in_frame << 6));
      m5.irq_pending = 0;
      nes6502_irq_clear();
      return v;
   }
   case 0x5205: return (uint8)((m5.mul_a * m5.mul_b) & 0xFF);
   case 0x5206: return (uint8)((m5.mul_a * m5.mul_b) >> 8);
   default: return 0xFF;
   }
}

/* Apres le chargement d'un etat : pointeurs internes recalcules
      (la RAM de programme n'est pas a la meme adresse apres un redemarrage) */
static void map5_post_load(void)
{
   map5_sync_prg();
   map5_sync_chr();
   map5_sync_nt();
   map5_sync_exattr();
}

static void map5_init(void)
{
   if (!prgram)
   {
#if defined(ESP_PLATFORM)
      prgram = (uint8 *)heap_caps_malloc(MMC5_PRGRAM_SIZE + 0x400, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
      if (!prgram)
         prgram = (uint8 *)malloc(MMC5_PRGRAM_SIZE + 0x400);
      if (!prgram)
         log_printf("MMC5 : pas de memoire pour la RAM de programme\n");
   }
   if (prgram)
   {
      memset(prgram, 0, MMC5_PRGRAM_SIZE + 0x400);
      exram = prgram + MMC5_PRGRAM_SIZE;
   }
   else
   {
      exram = NULL;   /* pas de memoire : ExRAM absente (lue $FF) */
   }
   memset(&m5, 0, sizeof(m5));
   /* Etat pour la sauvegarde instantanee (savestate.h), RAM de
         programme sauvegardee par la pile (jeux Koei) */
   if (prgram)
   {
      ss_region(prgram, MMC5_PRGRAM_SIZE + 0x400, true);
      if (mmc_getinfo()->flags & ROM_FLAG_BATTERY)
         ss_battery_region(prgram, MMC5_PRGRAM_SIZE);
   }
   ss_var(&m5, sizeof(m5));
   ss_post_load(map5_post_load);
   m5.prg_mode = 3;
   m5.chr_mode = 3;
   m5.prg[4] = 0xFF;   /* $5117 : derniere banque a la mise sous tension */
   m5.prg[1] = m5.prg[2] = m5.prg[3] = 0xFF;
   map5_sync_prg();
   map5_sync_exattr();
}

static void map5_getstate(SnssMapperBlock *state)
{
   state->extraData.mapper5.dummy = 0;
}

static void map5_setstate(SnssMapperBlock *state)
{
   UNUSED(state);
}

static map_memwrite map5_memwrite[] =
    {
        /* $5000 - $5015 handled by sound */
        {0x5016, 0x5FFF, map5_write},
        {0x8000, 0xFFFF, map5_write},
        {-1, -1, NULL}};

static map_memread map5_memread[] =
    {
        {0x5204, 0x5206, map5_read},
        {0x5C00, 0x5FFF, map5_read},
        {-1, -1, NULL}};

mapintf_t map5_intf =
    {
        5,             /* mapper number */
        "MMC5",        /* mapper name */
        map5_init,     /* init routine */
        NULL,          /* vblank callback */
        map5_hblank,   /* hblank callback */
        map5_getstate, /* get state (snss) */
        map5_setstate, /* set state (snss) */
        map5_memread,  /* memory read structure */
        map5_memwrite, /* memory write structure */
        &mmc5_ext      /* external sound device */
};
