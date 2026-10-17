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
** map042.c
**
** Mapper #42 (Baby Mario bootleg)
** Implementation by Firebug
** Mapper information courtesy of Kevin Horton
** $Id: map042.c,v 1.2 2001/04/27 14:37:11 neil Exp $
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

static struct
{
  bool enabled;
  uint32 counter;
} irq;
 
static void map42_irq_reset(void)
{ 
  irq.enabled = false;
  irq.counter = 0x0000;
 
  return;
}
 
static void map42_init(void)
{
   /* Etat pour la sauvegarde instantanee (savestate.h) */
   ss_var(&irq, sizeof(irq));
 
  mmc_bankrom(8, 0x8000, 0x0C);
  mmc_bankrom(8, 0xA000, 0x0D);
  mmc_bankrom(8, 0xC000, 0x0E);
  mmc_bankrom(8, 0xE000, 0x0F);
 
  map42_irq_reset();
 
  return;
}
 
static void map42_hblank(int vblank)
{ 
  UNUSED(vblank);
 
  if (irq.enabled)
  { 
    irq.counter = irq.counter + 114;
 
    if (irq.counter >= 0x6000)
    { 
      nes_irq(); 
      map42_irq_reset();
    }
  }
}
 
static void map42_write(uint32 address, uint8 value)
{
  switch (address & 0x03)
  { 
  case 0x00:
    mmc_bankrom(8, 0x6000, value & 0x0F);
    break;
  case 0x01:
    if (value & 0x08)
      ppu_mirror(0, 0, 1, 1);  
    else
      ppu_mirror(0, 1, 0, 1);  
    break; 
  case 0x02:
    if (value & 0x02)
      irq.enabled = true;
    else
      map42_irq_reset();
    break;
 
  default:
    break;
  } 
  return;
}
 
static void map42_setstate(SnssMapperBlock *state)
{ 
  UNUSED(state); 
  return;
}
 
static void map42_getstate(SnssMapperBlock *state)
{ 
  UNUSED(state); 
  return;
}

static map_memwrite map42_memwrite[] =
    {
        {0xE000, 0xFFFF, map42_write},
        {-1, -1, NULL}};

mapintf_t map42_intf =
    {
        42,                     /* Mapper number */
        "Baby Mario (bootleg)", /* Mapper name */
        map42_init,             /* Initialization routine */
        NULL,                   /* VBlank callback */
        map42_hblank,           /* HBlank callback */
        map42_getstate,         /* Get state (SNSS) */
        map42_setstate,         /* Set state (SNSS) */
        NULL,                   /* Memory read structure */
        map42_memwrite,         /* Memory write structure */
        NULL                    /* External sound device */
};
