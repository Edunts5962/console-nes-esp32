// game_catalog.h - catalogue de TOUS les jeux trouves (memoire interne + carte
// SD), jusqu'a MAX_GAMES_TOTAL, et la "page" de la liste affichee (MAX_GAMES
// jeux au plus, filtre de recherche compris).
//
// Pourquoi des pages : chaque ligne de la liste des menus est un objet
// LVGL pris dans le tas LVGL (48 Ko de memoire interne, lv_conf.h) - 50
// lignes y tiennent, 800 non. Le catalogue (64 octets par jeu) est en PSRAM ;
// seule la page en cours devient des lignes a l'ecran. Toutes les fonctions
// osd_game_*(index) (osd_menu_api.h) designent un jeu DE LA PAGE en cours.
//
// Sans Arduino ni LVGL : compile aussi dans le simulateur des menus (PC).
#ifndef GAME_CATALOG_H
#define GAME_CATALOG_H

#include <stdint.h>
#include <stddef.h>

#ifndef MAX_GAMES_TOTAL
#define MAX_GAMES_TOTAL 1000
#endif

typedef struct {
  char name[64];      // nom du fichier, ex. "001 Super Mario Bros.nes"
  uint32_t size;      // octets
  uint8_t where;      // ROM_WHERE_SD / ROM_WHERE_FLASH
} CatalogEntry;

// Vide le catalogue (avant une relecture). false : pas de memoire.
bool catalog_reset();
// Ajoute un jeu ; false si le catalogue est plein (MAX_GAMES_TOTAL).
bool catalog_add(const char *name, uint32_t size, uint8_t where);
// Tri alphabetique sans tenir compte des majuscules ; a nom egal, carte SD
// (where = 0) avant memoire interne.
void catalog_sort();
int catalog_total();
int catalog_capacity();
// Jeux ignores parce que le catalogue etait plein (derniere relecture).
int catalog_dropped();

// Choisit la page affichee : jeux dont le nom contient TOUS les mots de
// query (sans tenir compte des majuscules, extension ".nes" exclue ; "" :
// tous), page n° page (bornee). per_page : jeux par page (MAX_GAMES).
void catalog_set_view(const char *query, int page, int per_page);
int catalog_view_matching();    // jeux qui correspondent au filtre
int catalog_view_page();        // page affichee (0..)
int catalog_view_pages();       // nombre de pages (1 au moins)
int catalog_view_first();       // rang du 1er jeu de la page parmi les jeux filtres
int catalog_view_count();       // jeux sur la page
const CatalogEntry *catalog_view_entry(int i);   // i : 0..catalog_view_count()-1
const char *catalog_view_query();

// Comme catalog_set_view(), mais parmi les seuls jeux de order[] (numeros du
// catalogue, dans cet ordre : favoris, recents) ; order NULL : tous les jeux,
// ordre alphabetique.
void catalog_set_view_list(const char *query, int page, int per_page, const int16_t *order, int n_order);

// Recherche par mots (meme regle que la barre de recherche des menus).
bool catalog_name_matches(const char *filename, const char *query);

// Acces par numero DU CATALOGUE (0..catalog_total()-1, ordre alphabetique ;
// numeros valables jusqu'a la prochaine relecture) - liste complete de la
// manette du telephone.
const CatalogEntry *catalog_entry(int ci);          // NULL hors du catalogue
int catalog_view_global(int i);                     // numero du catalogue du jeu i de la page (-1 : aucun)
int catalog_view_position_of(int ci);               // rang parmi les jeux filtres (-1 : filtre par la recherche)
// Jeu de rang pos parmi les jeux filtres (0..catalog_view_matching()-1), NULL
// hors limites.
const CatalogEntry *catalog_view_at(int pos);

#endif
