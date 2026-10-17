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
** map050.c
**
** Mapper #50 (SMB2j - 3rd discovered variation)
** Implementation by Firebug
** Mapper information courtesy of Kevin Horton
** $Id: map050.c,v 1.2 2001/04/27 14:37:11 neil Exp $
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
 
static void map50_irq_reset(void)
{ 
  irq.enabled = false;
  irq.counter = 0x0000; 
  return;
}
 
static void map50_init(void)
{
   /* Etat pour la sauvegarde instantanee (savestate.h) */
   ss_var(&irq, sizeof(irq));
 
  mmc_bankrom(8, 0x6000, 0x0F);
  mmc_bankrom(8, 0x8000, 0x08);
  mmc_bankrom(8, 0xA000, 0x09);
  mmc_bankrom(8, 0xE000, 0x0B); 
  map50_irq_reset();
 
  return;
}
 
static void map50_hblank(int vblank)
{ 
  UNUSED(vblank); 
  if (irq.enabled)
  { 
    irq.counter = irq.counter + 114; 
    if (irq.counter & 0x1000)
    { 
      nes_irq(); 
      map50_irq_reset();
    }
  }
}
  
static void map50_write(uint32 address, uint8 value)
{
  uint8 selectable_bank; 
  if ((address & 0x60) != 0x20)
    return;

  /* A8 low  = $C000-$DFFF page selection */
  /* A8 high = IRQ timer toggle */
  if (address & 0x100)
  { 
    if (value & 0x01)
      irq.enabled = true;
    else
      map50_irq_reset();
  }
  else
  { 
    selectable_bank = 0x00;
    if (value & 0x08)
      selectable_bank |= 0x08;
    if (value & 0x04)
      selectable_bank |= 0x02;
    if (value & 0x02)
      selectable_bank |= 0x01;
    if (value & 0x01)
      selectable_bank |= 0x04;
    mmc_bankrom(8, 0xC000, selectable_bank);
  } 
  return;
}
 
static void map50_setstate(SnssMapperBlock *state)
{ 
  UNUSED(state); 
  return;
}
 
static void map50_getstate(SnssMapperBlock *state)
{ 
  UNUSED(state); 
  return;
}

static map_memwrite map50_memwrite[] =
    {
        {0x4000, 0x5FFF, map50_write},
        {-1, -1, NULL}};

mapintf_t map50_intf =
    {
        50,                               /* Mapper number */
        "SMB2j (3rd discovered variant)", /* Mapper name */
        map50_init,                       /* Initialization routine */
        NULL,                             /* VBlank callback */
        map50_hblank,                     /* HBlank callback */
        map50_getstate,                   /* Get state (SNSS) */
        map50_setstate,                   /* Set state (SNSS) */
        NULL,                             /* Memory read structure */
        map50_memwrite,                   /* Memory write structure */
        NULL                              /* External sound device */
};
