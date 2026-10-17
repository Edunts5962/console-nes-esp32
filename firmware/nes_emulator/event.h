/* vim: set tabstop=3 expandtab:
**
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
** event.h
**
** OS-independent event handling
** $Id: event.h,v 1.2 2001/04/27 14:37:11 neil Exp $
*/
/*
** Modifie depuis la version d'origine de nofrendo : portages ESP32
** (esp32-nesemu d'Espressif, puis Esp32-S3-nes-emulator-by-DSN) et projet
** Console NES ESP32 (2026). Origines et licences : fichier NOTICE.
*/

#ifndef _EVENT_H_
#define _EVENT_H_

#include "nofrendo.h"

#ifdef __cplusplus
extern "C" {
#endif
 
enum
{
   event_none = 0,
    
   event_quit,
   event_insert,
   event_eject,
   event_togglepause,
   event_soft_reset,   
   event_hard_reset,   
   event_snapshot,
   event_toggle_frameskip,
 
   event_state_save,
   event_state_load,
   event_state_slot_0,
   event_state_slot_1,
   event_state_slot_2,
   event_state_slot_3,
   event_state_slot_4,
   event_state_slot_5,
   event_state_slot_6,
   event_state_slot_7,
   event_state_slot_8,
   event_state_slot_9,
 
   event_gui_toggle_oam,
   event_gui_toggle_wave,
   event_gui_toggle_pattern,
   event_gui_pattern_color_up,
   event_gui_pattern_color_down,
   event_gui_toggle_fps,
   event_gui_display_info,
   event_gui_toggle,
 
   event_toggle_channel_0,
   event_toggle_channel_1,
   event_toggle_channel_2,
   event_toggle_channel_3,
   event_toggle_channel_4,
   event_toggle_channel_5,
   event_set_filter_0,
   event_set_filter_1,
   event_set_filter_2,
 
   event_toggle_sprites,
   event_palette_hue_up,
   event_palette_hue_down,
   event_palette_tint_up,
   event_palette_tint_down,
   event_palette_set_default,
   event_palette_set_shady,
 
   event_joypad1_a,
   event_joypad1_b,
   event_joypad1_start,
   event_joypad1_select,
   event_joypad1_up,
   event_joypad1_down,
   event_joypad1_left,
   event_joypad1_right,
 
   event_joypad2_a,
   event_joypad2_b,
   event_joypad2_start,
   event_joypad2_select,
   event_joypad2_up,
   event_joypad2_down,
   event_joypad2_left,
   event_joypad2_right,
 
   event_songup,
   event_songdown,
   event_startsong,
 
   event_osd_1,
   event_osd_2,
   event_osd_3,
   event_osd_4,
   event_osd_5,
   event_osd_6,
   event_osd_7,
   event_osd_8,
   event_osd_9,

   event_last
};
 
typedef void (*event_t)(int code);
 
extern void event_init(void);
extern void event_set(int index, event_t handler);
extern event_t event_get(int index);
extern void event_set_system(system_t type);

#ifdef __cplusplus
}
#endif

#endif 
