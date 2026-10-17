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
** map33.c
**
** mapper 33 interface
** $Id: map033.c,v 1.2 2001/04/27 14:37:11 neil Exp $
*/
/*
** Modifie depuis la version d'origine de nofrendo : portages ESP32
** (esp32-nesemu d'Espressif, puis Esp32-S3-nes-emulator-by-DSN) et projet
** Console NES ESP32 (2026). Origines et licences : fichier NOTICE.
*/

#include "noftypes.h"
#include "nes_mmc.h"
#include "nes_ppu.h"
 
static void map33_write(uint32 address, uint8 value)
{
   int page = (address >> 13) & 3;
   int reg = address & 3;

   switch (page)
   {
   case 0:  
      switch (reg)
      {
      case 0:
         mmc_bankrom(8, 0x8000, value);
         break;

      case 1:
         mmc_bankrom(8, 0xA000, value);
         break;

      case 2:
         mmc_bankvrom(2, 0x0000, value);
         break;

      case 3:
         mmc_bankvrom(2, 0x0800, value);
         break;
      }
      break;

   case 1: 
   {
      int loc = 0x1000 + (reg << 10);
      mmc_bankvrom(1, loc, value);
   }
   break;

   case 2:  
   case 3: 
      switch (reg)
      {
      case 0:
         /* irqs maybe ? */
         //break;

      case 1: 
         if (value & 1)
            ppu_mirror(0, 0, 1, 1); 
         else
            ppu_mirror(0, 1, 0, 1);
         break;

      default:
         break;
      }
      break;
   }
}

static map_memwrite map33_memwrite[] =
    {
        {0x8000, 0xFFFF, map33_write},
        {-1, -1, NULL}};

mapintf_t map33_intf =
    {
        33,             /* mapper number */
        "Taito TC0190", /* mapper name */
        NULL,           /* init routine */
        NULL,           /* vblank callback */
        NULL,           /* hblank callback */
        NULL,           /* get state (snss) */
        NULL,           /* set state (snss) */
        NULL,           /* memory read structure */
        map33_memwrite, /* memory write structure */
        NULL            /* external sound device */
};
