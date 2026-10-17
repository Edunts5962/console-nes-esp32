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
** map073.c
**
** Mapper #73 (Konami VRC3)
** Implementation by Firebug
** $Id: map073.c,v 1.2 2001/04/27 14:37:11 neil Exp $
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
 
static void map73_init(void)
{
   /* Etat pour la sauvegarde instantanee (savestate.h) */
   ss_var(&irq, sizeof(irq));
 
  irq.enabled = false;
  irq.counter = 0x0000; 
  return;
}
 
static void map73_hblank(int vblank)
{ 
  UNUSED(vblank); 
  if (irq.enabled)
  { 
    irq.counter = irq.counter + 114; 
    if (irq.counter & 0x10000)
    { 
      irq.counter &= 0xFFFF; 
      nes_irq(); 
      irq.enabled = false;
    }
  }
}
 
static void map73_write(uint32 address, uint8 value)
{
  switch (address & 0xF000)
  {
  case 0x8000:
    irq.counter &= 0xFFF0;
    irq.counter |= (uint32)(value);
    break;
  case 0x9000:
    irq.counter &= 0xFF0F;
    irq.counter |= (uint32)(value << 4);
    break;
  case 0xA000:
    irq.counter &= 0xF0FF;
    irq.counter |= (uint32)(value << 8);
    break;
  case 0xB000:
    irq.counter &= 0x0FFF;
    irq.counter |= (uint32)(value << 12);
    break;
  case 0xC000:
    if (value & 0x02)
      irq.enabled = true;
    else
      irq.enabled = false;
    break;
  case 0xF000:
    mmc_bankrom(16, 0x8000, value);
  default:
    break;
  } 
  return;
}
 
static void map73_setstate(SnssMapperBlock *state)
{ 
  UNUSED(state); 
  return;
}
 
static void map73_getstate(SnssMapperBlock *state)
{ 
  UNUSED(state); 
  return;
}

static map_memwrite map73_memwrite[] =
    {
        {0x8000, 0xFFFF, map73_write},
        {-1, -1, NULL}};

mapintf_t map73_intf =
    {
        73,             /* Mapper number */
        "Konami VRC3",  /* Mapper name */
        map73_init,     /* Initialization routine */
        NULL,           /* VBlank callback */
        map73_hblank,   /* HBlank callback */
        map73_getstate, /* Get state (SNSS) */
        map73_setstate, /* Set state (SNSS) */
        NULL,           /* Memory read structure */
        map73_memwrite, /* Memory write structure */
        NULL            /* External sound device */
};
