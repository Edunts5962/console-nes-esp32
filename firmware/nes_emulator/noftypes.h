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
** types.h
**
** Data type definitions
** $Id: noftypes.h,v 1.1 2001/04/27 14:37:11 neil Exp $
*/
/*
** Modifie depuis la version d'origine de nofrendo : portages ESP32
** (esp32-nesemu d'Espressif, puis Esp32-S3-nes-emulator-by-DSN) et projet
** Console NES ESP32 (2026). Origines et licences : fichier NOTICE.
*/

#ifndef _NOFTYPES_H_
#define _NOFTYPES_H_

#include <stdbool.h>

#define NOFRENDO_DEBUG
#define NOFRENDO_MEM_DEBUG
// #define NOFRENDO_VRAM_DEBUG
// #define NOFRENDO_LOG_TO_FILE 
// #define NOFRENDO_DOUBLE_FRAMEBUFFER
 
#define HOST_LITTLE_ENDIAN

#ifdef __GNUC__
#define INLINE static inline
#define ZERO_LENGTH 0
#elif defined(WIN32)
#define INLINE static __inline
#define ZERO_LENGTH 0
#else  
#define INLINE static
#define ZERO_LENGTH 1
#endif
 
#define UNUSED(x) ((x) = (x))

typedef signed char int8;
typedef signed short int16;
typedef signed int int32;
typedef unsigned char uint8;
typedef unsigned short uint16;
typedef unsigned int uint32;

#include "memguard.h"
#include "log.h"

#ifdef NOFRENDO_DEBUG

#define ASSERT(expr) nofrendo_log_assert((int)(expr), __LINE__, __FILE__, NULL)
#define ASSERT_MSG(msg) nofrendo_log_assert(false, __LINE__, __FILE__, (msg))

#else  

#define ASSERT(expr)
#define ASSERT_MSG(msg)

#endif  

#endif
