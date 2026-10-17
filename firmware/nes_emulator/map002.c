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
** map2.c
**
** mapper 2 interface
** $Id: map002.c,v 1.2 2001/04/27 14:37:11 neil Exp $
*/
/*
** Modifie depuis la version d'origine de nofrendo : portages ESP32
** (esp32-nesemu d'Espressif, puis Esp32-S3-nes-emulator-by-DSN) et projet
** Console NES ESP32 (2026). Origines et licences : fichier NOTICE.
*/

#include "noftypes.h"
#include "nes_mmc.h"
#include "savestate.h"
 
static void map2_init()
{
   ss_mapper_stateless();   /* Banques seulement (savestate.h) */

   int last_bank = mmc_getinfo()->rom_banks - 1;
   printf("map2_init. last_bank=%d\n", last_bank);
   mmc_bankrom(16, 0xc000, last_bank);
   mmc_bankrom(16, 0x8000, 0);
}

static void map2_write(uint32 address, uint8 value)
{
   UNUSED(address);

   mmc_bankrom(16, 0x8000, value);
}

static map_memwrite map2_memwrite[] =
    {
        {0x8000, 0xFFFF, map2_write},
        {-1, -1, NULL}};

mapintf_t map2_intf =
    {
        2,             /* mapper number */
        "UNROM",       /* mapper name */
        map2_init,     /* init routine */
        NULL,          /* vblank callback */
        NULL,          /* hblank callback */
        NULL,          /* get state (snss) */
        NULL,          /* set state (snss) */
        NULL,          /* memory read structure */
        map2_memwrite, /* memory write structure */
        NULL           /* external sound device */
};
