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
** nes.h
**
** NES hardware related definitions / prototypes
** $Id: nes.h,v 1.2 2001/04/27 14:37:11 neil Exp $
*/
/*
** Modifie depuis la version d'origine de nofrendo : portages ESP32
** (esp32-nesemu d'Espressif, puis Esp32-S3-nes-emulator-by-DSN) et projet
** Console NES ESP32 (2026). Origines et licences : fichier NOTICE.
*/

#ifndef _NES_H_
#define _NES_H_

#include <noftypes.h>
#include <nes_apu.h>
#include <nes_mmc.h>
#include <nes_ppu.h>
#include <nes_rom.h>
#include "nes6502.h"
#include <bitmap.h>
 
#ifndef NES_VISIBLE_HEIGHT
#define  NES_VISIBLE_HEIGHT   224
#endif 
#define  NES_SCREEN_WIDTH     256
#define  NES_SCREEN_HEIGHT    240
 
#ifdef PAL
#define  NES_REFRESH_RATE     50
#else 
#define  NES_REFRESH_RATE     60
#endif  

#define  MAX_MEM_HANDLERS     32

enum
{
   SOFT_RESET,
   HARD_RESET
};


typedef struct nes_s
{ 
   nes6502_context *cpu;
   nes6502_memread readhandler[MAX_MEM_HANDLERS];
   nes6502_memwrite writehandler[MAX_MEM_HANDLERS];

   ppu_t *ppu;
   apu_t *apu;
   mmc_t *mmc;
   rominfo_t *rominfo;
 
//   bitmap_t *vidbuf; 

   bool fiq_occurred;
   uint8 fiq_state;
   int fiq_cycles;

   int scanline;
 
   float scanline_cycles;
   bool autoframeskip;
 
   bool poweroff;
   bool pause;

} nes_t;


extern int nes_isourfile(const char *filename); 
extern nes_t *nes_getcontextptr(void);
 
extern void nes_getcontext(nes_t *machine);
extern void nes_setcontext(nes_t *machine);

extern nes_t *nes_create(void);
extern void nes_destroy(nes_t **machine);
extern int nes_insertcart(const char *filename, nes_t *machine);

extern void nes_setfiq(uint8 state);
extern void nes_nmi(void);
extern void nes_irq(void);
extern void nes_emulate(void);

extern void nes_reset(int reset_type);

extern void nes_poweroff(void);
extern void nes_togglepause(void);

#endif
