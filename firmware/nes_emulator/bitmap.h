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
** bitmap.h
**
** Bitmap object defines / prototypes
** $Id: bitmap.h,v 1.2 2001/04/27 14:37:11 neil Exp $
*/
/*
** Modifie depuis la version d'origine de nofrendo : portages ESP32
** (esp32-nesemu d'Espressif, puis Esp32-S3-nes-emulator-by-DSN) et projet
** Console NES ESP32 (2026). Origines et licences : fichier NOTICE.
*/

#ifndef _BITMAP_H_
#define _BITMAP_H_

#include <stdint.h>   
#include <stdbool.h> 
#include "noftypes.h"  
 
typedef struct rect_s
{
   int16 x, y;
   uint16 w, h;
} rect_t;
 
typedef struct rgb_s
{
   int r, g, b;
} rgb_t;
 
typedef struct bitmap_s
{
   int width;   
   int height;  
   int pitch; 
   bool hardware;     
   uint8 *data;   
 
   uint8 *line[ZERO_LENGTH]; 
} bitmap_t;
 
extern void bmp_clear(const bitmap_t *bitmap, uint8 color);
extern bitmap_t *bmp_create(int width, int height, int overdraw);
extern bitmap_t *bmp_createhw(uint8 *addr, int width, int height, int pitch);
extern void bmp_destroy(bitmap_t **bitmap);

#endif
