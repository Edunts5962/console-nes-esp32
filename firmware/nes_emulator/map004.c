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
** map4.c
**
** mapper 4 interface
** $Id: map004.c,v 1.2 2001/04/27 14:37:11 neil Exp $
*/
/*
** Modifie depuis la version d'origine de nofrendo : portages ESP32
** (esp32-nesemu d'Espressif, puis Esp32-S3-nes-emulator-by-DSN) et projet
** Console NES ESP32 (2026). Origines et licences : fichier NOTICE.
*/

#include "noftypes.h"
#include "nes_mmc.h"
#include "nes.h"
#include "libsnss.h"
#include "nes6502.h"
#include "savestate.h"

/* MMC3 refait d'apres le fonctionnement du vrai circuit.
      Le nofrendo d'origine approchait le compteur d'interruptions ; beaucoup
      de jeux de 1990 et plus restaient bloques ou sur un ecran gris
      (Fire'n Ice, Star Wars, Rampart, Twin Cobra, G.I. Joe 2...).
      - Compteur : "horloge" une fois par ligne quand l'image est affichee,
        lignes 0 a 239 ET ligne 261 (pre-rendu). A chaque horloge : s'il vaut 0
        ou si un rechargement est demande ($C001), il reprend la valeur de
        $C000, sinon il diminue de 1 ; s'il vaut alors 0 et que les
        interruptions sont permises ($E001), IRQ.
      - $C001 remet le compteur a 0 et demande le rechargement (a la ligne
        suivante, pas tout de suite).
      - $E000 interdit les interruptions ET acquitte celle qui attendait (elle
        n'arrive plus apres coup : nes6502_irq_clear()).
      - Banques : les 8 registres sont gardes ; un changement de mode ($8000,
        bits 6 et 7) replace TOUTES les banques (avant : seulement la banque
        fixe, le reste attendait la prochaine ecriture). */
static struct
{
   int counter, latch;
   bool enabled, reload;
} irq;

static uint8 regs[8];
static uint8 command;
static bool txsrom = false;   /* Mapper 118 (TxSROM : miroirs par les banques graphiques) */

/* TxSROM (Goal! Two, Pro Sport Hockey...) : le bit 7 des registres
   graphiques choisit l'ecran de decor (memoire A ou B de la console). */
static void map118_nt(void)
{
   if (!(command & 0x80))
   {
      const int a = regs[0] >> 7, b = regs[1] >> 7;
      ppu_mirror(a, a, b, b);
   }
   else
   {
      ppu_mirror(regs[2] >> 7, regs[3] >> 7, regs[4] >> 7, regs[5] >> 7);
   }
}

static void map4_sync_prg(void)
{
   const int last = mmc_getinfo()->rom_banks * 2 - 1;   /* banques de 8 Ko */
   if (command & 0x40)
   {
      mmc_bankrom(8, 0x8000, last - 1);
      mmc_bankrom(8, 0xC000, regs[6]);
   }
   else
   {
      mmc_bankrom(8, 0x8000, regs[6]);
      mmc_bankrom(8, 0xC000, last - 1);
   }
   mmc_bankrom(8, 0xA000, regs[7]);
   mmc_bankrom(8, 0xE000, last);
}

static void map4_sync_chr(void)
{
   const uint16 base = (command & 0x80) ? 0x1000 : 0x0000;
   mmc_bankvrom(1, base ^ 0x0000, regs[0] & 0xFE);
   mmc_bankvrom(1, base ^ 0x0400, regs[0] | 0x01);
   mmc_bankvrom(1, base ^ 0x0800, regs[1] & 0xFE);
   mmc_bankvrom(1, base ^ 0x0C00, regs[1] | 0x01);
   mmc_bankvrom(1, base ^ 0x1000, regs[2]);
   mmc_bankvrom(1, base ^ 0x1400, regs[3]);
   mmc_bankvrom(1, base ^ 0x1800, regs[4]);
   mmc_bankvrom(1, base ^ 0x1C00, regs[5]);
}

static void map4_write(uint32 address, uint8 value)
{
   switch (address & 0xE001)
   {
   case 0x8000:
   {
      const uint8 old = command;
      command = value;
      if ((old ^ value) & 0x40)
         map4_sync_prg();
      if ((old ^ value) & 0x80)
      {
         map4_sync_chr();
         if (txsrom)
            map118_nt();
      }
      break;
   }

   case 0x8001:
   {
      const int r = command & 0x07;
      regs[r] = value;
      if (r >= 6)
         map4_sync_prg();
      else
      {
         const uint16 base = (command & 0x80) ? 0x1000 : 0x0000;
         switch (r)
         {
         case 0:
            mmc_bankvrom(1, base ^ 0x0000, value & 0xFE);
            mmc_bankvrom(1, base ^ 0x0400, value | 0x01);
            break;
         case 1:
            mmc_bankvrom(1, base ^ 0x0800, value & 0xFE);
            mmc_bankvrom(1, base ^ 0x0C00, value | 0x01);
            break;
         default:
            mmc_bankvrom(1, base ^ (0x1000 + ((r - 2) << 10)), value);
            break;
         }
         if (txsrom)
            map118_nt();
      }
      break;
   }

   case 0xA000:
      if (!txsrom && 0 == (mmc_getinfo()->flags & ROM_FLAG_FOURSCREEN))
      {
         if (value & 1)
            ppu_mirror(0, 0, 1, 1);  /* horizontal */
         else
            ppu_mirror(0, 1, 0, 1);  /* vertical */
      }
      break;

   case 0xA001:   /* protection de la RAM de sauvegarde : ignoree */
      break;

   case 0xC000:
      irq.latch = value;
      break;

   case 0xC001:
      irq.counter = 0;
      irq.reload = true;
      break;

   case 0xE000:
      irq.enabled = false;
      nes6502_irq_clear();
      break;

   case 0xE001:
      irq.enabled = true;
      break;

   default:
      break;
   }
}

static void map4_hblank(int vblank)
{
   const int line = nes_getcontextptr()->scanline;
   (void)vblank;
   if (line >= 240 && line != 261)
      return;
   if (!ppu_enabled())
      return;
   if (0 == irq.counter || irq.reload)
   {
      irq.counter = irq.latch;
      irq.reload = false;
   }
   else
   {
      irq.counter--;
   }
   if (0 == irq.counter && irq.enabled)
      nes_irq();
}

static void map4_getstate(SnssMapperBlock *state)
{
   state->extraData.mapper4.irqCounter = irq.counter;
   state->extraData.mapper4.irqLatchCounter = irq.latch;
   state->extraData.mapper4.irqCounterEnabled = irq.enabled;
   state->extraData.mapper4.last8000Write = command;
}

static void map4_setstate(SnssMapperBlock *state)
{
   irq.counter = state->extraData.mapper4.irqCounter;
   irq.latch = state->extraData.mapper4.irqLatchCounter;
   irq.enabled = state->extraData.mapper4.irqCounterEnabled;
   command = state->extraData.mapper4.last8000Write;
}

static void map4_init(void)
{
   /* Etat pour la sauvegarde instantanee (savestate.h) */
   ss_var(&irq, sizeof(irq));
   ss_var(&regs, sizeof(regs));
   ss_var(&command, sizeof(command));
   ss_var(&txsrom, sizeof(txsrom));

   int i;
   txsrom = false;
   irq.counter = irq.latch = 0;
   irq.enabled = irq.reload = false;
   command = 0;
   /* Etat de depart d'un vrai MMC3 : R0-R5 = 0,2,4,5,6,7 (CHR), R6 = 0,
      R7 = 1 ; les jeux les ecrivent tous avant de s'en servir. */
   for (i = 0; i < 8; i++)
      regs[i] = 0;
   regs[1] = 2; regs[2] = 4; regs[3] = 5; regs[4] = 6; regs[5] = 7; regs[7] = 1;
   map4_sync_prg();
   if (mmc_getinfo()->vrom_banks)
      map4_sync_chr();
}

static map_memwrite map4_memwrite[] =
    {
        {0x8000, 0xFFFF, map4_write},
        {-1, -1, NULL}};

mapintf_t map4_intf =
    {
        4,             /* mapper number */
        "MMC3",        /* mapper name */
        map4_init,     /* init routine */
        NULL,          /* vblank callback */
        map4_hblank,   /* hblank callback */
        map4_getstate, /* get state (snss) */
        map4_setstate, /* set state (snss) */
        NULL,          /* memory read structure */
        map4_memwrite, /* memory write structure */
        NULL           /* external sound device */
};

/* Mapper 118 (TxSROM) = MMC3 dont les miroirs suivent les banques
      graphiques. */
static void map118_init(void)
{
   map4_init();
   txsrom = true;
   map118_nt();
}

mapintf_t map118_intf =
    {
        118,           /* mapper number */
        "TxSROM",      /* mapper name */
        map118_init,   /* init routine */
        NULL,          /* vblank callback */
        map4_hblank,   /* hblank callback */
        map4_getstate, /* get state (snss) */
        map4_setstate, /* set state (snss) */
        NULL,          /* memory read structure */
        map4_memwrite, /* memory write structure */
        NULL           /* external sound device */
};
