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
** map046.c
**
** Mapper #46 (Pelican Game Station)
** Implementation by Firebug
** Mapper information courtesy of Kevin Horton
** $Id: map046.c,v 1.2 2001/04/27 14:37:11 neil Exp $
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

static uint8 prg_low_bank;
static uint8 chr_low_bank;
static uint8 prg_high_bank;
static uint8 chr_high_bank;
 
static void map46_set_banks(void)
{ 
  mmc_bankrom(32, 0x8000, (prg_high_bank << 1) | (prg_low_bank));
  mmc_bankvrom(8, 0x0000, (chr_high_bank << 3) | (chr_low_bank));
 
  return;
}
 
static void map46_init(void)
{
   /* Etat pour la sauvegarde instantanee (savestate.h) */
   ss_var(&prg_low_bank, sizeof(prg_low_bank));
   ss_var(&chr_low_bank, sizeof(chr_low_bank));
   ss_var(&prg_high_bank, sizeof(prg_high_bank));
   ss_var(&chr_high_bank, sizeof(chr_high_bank));
 
  /* Banques basses remises a zero aussi (changement de
        jeu sans redemarrer la carte). */
  prg_low_bank = 0x00;
  chr_low_bank = 0x00;
  prg_high_bank = 0x00;
  chr_high_bank = 0x00;
  map46_set_banks();
 
  return;
}
 
static void map46_write(uint32 address, uint8 value)
{ 
  if (address & 0x8000)
  {
    prg_low_bank = value & 0x01;
    chr_low_bank = (value >> 4) & 0x07;
    map46_set_banks();
  }
  else
  {
    prg_high_bank = value & 0x0F;
    chr_high_bank = (value >> 4) & 0x0F;
    map46_set_banks();
  } 
  return;
}
 
static void map46_setstate(SnssMapperBlock *state)
{ 
  UNUSED(state); 
  return;
}
 
static void map46_getstate(SnssMapperBlock *state)
{ 
  UNUSED(state); 
  return;
}

static map_memwrite map46_memwrite[] =
    {
        {0x6000, 0xFFFF, map46_write},
        {-1, -1, NULL}};

mapintf_t map46_intf =
    {
        46,                     /* Mapper number */
        "Pelican Game Station", /* Mapper name */
        map46_init,             /* Initialization routine */
        NULL,                   /* VBlank callback */
        NULL,                   /* HBlank callback */
        map46_getstate,         /* Get state (SNSS) */
        map46_setstate,         /* Set state (SNSS) */
        NULL,                   /* Memory read structure */
        map46_memwrite,         /* Memory write structure */
        NULL                    /* External sound device */
};
