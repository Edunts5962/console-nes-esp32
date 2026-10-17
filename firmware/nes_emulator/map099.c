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
** map99.c
**
** mapper 99 interface
** $Id: map099.c,v 1.2 2001/04/27 14:37:11 neil Exp $
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
 
static void map99_vromswitch(uint8 value)
{
   int bank = (value & 0x04) >> 2;
   mmc_bankvrom(8, 0x0000, bank);
}
 
static void map99_init(void)
{
   ss_mapper_stateless();   /* Banques seulement (savestate.h) */

   ppu_mirror(0, 1, 2, 3);
   ppu_setvromswitch(map99_vromswitch);
}

mapintf_t map99_intf =
    {
        99,           /* mapper number */
        "VS. System", /* mapper name */
        map99_init,   /* init routine */
        NULL,         /* vblank callback */
        NULL,         /* hblank callback */
        NULL,         /* get state (snss) */
        NULL,         /* set state (snss) */
        NULL,         /* memory read structure */
        NULL,         /* memory write structure */
        NULL          /* external sound device */
};
