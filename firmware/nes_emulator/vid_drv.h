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
** vid_drv.h
**
** Video driver
** $Id: vid_drv.h,v 1.2 2001/04/27 14:37:11 neil Exp $
*/
/*
** Modifie depuis la version d'origine de nofrendo : portages ESP32
** (esp32-nesemu d'Espressif, puis Esp32-S3-nes-emulator-by-DSN) et projet
** Console NES ESP32 (2026). Origines et licences : fichier NOTICE.
*/

#ifndef _VID_DRV_H_
#define _VID_DRV_H_

#include "bitmap.h"

typedef struct viddriver_s
{ 
   const char *name; 
   int (*init)(int width, int height); 
   void (*shutdown)(void); 
   int (*set_mode)(int width, int height); 
   void (*set_palette)(rgb_t *palette); 
   void (*clear)(uint8 color); 
   bitmap_t *(*lock_write)(void); 
   void (*free_write)(int num_dirties, rect_t *dirty_rects); 
   void (*custom_blit)(bitmap_t *primary, int num_dirties,
                       rect_t *dirty_rects); 
   bool invalidate;
} viddriver_t;
 
extern bitmap_t *vid_getbuffer(void);

extern int vid_init(int width, int height, viddriver_t *osd_driver);
extern void vid_shutdown(void);

extern int vid_setmode(int width, int height);
extern void vid_setpalette(rgb_t *pal);

extern void vid_blit(bitmap_t *bitmap, int src_x, int src_y, int dest_x,
                     int dest_y, int blit_width, int blit_height);
extern void vid_flush(void);

#endif
