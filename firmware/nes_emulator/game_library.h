// game_library.h - bibliotheque du joueur.
//  - jeux recents (12 au plus, le dernier en tete) et favoris (60 au plus),
//    gardes dans la NVS (memoire de la console, efface seulement par un
//    effacement complet de la flash) ;
//  - fichiers de sauvegarde, dans un dossier "/saves" a cote des jeux
//    (carte SD ou memoire interne, la ou est le jeu) :
//      "/saves/<nom du jeu>.sav"  RAM de sauvegarde de la cartouche (pile)
//      "/saves/<nom du jeu>.st1"  sauvegarde instantanee, emplacements 1 a 3
//      "/saves/<nom du jeu>.sta"  reprise automatique (en quittant le jeu)
//    Le nom du jeu est celui du fichier, sans ".nes". Un jeu deplace d'un
//    emplacement a l'autre retrouve ses sauvegardes (recherche dans les deux).
#ifndef GAME_LIBRARY_H
#define GAME_LIBRARY_H

#include <stdint.h>
#include <stddef.h>

#define LIB_MAX_RECENTS   12
#define LIB_MAX_FAVORITES 60

void lib_init();

// ---- Recents (nom de fichier, ex. "Zelda.nes", et emplacement ROM_WHERE_*) ----
void lib_recent_push(const char *name, int where);
int lib_recent_count();
const char *lib_recent_name(int i);   // 0 : le plus recent
int lib_recent_where(int i);
void lib_recent_remove(const char *name);

// ---- Favoris (par nom de fichier, quel que soit l'emplacement) ----
bool lib_fav_is(const char *name);
// Ajoute ou retire ; renvoie le nouvel etat. Faux aussi si la liste est
// pleine (ajout refuse) : voir lib_fav_full().
bool lib_fav_toggle(const char *name);
bool lib_fav_full();
int lib_fav_count();
const char *lib_fav_name(int i);
// Change a chaque modification des favoris ou des recents (listes a refaire).
uint32_t lib_generation();

// ---- Fichiers de sauvegarde ----
// ext : "sav", "sta", "st1", "st2", "st3". Cherche d'abord a cote du jeu,
// puis sur l'autre emplacement. Renvoie l'emplacement trouve, -1 sinon.
int lib_save_find(const char *game_name, int game_where, const char *ext);
// Lit au plus max octets ; renvoie le nombre lu (-1 : absent).
long lib_save_read(const char *game_name, int game_where, const char *ext, uint8_t *buf, size_t max);
// Taille du fichier (-1 : absent).
long lib_save_size(const char *game_name, int game_where, const char *ext);
// Ecrit a cote du jeu (dossier cree au besoin). true si tout est ecrit.
bool lib_save_write(const char *game_name, int game_where, const char *ext, const uint8_t *buf, size_t len);
bool lib_save_remove(const char *game_name, int game_where, const char *ext);

#endif
