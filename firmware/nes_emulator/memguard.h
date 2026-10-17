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
** memguard.h
**
** memory allocation wrapper routines
** $Id: memguard.h,v 1.1.1.1 2001/04/27 07:03:54 neil Exp $
*/
/*
** Modifie depuis la version d'origine de nofrendo : portages ESP32
** (esp32-nesemu d'Espressif, puis Esp32-S3-nes-emulator-by-DSN) et projet
** Console NES ESP32 (2026). Origines et licences : fichier NOTICE.
*/

#ifndef _MEMGUARD_H_
#define _MEMGUARD_H_

#include <stdlib.h>
#include <stdbool.h>  
#include <string.h>  
 
#define mem_checkblocks()
#define mem_checkleaks()
#define mem_cleanup()
#define mem_term()
 
void *mem_alloc(int size, bool prefer_fast_memory);
extern void nes_mem_free(void *ptr);
extern void *nes_mem_calloc(size_t nmemb, size_t size);
extern void *nes_mem_malloc(size_t size);
 
#define NOFRENDO_MALLOC(x)  nes_mem_malloc(x)
#define NOFRENDO_CALLOC(x,y) nes_mem_calloc(x,y)
#define NOFRENDO_FREE(x)   nes_mem_free(x)
#define NOFRENDO_STRDUP(s) strdup(s)

#ifdef __cplusplus
extern "C" {
#endif
 
int nofrendo_log_printf(const char *format, ...);
 
#define log_printf nofrendo_log_printf

#ifdef __cplusplus
}
#endif

#endif
