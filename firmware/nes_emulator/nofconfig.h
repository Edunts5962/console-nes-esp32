/* Nofrendo Configuration API
**
** This file is in the public domain.
**
** $Id: nofconfig.h,v 1.1 2001/04/27 14:37:11 neil Exp $
*/
/*
** Modifie depuis la version d'origine de nofrendo : portages ESP32
** (esp32-nesemu d'Espressif, puis Esp32-S3-nes-emulator-by-DSN) et projet
** Console NES ESP32 (2026). Origines et licences : fichier NOTICE.
*/

#ifndef _NOFCONFIG_H_
#define _NOFCONFIG_H_

#include <stdbool.h>

#ifndef CONFIG_FILE
#define CONFIG_FILE "nofrendo.cfg"
#endif

typedef struct config_s
{
 
   bool (*open)(void);
 
   void (*close)(void);
 
   int (*read_int)(const char *group, const char *key, int def);
 
   const char *(*read_string)(const char *group, const char *key, const char *def);

   void (*write_int)(const char *group, const char *key, int value);
   void (*write_string)(const char *group, const char *key, const char *value);
   char *filename;
} config_t;

extern config_t config;

#endif 