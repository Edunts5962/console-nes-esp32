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
** map11.c
**
** mapper 11 interface
** $Id: map011.c,v 1.2 2001/04/27 14:37:11 neil Exp $
*/
/*
** Modifie depuis la version d'origine de nofrendo : portages ESP32
** (esp32-nesemu d'Espressif, puis Esp32-S3-nes-emulator-by-DSN) et projet
** Console NES ESP32 (2026). Origines et licences : fichier NOTICE.
*/

#include "noftypes.h"
#include "nes_mmc.h"
#include "savestate.h"
 
static void map11_write(uint32 address, uint8 value)
{
   UNUSED(address);

   mmc_bankrom(32, 0x8000, value & 0x0F);
   mmc_bankvrom(8, 0x0000, value >> 4);
}

static void map11_init(void)
{
   ss_mapper_stateless();   /* Banques seulement (savestate.h) */

   mmc_bankrom(32, 0x8000, 0);
   mmc_bankvrom(8, 0x0000, 0);
}

static map_memwrite map11_memwrite[] =
    {
        {0x8000, 0xFFFF, map11_write},
        {-1, -1, NULL}};

mapintf_t map11_intf =
    {
        11,             /* mapper number */
        "Color Dreams", /* mapper name */
        map11_init,     /* init routine */
        NULL,           /* vblank callback */
        NULL,           /* hblank callback */
        NULL,           /* get state (snss) */
        NULL,           /* set state (snss) */
        NULL,           /* memory read structure */
        map11_memwrite, /* memory write structure */
        NULL            /* external sound device */
};
