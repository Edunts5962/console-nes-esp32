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
** fds_snd.c
**
** Famicom Disk System sound emulation
** $Id: fds_snd.c,v 1.2 2001/04/27 14:37:11 neil Exp $
*/
/*
** Modifie depuis la version d'origine de nofrendo : portages ESP32
** (esp32-nesemu d'Espressif, puis Esp32-S3-nes-emulator-by-DSN) et projet
** Console NES ESP32 (2026). Origines et licences : fichier NOTICE.
*/

#include "noftypes.h"
#include "nes_apu.h"
#include "fds_snd.h"

static int32 fds_incsize = 0;
 
static int32 fds_process(void)
{
   int32 output;
   output = 0;

   return output;
}
 
static void fds_write(uint32 address, uint8 value)
{
   UNUSED(address);
   UNUSED(value);
}
 
static void fds_reset(void)
{
   apu_t apu;

   apu_getcontext(&apu);
   //   fds_incsize = apu.cycle_rate;
   fds_incsize = (int32)(APU_BASEFREQ * 65536.0 / (float)apu.sample_rate);
}

static apu_memwrite fds_memwrite[] =
    {
        {0x4040, 0x4092, fds_write},
        {-1, -1, NULL}};

apuext_t fds_ext =
    {
        NULL,  
        NULL, 
        fds_reset,
        fds_process,
        NULL, 
        fds_memwrite};
