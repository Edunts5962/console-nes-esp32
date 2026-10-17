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
** event.c
**
** OS-independent event handling
** $Id: event.c,v 1.3 2001/04/27 14:37:11 neil Exp $
*/
/*
** Modifie depuis la version d'origine de nofrendo : portages ESP32
** (esp32-nesemu d'Espressif, puis Esp32-S3-nes-emulator-by-DSN) et projet
** Console NES ESP32 (2026). Origines et licences : fichier NOTICE.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "event.h"
#include "log.h"

static event_t event_handlers[event_last];

void event_init(void)
{
   int i;
   for (i = 0; i < event_last; i++)
   {
      event_handlers[i] = 0;
   }
}

void event_set(int index, event_t handler)
{
   if (index >= 0 && index < event_last)
   {
      event_handlers[index] = handler;
   }
}

event_t event_get(int index)
{
   if (index >= 0 && index < event_last)
   {
      return event_handlers[index];
   }
   return 0;
}

void event_set_system(system_t type)
{
   (void)type;
}

void event_trigger(int index, int para)
{
   if (index >= 0 && index < event_last)
   {
      if (event_handlers[index])
      {
         event_handlers[index](para);
      }
   }
}