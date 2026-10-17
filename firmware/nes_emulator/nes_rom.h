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
** nes_rom.h
**
** NES ROM loading/saving related defines / prototypes
** $Id: nes_rom.h,v 1.2 2001/04/27 14:37:11 neil Exp $
*/
/*
** Modifie depuis la version d'origine de nofrendo : portages ESP32
** (esp32-nesemu d'Espressif, puis Esp32-S3-nes-emulator-by-DSN) et projet
** Console NES ESP32 (2026). Origines et licences : fichier NOTICE.
*/

#ifndef _NES_ROM_H_
#define _NES_ROM_H_

#include <unistd.h>
#include <osd.h>

typedef enum
{
   MIRROR_HORIZ   = 0,
   MIRROR_VERT    = 1
} mirror_t;

#define  ROM_FLAG_BATTERY     0x01
#define  ROM_FLAG_TRAINER     0x02
#define  ROM_FLAG_FOURSCREEN  0x04
#define  ROM_FLAG_VERSUS      0x08

typedef struct rominfo_s
{ 
   uint8 *rom, *vrom; 
   uint8 *sram, *vram;
 
   int rom_banks, vrom_banks;
   int sram_banks, vram_banks;

   int mapper_number;
   mirror_t mirror;

   uint8 flags;

   char filename[PATH_MAX + 1];
} rominfo_t;


extern int rom_checkmagic(const char *filename);
extern rominfo_t *rom_load(const char *filename);
extern void rom_free(rominfo_t **rominfo);
extern char *rom_getinfo(rominfo_t *rominfo);


#endif 
