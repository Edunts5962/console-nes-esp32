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
** gui_elem.h
**
** GUI elements (font, mouse pointer, etc.)
** $Id: gui_elem.h,v 1.1.1.1 2001/04/27 07:03:54 neil Exp $
*/
/*
** Modifie depuis la version d'origine de nofrendo : portages ESP32
** (esp32-nesemu d'Espressif, puis Esp32-S3-nes-emulator-by-DSN) et projet
** Console NES ESP32 (2026). Origines et licences : fichier NOTICE.
*/

#ifndef _GUI_ELEM_H_
#define _GUI_ELEM_H_

typedef struct fontchar_s
{
   uint8 lines[6];
   uint8 spacing;
} fontchar_t;

typedef struct font_s
{
   fontchar_t *character;
   uint8 height;
} font_t;

extern font_t small;

#define CURSOR_WIDTH 11
#define CURSOR_HEIGHT 19

extern const uint8 cursor_color[];
extern const uint8 cursor[];

#endif
