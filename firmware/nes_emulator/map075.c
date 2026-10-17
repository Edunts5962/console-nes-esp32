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
** map75.c
**
** mapper 75 interface
** $Id: map075.c,v 1.2 2001/04/27 14:37:11 neil Exp $
*/
/*
** Modifie depuis la version d'origine de nofrendo : portages ESP32
** (esp32-nesemu d'Espressif, puis Esp32-S3-nes-emulator-by-DSN) et projet
** Console NES ESP32 (2026). Origines et licences : fichier NOTICE.
*/

#include "noftypes.h"
#include "nes_mmc.h"
#include "nes_ppu.h"
#include "savestate.h"

static uint8 latch[2];
static uint8 hibits;

/* Routine d'init ajoutee (il n'y en avait pas) pour
      remettre l'etat a zero quand on change de jeu sans redemarrer la carte.
      Au premier jeu, elle ne change rien (deja a zero). */
static void map75_init(void)
{
   /* Etat pour la sauvegarde instantanee (savestate.h) */
   ss_var(&latch, sizeof(latch));
   ss_var(&hibits, sizeof(hibits));

   latch[0] = latch[1] = 0;
   hibits = 0;
}
 
static void map75_write(uint32 address, uint8 value)
{
   switch ((address & 0xF000) >> 12)
   {
   case 0x8:
      mmc_bankrom(8, 0x8000, value);
      break;

   case 0x9:
      hibits = (value & 0x06);

      mmc_bankvrom(4, 0x0000, ((hibits & 0x02) << 3) | latch[0]);
      mmc_bankvrom(4, 0x1000, ((hibits & 0x04) << 2) | latch[1]);

      if (value & 1)
         ppu_mirror(0, 1, 0, 1);  
      else
         ppu_mirror(0, 0, 1, 1);  

      break;

   case 0xA:
      mmc_bankrom(8, 0xA000, value);
      break;

   case 0xC:
      mmc_bankrom(8, 0xC000, value);
      break;

   case 0xE:
      latch[0] = (value & 0x0F);
      mmc_bankvrom(4, 0x0000, ((hibits & 0x02) << 3) | latch[0]);
      break;

   case 0xF:
      latch[1] = (value & 0x0F);
      mmc_bankvrom(4, 0x1000, ((hibits & 0x04) << 2) | latch[1]);
      break;

   default:
      break;
   }
}

static map_memwrite map75_memwrite[] =
    {
        {0x8000, 0xFFFF, map75_write},
        {-1, -1, NULL}};

mapintf_t map75_intf =
    {
        75,             /* mapper number */
        "Konami VRC1",  /* mapper name */
        map75_init,     /* init routine */
        NULL,           /* vblank callback */
        NULL,           /* hblank callback */
        NULL,           /* get state (snss) */
        NULL,           /* set state (snss) */
        NULL,           /* memory read structure */
        map75_memwrite, /* memory write structure */
        NULL            /* external sound device */
};
