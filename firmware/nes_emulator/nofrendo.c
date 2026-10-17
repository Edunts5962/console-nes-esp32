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
** nofrendo.c
**
** Entry point of program
** Note: all architectures should call these functions
** $Id: nofrendo.c,v 1.3 2001/04/27 14:37:11 neil Exp $
*/
/*
** Modifie depuis la version d'origine de nofrendo : portages ESP32
** (esp32-nesemu d'Espressif, puis Esp32-S3-nes-emulator-by-DSN) et projet
** Console NES ESP32 (2026). Origines et licences : fichier NOTICE.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "noftypes.h"
#include "nofrendo.h"
#include "event.h"
#include "nofconfig.h"
#include "log.h"
#include "osd.h"
#include "gui.h"
#include "vid_drv.h"

#include "nes.h"

static struct
{
   char *filename, *nextfilename;
   system_t type, nexttype;

   union
   {
      nes_t *nes;
   } machine;

   int refresh_rate;

   bool quit;
} console;
 
volatile int nofrendo_ticks = 0;

/* Changement de jeu a chaud (sans redemarrer la carte), demande par la
   manette du telephone ou le menu pause. Deroulement :
   1. osd.cpp appelle nofrendo_request_switch() depuis osd_getinput(),
      c'est-a-dire a la toute fin d'une image : la boucle nes_emulate()
      s'arrete juste apres (nes_poweroff()) ;
   2. internal_insert() libere alors la machine (nes_destroy()), une fois
      sortie de la boucle d'emulation - jamais pendant une image, qui
      utilise encore cette memoire (le main_insert() d'origine la liberait
      immediatement, d'ou son absence ici) ;
   3. main_loop() charge le jeu suivant avec la meme sequence qu'au
      premier lancement ; le son, l'ecran et la minuterie restent en place ;
   4. si ce jeu ne se charge pas, retour au dernier jeu qui fonctionnait.
   Rappels vers osd.cpp (prefixe osd_game_) : avant chaque chargement,
   au debut de chaque partie, et en cas d'echec du chargement. */
extern void osd_game_will_load(const char *filename, int is_switch);
extern void osd_game_started(const char *filename);
extern void osd_game_load_failed(const char *filename, int fallback);
static char *last_good_filename = NULL;   /* dernier jeu lance avec succes */
static void timer_isr(void)
{
   nofrendo_ticks++;
}
static void timer_isr_end(void) {}  

static void shutdown_everything(void)
{
   if (console.filename)
   {
      NOFRENDO_FREE(console.filename);
      console.filename = NULL;
   }
   if (console.nextfilename)
   {
      NOFRENDO_FREE(console.nextfilename);
      console.nextfilename = NULL;
   }

   config.close();
   osd_shutdown();
   gui_shutdown();
   vid_shutdown();
   nofrendo_log_shutdown();
}
 
void main_eject(void)
{
   switch (console.type)
   {
   case system_nes:
      nes_poweroff();
      nes_destroy(&(console.machine.nes));
      break;

   default:
      break;
   }

   if (NULL != console.filename)
   {
      NOFRENDO_FREE(console.filename);
      console.filename = NULL;
   }
   console.type = system_unknown;
}
 
void main_quit(void)
{
   console.quit = true;

   main_eject();
 
   if (NULL != console.nextfilename)
   {
      NOFRENDO_FREE(console.nextfilename);
      console.nextfilename = NULL;
   }
   console.nexttype = system_unknown;
}
 
static system_t detect_systemtype(const char *filename)
{
   if (NULL == filename)
      return system_unknown;

   if (0 == nes_isourfile(filename))
      return system_nes;
 
   return system_unknown;
}

static int install_timer(int hertz)
{
   return osd_installtimer(hertz, (void *)timer_isr,
                           (int)timer_isr_end - (int)timer_isr,
                           (void *)&nofrendo_ticks,
                           sizeof(nofrendo_ticks));
}
 
static int internal_insert(const char *filename, system_t type)
{ 
   if (system_autodetect == type)
      type = detect_systemtype(filename);

   /* Nom du jeu precedent libere (il y en a un apres un changement) */
   if (NULL != console.filename)
      NOFRENDO_FREE(console.filename);
   console.filename = NOFRENDO_STRDUP(filename);
   console.type = type;
 
   event_set_system(type);

   switch (console.type)
   {
   case system_nes:
      gui_setrefresh(NES_REFRESH_RATE);

      console.machine.nes = nes_create();
      if (NULL == console.machine.nes)
      {
         nofrendo_log_printf("Failed to create NES instance.\n");
         return -1;
      }

      if (nes_insertcart(console.filename, console.machine.nes))
      {
         /* La machine a deja ete liberee par nes_insertcart() */
         console.machine.nes = NULL;
         return -1;
      }

      vid_setmode(NES_SCREEN_WIDTH, NES_SCREEN_HEIGHT);

      if (install_timer(NES_REFRESH_RATE))
      {
         nes_destroy(&(console.machine.nes));
         return -1;
      }

      /* Partie lancee - memorisee comme dernier jeu qui fonctionne */
      if (NULL != last_good_filename)
         NOFRENDO_FREE(last_good_filename);
      last_good_filename = NOFRENDO_STRDUP(console.filename);
      osd_game_started(console.filename);

      nes_emulate();

      /* Sortie de la boucle d'emulation (changement de jeu
            demande) : la machine est liberee ici, plus aucune image en cours. */
      nes_destroy(&(console.machine.nes));
      break;

   case system_unknown:
   default:
      nofrendo_log_printf("system type unknown, playing nofrendo NES intro.\n");
      if (NULL != console.filename)
         NOFRENDO_FREE(console.filename);
      console.filename = NULL;   /* Sinon liberee deux fois par l'appel suivant */
 
      return internal_insert(filename, system_nes);
   }

   return 0;
}
 
/* Voir le commentaire en tete de fichier. A appeler
      seulement depuis osd_getinput() (fin d'image), pendant une partie. */
void nofrendo_request_switch(const char *filename)
{
   if (NULL != console.nextfilename)
      NOFRENDO_FREE(console.nextfilename);
   console.nextfilename = NOFRENDO_STRDUP(filename);
   console.nexttype = system_nes;
   nes_poweroff();
}

void main_insert(const char *filename, system_t type)
{
   console.nextfilename = NOFRENDO_STRDUP(filename);
   console.nexttype = type;

   main_eject();
}

int nofrendo_main(int argc, char *argv[])
{ 
   console.filename = NULL;
   console.nextfilename = NULL;
   console.type = system_unknown;
   console.nexttype = system_unknown;
   console.refresh_rate = 0;
   console.quit = false;

   if (nofrendo_log_init())
      return -1;

   event_init();

   return osd_main(argc, argv);
}
 
int main_loop(const char *filename, system_t type)
{
   vidinfo_t video;
 
   atexit(shutdown_everything);
   nofrendo_log_printf("[BOOT] main_loop start\n");

   nofrendo_log_printf("[BOOT] config.open...\n");
   if (config.open())
   {
      nofrendo_log_printf("[BOOT] config.open FAILED\n");
      return -1;
   }
   nofrendo_log_printf("[BOOT] config.open OK\n");

   nofrendo_log_printf("[BOOT] osd_init...\n");
   if (osd_init())
   {
      nofrendo_log_printf("[BOOT] osd_init FAILED\n");
      return -1;
   }
   nofrendo_log_printf("[BOOT] osd_init OK\n");

   nofrendo_log_printf("[BOOT] gui_init...\n");
   if (gui_init())
   {
      nofrendo_log_printf("[BOOT] gui_init FAILED\n");
      return -1;
   }
   nofrendo_log_printf("[BOOT] gui_init OK\n");

   nofrendo_log_printf("[BOOT] osd_getvideoinfo...\n");
   osd_getvideoinfo(&video);
   nofrendo_log_printf("[BOOT] vid_init...\n");
   if (vid_init(video.default_width, video.default_height, video.driver))
   {
      nofrendo_log_printf("[BOOT] vid_init FAILED\n");
      return -1;
   }
   nofrendo_log_printf("[BOOT] vid_init OK\n");

   console.nextfilename = NOFRENDO_STRDUP(filename);
   console.nexttype = type;
   nofrendo_log_printf("[BOOT] entering main loop with %s\n", console.nextfilename ? console.nextfilename : "(null)");

   /* Boucle des parties - un tour par jeu, le suivant etant choisi
         par nofrendo_request_switch() pendant la partie en cours. */
   int first = 1;
   while (false == console.quit)
   {
      if (NULL == console.nextfilename)
         return 1;
      osd_game_will_load(console.nextfilename, !first);
      if (internal_insert(console.nextfilename, console.nexttype))
      {
         /* Changement de jeu rate : retour au dernier jeu qui fonctionnait
            (un seul essai : s'il echoue aussi, on abandonne). */
         const int fallback = !first && NULL != last_good_filename
                              && 0 != strcmp(last_good_filename, console.nextfilename);
         osd_game_load_failed(console.nextfilename, fallback);
         if (fallback)
         {
            NOFRENDO_FREE(console.nextfilename);
            console.nextfilename = NOFRENDO_STRDUP(last_good_filename);
            console.nexttype = system_nes;
            continue;
         }
         return 1;
      }
      first = 0;
   }

   return 0;
}
