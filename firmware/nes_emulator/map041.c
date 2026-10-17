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
** map041.c
**
** Mapper #41 (Caltron 6 in 1)
** Implementation by Firebug
** Mapper information courtesy of Kevin Horton
** $Id: map041.c,v 1.2 2001/04/27 14:37:11 neil Exp $
**
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
#include "log.h"
#include "savestate.h"

static uint8 register_low;
static uint8 register_high;
 
static void map41_set_chr(void)
{ 
  mmc_bankvrom(8, 0x0000, ((register_low >> 1) & 0x0C) | (register_high));
 
  return;
}
 
static void map41_init(void)
{
   /* Etat pour la sauvegarde instantanee (savestate.h) */
   ss_var(&register_low, sizeof(register_low));
   ss_var(&register_high, sizeof(register_high));
  
  register_low = 0x00;
  register_high = 0x00;
  mmc_bankrom(32, 0x8000, 0x00);
  map41_set_chr();
 
  return;
}
 
static void map41_low_write(uint32 address, uint8 value)
{ 
  UNUSED(value);
 
  register_low = (uint8)(address & 0x3F);
  mmc_bankrom(32, 0x8000, register_low & 0x07);
  map41_set_chr();
  if (register_low & 0x20)
    ppu_mirror(0, 0, 1, 1);  
  else
    ppu_mirror(0, 1, 0, 1);  
  return;
}
 
static void map41_high_write(uint32 address, uint8 value)
{ 
  UNUSED(address);
 
  if (register_low & 0x04)
  {
    register_high = value & 0x03;
    map41_set_chr();
  }
 
  return;
}
 
static void map41_setstate(SnssMapperBlock *state)
{ 
  UNUSED(state);
 
  return;
}
 
static void map41_getstate(SnssMapperBlock *state)
{ 
  UNUSED(state); 
  return;
}

static map_memwrite map41_memwrite[] =
    {
        {0x6000, 0x67FF, map41_low_write},
        {0x8000, 0xFFFF, map41_high_write},
        {-1, -1, NULL}};

mapintf_t map41_intf =
    {
        41,               /* Mapper number */
        "Caltron 6 in 1", /* Mapper name */
        map41_init,       /* Initialization routine */
        NULL,             /* VBlank callback */
        NULL,             /* HBlank callback */
        map41_getstate,   /* Get state (SNSS) */
        map41_setstate,   /* Set state (SNSS) */
        NULL,             /* Memory read structure */
        map41_memwrite,   /* Memory write structure */
        NULL              /* External sound device */
};
