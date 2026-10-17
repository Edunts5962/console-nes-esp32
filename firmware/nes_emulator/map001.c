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
** map1.c
**
** mapper 1 interface
** $Id: map001.c,v 1.2 2001/04/27 14:37:11 neil Exp $
*/
/*
** Modifie depuis la version d'origine de nofrendo : portages ESP32
** (esp32-nesemu d'Espressif, puis Esp32-S3-nes-emulator-by-DSN) et projet
** Console NES ESP32 (2026). Origines et licences : fichier NOTICE.
*/

#include <string.h>
#include "noftypes.h"
#include "nes_mmc.h"
#include "nes_ppu.h"
#include "savestate.h"

 
/* MMC1 refait d'apres le fonctionnement du vrai circuit.
      Le nofrendo d'origine ne replacait pas les banques fixes quand le mode
      changeait, inversait le choix de la moitie de ROM des cartouches de
      512 Ko (SUROM : Dragon Warrior 3 et 4 bloques au demarrage) et ne la
      changeait qu'a l'ecriture suivante du registre de programme.
      - Ecriture serie : 5 bits, le 5e designe le registre (adresse de la 5e
        ecriture) ; bit 7 : remise a zero (mode de programme 3).
      - Toutes les banques sont replacees apres chaque registre (map1_sync).
      - 512 Ko (SUROM) : le bit 4 du registre CHR 0 choisit la moitie de 256 Ko
        ; la banque "fixe" est la derniere de cette moitie. */
static int bitcount = 0;
static uint8 latch = 0;
static uint8 regs[4];
static int lastreg = 0;

static void map1_sync(void)
{
   const int prg16 = mmc_getinfo()->rom_banks;           /* banques de 16 Ko */
   const int outer = (prg16 > 16 && (regs[1] & 0x10)) ? 16 : 0;
   const int span = (prg16 > 16) ? 16 : prg16;           /* banques par moitie */
   const int bank = regs[3] & 0x0F;
   const uint8 r0 = regs[0];

   /* programme */
   switch ((r0 >> 2) & 3)
   {
   case 0:
   case 1:   /* 32 Ko */
      mmc_bankrom(32, 0x8000, (outer + (bank & 0x0E)) >> 1);
      break;
   case 2:   /* premiere banque fixe en $8000, $C000 au choix */
      mmc_bankrom(16, 0x8000, outer);
      mmc_bankrom(16, 0xC000, outer + bank);
      break;
   default:  /* $8000 au choix, derniere banque fixe en $C000 */
      mmc_bankrom(16, 0x8000, outer + bank);
      mmc_bankrom(16, 0xC000, outer + span - 1);
      break;
   }

   /* graphismes (sans effet pour une cartouche a RAM graphique) */
   if (r0 & 0x10)
   {
      mmc_bankvrom(4, 0x0000, regs[1]);
      mmc_bankvrom(4, 0x1000, regs[2]);
   }
   else
   {
      mmc_bankvrom(8, 0x0000, regs[1] >> 1);
   }
}

/* miroirs : seulement quand le registre 0 est ecrit (comme le nofrendo
   d'origine : avant, ceux de l'en-tete du fichier restent en place) */
static void map1_mirror(void)
{
   switch (regs[0] & 3)
   {
   case 0: ppu_mirror(0, 0, 0, 0); break;
   case 1: ppu_mirror(1, 1, 1, 1); break;
   case 2: ppu_mirror(0, 1, 0, 1); break;   /* vertical */
   default: ppu_mirror(0, 0, 1, 1); break;  /* horizontal */
   }
}

static void map1_write(uint32 address, uint8 value)
{
   if (value & 0x80)
   {
      regs[0] |= 0x0C;
      bitcount = 0;
      latch = 0;
      map1_sync();
      return;
   }

   /* Garde du nofrendo d'origine : une ecriture vers un AUTRE registre
      recommence la serie. Le processeur emule n'ecrit qu'une fois pour un
      INC/ASL... en $8000-$FFFF (le vrai en ecrit deux, et le MMC1 ignore la
      seconde) : sans cette garde, "INC $FFFF" (remise a zero de Bill & Ted,
      Hillsfar, Rocket Ranger...) laissait un bit en trop et decalait tout. */
   {
      const int regnum = (address >> 13) & 3;
      if (regnum != lastreg)
      {
         bitcount = 0;
         latch = 0;
         lastreg = regnum;
      }
   }
   latch |= ((value & 1) << bitcount++);
   if (5 != bitcount)
      return;

   regs[(address >> 13) & 3] = latch;
   bitcount = 0;
   latch = 0;
   if (0 == ((address >> 13) & 3))
      map1_mirror();
   map1_sync();
}

static void map1_init(void)
{
   /* Etat pour la sauvegarde instantanee (savestate.h) */
   ss_var(&bitcount, sizeof(bitcount));
   ss_var(&latch, sizeof(latch));
   ss_var(&regs, sizeof(regs));
   ss_var(&lastreg, sizeof(lastreg));

   bitcount = 0;
   latch = 0;
   lastreg = 0;
   memset(regs, 0, sizeof(regs));
   regs[0] = 0x0C;   /* mode de programme 3 a la mise sous tension */
   /* miroirs de l'en-tete gardes jusqu'a la 1re ecriture du registre 0 */
   {
      const int prg16 = mmc_getinfo()->rom_banks;
      const int span = (prg16 > 16) ? 16 : prg16;
      mmc_bankrom(16, 0x8000, 0);
      mmc_bankrom(16, 0xC000, span - 1);
   }
}

static void map1_getstate(SnssMapperBlock *state)
{
   state->extraData.mapper1.registers[0] = regs[0];
   state->extraData.mapper1.registers[1] = regs[1];
   state->extraData.mapper1.registers[2] = regs[2];
   state->extraData.mapper1.registers[3] = regs[3];
   state->extraData.mapper1.latch = latch;
   state->extraData.mapper1.numberOfBits = bitcount;
}

static void map1_setstate(SnssMapperBlock *state)
{
   regs[0] = state->extraData.mapper1.registers[0];   /* Etait regs[1] */
   regs[1] = state->extraData.mapper1.registers[1];
   regs[2] = state->extraData.mapper1.registers[2];
   regs[3] = state->extraData.mapper1.registers[3];
   latch = state->extraData.mapper1.latch;
   bitcount = state->extraData.mapper1.numberOfBits;
   map1_sync();
}

static map_memwrite map1_memwrite[] =
    {
        {0x8000, 0xFFFF, map1_write},
        {-1, -1, NULL}};

mapintf_t map1_intf =
    {
        1,             /* mapper number */
        "MMC1",        /* mapper name */
        map1_init,     /* init routine */
        NULL,          /* vblank callback */
        NULL,          /* hblank callback */
        map1_getstate, /* get state (snss) */
        map1_setstate, /* set state (snss) */
        NULL,          /* memory read structure */
        map1_memwrite, /* memory write structure */
        NULL           /* external sound device */
};
