// osd_menu_api.h - interface entre osd.cpp (qui possede la liste des jeux et
// le volume) et ui_lvgl.cpp (menus LVGL). ui_lvgl.cpp ne touche jamais
// directement a game_list ni a la NVS du volume : il passe par ces
// fonctions, pour que les deux interfaces (USE_LVGL_UI = 0 ou 1) lisent et
// ecrivent les memes donnees.
#ifndef OSD_MENU_API_H
#define OSD_MENU_API_H

#include <stdint.h>

// La liste affichee est une PAGE de MAX_GAMES jeux au plus (chaque ligne est
// un objet LVGL, le tas LVGL n'en tient pas plus) ; tous les jeux trouves
// (MAX_GAMES_TOTAL au plus) sont dans le catalogue (game_catalog.h, en PSRAM).
// Toutes les fonctions ci-dessous qui prennent un "index" designent un jeu de
// la page en cours.
#define MAX_GAMES 50
#define MAX_GAMES_TOTAL 1000

// ---- Liste des jeux (racine de la carte SD) ----
int osd_games_count();                       // jeux de la page affichee
// Pages de la liste
int osd_games_total();                       // tous les jeux trouves
int osd_games_matching();                    // jeux qui correspondent au filtre de recherche
int osd_games_page();                        // page affichee (0..)
int osd_games_page_count();                  // nombre de pages (1 au moins)
int osd_games_page_first();                  // rang du 1er jeu de la page parmi les jeux filtres
const char *osd_games_query();               // filtre de recherche en cours ("" : aucun)
int osd_games_dropped();                     // jeux ignores (plus de MAX_GAMES_TOTAL)
// Filtre (mots a trouver dans le nom, "" : tous) et page ; la liste
// affichee (osd_games_count(), osd_game_*()) devient cette page-la.
void osd_games_set_view(const char *query, int page);
const char *osd_game_filename(int index);    // nom du fichier, ex. "Galaga (Europe).nes"
uint32_t osd_game_size(int index);           // taille en octets (0 si inconnue)
bool osd_game_is_bad(int index);             // ROM en liste noire (crash_guard)
void osd_games_rescan_quiet();               // relit la carte SD SANS rien dessiner
void osd_games_select(int index);            // memorise le jeu a lancer

// Emplacement d'un jeu : carte SD ou memoire interne (flash). Memes valeurs
// que ROM_WHERE_* (rom_storage.h), verifie a la compilation dans osd.cpp.
#define OSD_WHERE_SD    0
#define OSD_WHERE_FLASH 1
int osd_game_where(int index);
// Espace libre / total d'un emplacement, en Ko. false (et 0/0) si
// l'emplacement est indisponible (carte absente, pas de partition FAT).
bool osd_storage_info(int where, uint32_t *free_kb, uint32_t *total_kb);
// Diagnostic de la memoire interne : "" si elle fonctionne, sinon un texte
// pour l'ecran (raison + partitions presentes + taille de la flash).
const char *osd_flash_diag();
// Supprime DEFINITIVEMENT le jeu n° index (carte SD ou memoire interne, selon
// son emplacement). La liste n'est pas relue : appeler
// osd_games_rescan_quiet() ensuite. true si le fichier a ete supprime.
bool osd_game_delete(int index);

// Numero de la liste des jeux : change a chaque relecture de la carte (envoi
// ou suppression par le portail, suppression depuis l'ecran). Le telephone le
// renvoie avec sa demande de lancement : une liste relue entre-temps ne lance
// pas le mauvais jeu.
uint8_t osd_games_generation();

// CATALOGUE COMPLET pour la manette du telephone (tous les jeux, pas seulement
// la page affichee). ci : numero du catalogue, 0..osd_catalog_count()-1, ordre
// alphabetique, valable tant que osd_catalog_generation() ne change pas
// (relecture de la carte ; un changement de page ou de recherche ne le change
// pas).
int osd_catalog_count();
uint8_t osd_catalog_generation();
const char *osd_catalog_filename(int ci);    // "" hors du catalogue
uint32_t osd_catalog_size(int ci);
int osd_catalog_where(int ci);               // OSD_WHERE_*
bool osd_catalog_is_bad(int ci);
int osd_games_page_to_catalog(int index);    // jeu de la page -> catalogue (-1 : aucun)
// Rend affichee la page qui contient le jeu ci (recherche gardee s'il y
// correspond, effacee sinon) et donne son index dans cette page (-1 : jeu
// inconnu). Change osd_games_generation() si la page change.
int osd_games_show_catalog(int ci);
// Jeu en cours de partie (numero du catalogue), -1 : aucun.
int osd_games_running_catalog();

// Liste affichee : tous les jeux, favoris (ordre alphabetique) ou recents (le
// dernier joue en tete). La recherche s'applique dans chaque cas.
#define OSD_VIEW_ALL     0
#define OSD_VIEW_FAVS    1
#define OSD_VIEW_RECENTS 2
void osd_games_set_mode(int mode);           // revient a la premiere page
int osd_games_mode();
bool osd_game_is_fav(int index);             // jeu de la page
bool osd_game_toggle_fav(int index);         // nouvel etat (faux aussi si favoris pleins)
bool osd_catalog_is_fav(int ci);
bool osd_catalog_toggle_fav(int ci);
bool osd_fav_full();
int osd_fav_count();
int osd_recent_count();
int osd_recent_catalog(int i);               // recent n° i -> catalogue (-1 : supprime)
uint32_t osd_library_generation();           // change avec les favoris / recents
// "Reprendre" : dernier jeu joue encore present (numero du catalogue, -1 :
// aucun) ; *has_state : une reprise automatique existe (etat garde en
// quittant le jeu).
int osd_resume_catalog(bool *has_state);
// Le prochain lancement charge la reprise automatique du jeu.
void osd_request_resume(bool on);
// Une reprise automatique existe pour ce jeu de la page / du catalogue.
bool osd_game_has_resume(int index);
bool osd_catalog_has_resume(int ci);
// Nom du fichier du jeu de rang pos parmi les jeux de la liste (filtre et
// mode compris, toutes pages) : "" hors limites. Pour "Aller a la page".
const char *osd_games_name_at(int pos);

// Jeux ecartes apres un plantage au demarrage (dossier "/quarantaine",
// rom_storage.h). osd_quarantine_refresh() relit la liste (les deux
// emplacements) et renvoie leur nombre ; i : 0..nombre-1.
#define OSD_QUARANTINE_MAX 40
int osd_quarantine_refresh();
int osd_quarantine_count();
const char *osd_quarantine_name(int i);
int osd_quarantine_where(int i);
uint32_t osd_quarantine_size(int i);
// Remet le jeu dans la liste : NULL si reussi, sinon le motif (texte court).
const char *osd_quarantine_restore(int i);
bool osd_quarantine_delete(int i);
// Message a montrer une fois a l'ouverture des menus (jeu ecarte apres un
// plantage, reprise effacee...) ; NULL : aucun.
const char *osd_take_notice();

// Vrai si la liste des jeux peut s'afficher sur la television (sortie
// Televiseur choisie et liaison video prete).
bool osd_tv_list_available();

// Controle d'un jeu AVANT de le lancer (en-tete iNES, taille du fichier,
// mapper gere par l'emulateur) : un jeu impossible a charger n'est pas lance
// et n'interrompt pas la partie en cours.
#define OSD_PRECHECK_OK         0
#define OSD_PRECHECK_NOFILE     1   // fichier introuvable ou illisible
#define OSD_PRECHECK_NOT_NES    2   // pas d'en-tete iNES
#define OSD_PRECHECK_TRUNCATED  3   // fichier plus court que l'en-tete ne l'annonce
#define OSD_PRECHECK_MAPPER     4   // mapper non pris en charge (*mapper rempli)
int osd_game_precheck(int index, int *mapper);
// Texte court pour l'ecran (UTF-8), ex. "Mapper 85 non pris en charge".
const char *osd_precheck_text(int code, int mapper);

// ---- Volume logiciel (master_volume, 0..200 %) ----
int osd_get_volume();
void osd_set_volume(int percent);            // borne a 0..200, effet immediat
void osd_volume_commit();                    // ecrit en NVS ("nesvol") seulement si modifie
// Volume regle depuis le telephone : applique, ecrit en NVS, et montre par la
// barre de volume pendant une partie.
void osd_volume_remote(int percent);

#endif
