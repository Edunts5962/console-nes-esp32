/* savestate.h - sauvegarde instantanee (etat complet de la console emulee) et
 * RAM de sauvegarde des cartouches.
 *
 * L'etat est une image binaire de tout ce qui change pendant une partie :
 * processeur (registres, banques), RAM de 2 Ko, PPU (registres internes,
 * ecrans, sprites, palette, banques graphiques), APU (canaux), compteurs de
 * nes.c, RAM de sauvegarde, RAM graphique, et l'etat propre au mapper.
 *
 * Mappers : chacun declare ses variables dans son init (ss_var...). Un
 * mapper qui ne declare rien n'est PAS pris en charge (ss_supported() faux)
 * : mieux vaut refuser que recharger un etat faux.
 *
 * A n'appeler qu'entre deux images (osd_getinput()), jamais pendant
 * l'emulation d'une image.
 */
#ifndef _SAVESTATE_H_
#define _SAVESTATE_H_

#include <stddef.h>
#include "noftypes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- cote mappers (dans leur fonction init) ---- */
/* Le mapper n'a pas d'etat propre (banques seulement) : pris en charge. */
void ss_mapper_stateless(void);
/* Variable du mapper a sauver telle quelle (octets). Pris en charge. */
void ss_var(void *addr, size_t size);
/* Pointeur du mapper (uint8 *) vers une zone connue (ROM, RAM...). */
void ss_ptr(uint8 **pp);
/* Zone de memoire propre au mapper (RAM de programme du MMC5...) : les
   pointeurs qui y menent sont reconnus ; save_content : son contenu est
   sauve aussi. */
void ss_region(uint8 *base, uint32 size, bool save_content);
/* Appele apres le chargement d'un etat (recalculs internes du mapper). */
void ss_post_load(void (*fn)(void));
/* RAM de sauvegarde de cartouche autre que rominfo->sram (MMC5 : 64 Ko). */
void ss_battery_region(uint8 *base, uint32 size);

/* reinitialise la liste (nes_mmc.c, avant l'init du mapper) */
void ss_registry_reset(void);

/* ---- cote console (osd.cpp) ---- */
bool ss_supported(void);
/* Etat courant dans un tampon alloue (a liberer par ss_free) ; NULL si
   impossible (mapper non pris en charge, pas de memoire). */
uint8 *ss_save(size_t *len);
void ss_free(uint8 *buf);
/* 0 : charge ; <0 : refuse (autre jeu, autre version, fichier abime) -
   rien n'est modifie en cas de refus. */
int ss_load(const uint8 *buf, size_t len);
const char *ss_error_text(int code);

/* RAM de sauvegarde de la cartouche (pile) : zone et taille (0 : aucune). */
uint8 *ss_battery_ram(uint32 *size);
/* Empreinte (CRC-32) de la ROM du jeu en cours (calculee une fois). */
uint32 ss_rom_crc(void);

#ifdef __cplusplus
}
#endif

#endif
