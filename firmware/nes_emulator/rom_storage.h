// rom_storage.h - les deux emplacements des jeux (.nes) :
//  - la carte microSD (SD_MMC, montee sur "/sdcard") ;
//  - la "memoire interne" : partition FAT de la flash de 16 Mo (schema de
//    partition "16M Flash (3MB APP/9.9MB FATFS)", ~9,6 Mo utilisables).
//
// Dans les deux cas, le jeu est copie en entier en PSRAM au lancement
// (osd_rom_open()/osd_rom_read(), osd.cpp) : la vitesse de jeu est la meme ;
// seule la lecture au lancement change, et un jeu en memoire interne reste
// jouable sans carte SD.
//
// Chemins : un jeu de la carte SD est designe par "/nom.nes", un jeu en
// memoire interne par "flash:/nom.nes". rom_storage_split() fait la
// conversion vers le bon systeme de fichiers.
#ifndef ROM_STORAGE_H
#define ROM_STORAGE_H

#include <stdint.h>
#include <stddef.h>

#define ROM_WHERE_SD    0
#define ROM_WHERE_FLASH 1

// Point de montage de la carte SD (SD_MMC.begin(), nes_emulator.ino) -
// aussi utilise par le formatage de la carte depuis le portail.
#define ROM_SD_MOUNTPOINT "/sdcard"

// Suffixes des fichiers temporaires d'un envoi par le portail web (voir
// wifi_portal.cpp et rom_storage_cleanup_leftovers()).
#define ROM_UPLOAD_TMP_SUFFIX "~tmp"
#define ROM_UPLOAD_OLD_SUFFIX "~old"
// L'envoi en cours s'ecrit dans ce nom COURT (8.3, une seule entree de
// repertoire) au lieu de "nom.nes~tmp" (un nom long de plus) - voir
// rom_storage_root_entries().
#define ROM_UPLOAD_TMP_NAME "/NESUPLD.TMP"

#define ROM_FLASH_PREFIX     "flash:"
#define ROM_FLASH_PREFIX_LEN 6

// Longueur maximale d'un chemin complet ("flash:/" + nom de 63 caracteres
// + caractere nul), a utiliser pour tous les tampons de chemin.
#define ROM_PATH_MAX 80

// A appeler une fois dans setup(), AVANT le montage de la carte SD.
// Monte la partition FAT interne ("ffat") ; la formate automatiquement la
// toute premiere fois (quelques secondes). Renvoie false si le schema de
// partition choisi dans l'IDE n'a pas de partition FAT (la fonction
// memoire interne est alors simplement masquee partout).
// first_format_cb (facultatif) est appele juste avant ce premier
// formatage, qui peut durer jusqu'a ~2 minutes (message a l'ecran).
// (Declaree plus bas, partie C++ : argument par defaut.)

// Etat des deux emplacements.
bool rom_storage_flash_ok();
// Diagnostic : raison courte si la memoire interne est indisponible ("aucune
// partition FAT", "montage impossible (ffat)"), "" sinon ; et detail
// (partitions presentes, taille de la flash).
const char *rom_storage_flash_reason();
const char *rom_storage_flash_detail();
bool rom_storage_sd_ok();
void rom_storage_set_sd_ok(bool ok);   // appele par setup() apres le montage SD
bool rom_storage_available(int where);

// Espace (en octets) d'un emplacement ; 0 si indisponible.
uint64_t rom_storage_total_bytes(int where);
uint64_t rom_storage_used_bytes(int where);
uint64_t rom_storage_free_bytes(int where);

// Construit le chemin complet d'un jeu ("/nom.nes" ou "flash:/nom.nes").
// Renvoie false si le chemin a du etre tronque (nom trop long).
bool rom_storage_make_path(int where, const char *name, char *out, size_t out_len);

// Decoupe un chemin complet : renvoie l'emplacement et, dans *fs_path, le
// chemin dans ce systeme de fichiers ("/nom.nes").
int rom_storage_split(const char *path, const char **fs_path);

#ifdef __cplusplus
#include "FS.h"
bool rom_storage_flash_begin(void (*first_format_cb)() = nullptr);
// Systeme de fichiers d'un emplacement (NULL si indisponible).
fs::FS *rom_storage_fs(int where);
// Ouvre un chemin complet en lecture (File invalide si impossible).
fs::File rom_storage_open_read(const char *path);
// Supprime un chemin complet. true si le fichier a ete supprime.
bool rom_storage_remove(const char *path);
#endif

// Deplace un jeu d'un emplacement a l'autre (copie, verification de la
// taille, puis suppression de l'original). name = nom du fichier sans '/'.
// Renvoie 0 si reussi, sinon un code ROM_MOVE_ERR_*.
#define ROM_MOVE_OK          0
#define ROM_MOVE_ERR_UNAVAIL 1   // l'un des deux emplacements est absent
#define ROM_MOVE_ERR_NOFILE  2   // le jeu n'existe pas a l'origine
#define ROM_MOVE_ERR_EXISTS  3   // un jeu du meme nom existe deja a destination
#define ROM_MOVE_ERR_SPACE   4   // pas assez de place a destination
#define ROM_MOVE_ERR_IO      5   // erreur de lecture/ecriture (copie annulee)
#define ROM_MOVE_ERR_ROOT    6   // Racine pleine a destination (FAT12/16, voir plus bas)
int rom_storage_move(const char *name, int from, int to);

// ---- Maintenance ----
// Supprime tous les jeux (.nes) a la racine d'un emplacement ; les autres
// fichiers ne sont pas touches. Renvoie le nombre de jeux supprimes, ou -1 si
// l'emplacement est absent.
int rom_storage_delete_all_games(int where);
// A appeler au demarrage, une fois la carte SD montee : supprime les restes
// d'envois interrompus ("nom.nes~tmp") et remet en place un jeu dont le
// remplacement a ete interrompu ("nom.nes~old" sans "nom.nes").
void rom_storage_cleanup_leftovers();
// Efface ENTIEREMENT la memoire interne (formatage rapide de la partition
// FAT, qui ne sert qu'aux jeux) puis la remonte.
bool rom_storage_flash_erase();
// Formate la carte SD montee : efface TOUT son contenu, y compris ce qui
// n'est pas un jeu. Uniquement si la carte est montee (une carte illisible
// doit etre formatee en FAT32 sur un ordinateur).
// Toujours en FAT32 (grappes choisies pour : avant, une carte de 2 Go ou moins
// devenait FAT16, racine limitee a 512 noms).
bool rom_storage_sd_format();

// ---- racine des volumes FAT12 / FAT16 ----
// Les jeux sont a la racine. En FAT12/FAT16, la racine a un nombre FIXE
// d'entrees (512 en general) et chaque nom long en prend 1 + 1 par tranche
// de 13 caracteres ("426 Rampart.nes" : 3, "005 The Legend of Zelda
// (VF).nes" : 4) : environ 150 jeux, quelle que soit la place libre.
// Cartes SD de 2 Go ou moins (FAT16 d'usine) et memoire interne (FAT12).
// En FAT32, la racine grandit a la demande (pas de limite pratique).
int rom_storage_fat_type(int where);   // 12, 16 ou 32 ; 0 : inconnu ou absent
// FAT12/16 : entrees de la racine utilisees (estimation d'apres les noms,
// etiquette de volume comprise) et nombre total. false : FAT32, absent.
bool rom_storage_root_entries(int where, int *used, int *total);
// Entrees de repertoire prises par un nom (1 pour un nom court 8.3).
int rom_storage_name_entries(const char *name);
// Journal du demarrage : type de FAT et racine des deux emplacements.
void rom_storage_log_format();

// ---- jeux ecartes ("quarantaine") ----
// Un jeu qui fait planter la console au demarrage n'est pas supprime : il est
// range dans le dossier "/quarantaine" du meme emplacement (hors de la liste
// des jeux, qui ne lit que la racine). Reglages > Jeux ecartes et le portail
// permettent de le remettre dans la liste ou de le supprimer.
#define ROM_QUARANTINE_DIR "/quarantaine"
// Range le jeu (chemin complet "/nom.nes" ou "flash:/nom.nes"). true si deplace.
bool rom_storage_quarantine(const char *path);
// Liste les jeux ecartes des deux emplacements : cb(nom, emplacement,
// taille, ctx) pour chacun ; renvoie leur nombre.
int rom_storage_quarantine_list(void (*cb)(const char *name, int where, uint32_t size, void *ctx), void *ctx);
// Remet le jeu a la racine (ROM_MOVE_OK, ROM_MOVE_ERR_EXISTS si un jeu du
// meme nom y est, ROM_MOVE_ERR_ROOT si la racine est pleine...).
int rom_storage_quarantine_restore(int where, const char *name);
// Supprime definitivement le jeu ecarte.
bool rom_storage_quarantine_remove(int where, const char *name);

#endif
