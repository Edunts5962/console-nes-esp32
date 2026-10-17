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
** map70.c
**
** mapper 70 interface
** $Id: map070.c,v 1.2 2001/04/27 14:37:11 neil Exp $
*/
/*
** Modifie depuis la version d'origine de nofrendo : portages ESP32
** (esp32-nesemu d'Espressif, puis Esp32-S3-nes-emulator-by-DSN) et projet
** Console NES ESP32 (2026). Origines et licences : fichier NOTICE.
*/

#include "noftypes.h"
#include "nes_mmc.h"
#include "nes_ppu.h"
 
static void map70_write(uint32 address, uint8 value)
{
   UNUSED(address);

   mmc_bankrom(16, 0x8000, (value >> 4) & 0x07);
   mmc_bankvrom(8, 0x0000, value & 0x0F);
 
   if (mmc_getinfo()->flags & ROM_FLAG_FOURSCREEN)
   {
      if (value & 0x80)
         ppu_mirror(0, 0, 1, 1);  
      else
         ppu_mirror(0, 1, 0, 1);  
   }
   else
   {
      int mirror = (value & 0x80) >> 7;
      ppu_mirror(mirror, mirror, mirror, mirror);
   }
}

static map_memwrite map70_memwrite[] =
    {
        {0x8000, 0xFFFF, map70_write},
        {-1, -1, NULL}};

mapintf_t map70_intf =
    {
        70,             /* mapper number */
        "Mapper 70",    /* mapper name */
        NULL,           /* init routine */
        NULL,           /* vblank callback */
        NULL,           /* hblank callback */
        NULL,           /* get state (snss) */
        NULL,           /* set state (snss) */
        NULL,           /* memory read structure */
        map70_memwrite, /* memory write structure */
        NULL            /* external sound device */
};
