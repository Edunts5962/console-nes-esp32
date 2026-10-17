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
** nes_mmc.h
**
** NES Memory Management Controller (mapper) defines / prototypes
** $Id: nes_mmc.h,v 1.2 2001/04/27 14:37:11 neil Exp $
*/
/*
** Modifie depuis la version d'origine de nofrendo : portages ESP32
** (esp32-nesemu d'Espressif, puis Esp32-S3-nes-emulator-by-DSN) et projet
** Console NES ESP32 (2026). Origines et licences : fichier NOTICE.
*/

#ifndef _NES_MMC_H_
#define _NES_MMC_H_

#include <libsnss.h>
#include <nes_apu.h>

#define  MMC_LASTBANK      -1

typedef struct
{
   uint32 min_range, max_range;
   uint8 (*read_func)(uint32 address);
} map_memread;

typedef struct
{
   uint32 min_range, max_range;
   void (*write_func)(uint32 address, uint8 value);
} map_memwrite;


typedef struct mapintf_s
{
   int number;
   char *name;
   void (*init)(void);
   void (*vblank)(void);
   void (*hblank)(int vblank);
   void (*get_state)(SnssMapperBlock *state);
   void (*set_state)(SnssMapperBlock *state);
   map_memread *mem_read;
   map_memwrite *mem_write;
   apuext_t *sound_ext;
} mapintf_t;


#include <nes_rom.h>
typedef struct mmc_s
{
   mapintf_t *intf;
   rominfo_t *cart;  
} mmc_t;

extern rominfo_t *mmc_getinfo(void);

extern void mmc_bankvrom(int size, uint32 address, int bank);
extern void mmc_bankrom(int size, uint32 address, int bank);
 
extern mmc_t *mmc_create(rominfo_t *rominfo);
extern void mmc_destroy(mmc_t **nes_mmc);

extern void mmc_getcontext(mmc_t *dest_mmc);
extern void mmc_setcontext(mmc_t *src_mmc);

extern bool mmc_peek(int map_num);

extern void mmc_reset(void);

#endif
