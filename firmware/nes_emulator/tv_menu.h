// tv_menu.h - liste des jeux affichee sur la television.
//
// La console dessine elle-meme l'ecran (256 x 240, 1 octet par pixel RGB332,
// le format de la liaison video) et l'envoie a la carte video comme une
// image de jeu : la carte video n'a besoin d'aucune mise a jour. Textes dans
// les polices de l'interface LVGL (ui_font_12/14/20, avec accents), lues
// directement - LVGL n'a pas besoin de tourner (utilisable en partie).
//
// Deux usages :
//  - menus de la console (ui_lvgl.cpp) : la liste des jeux de l'ecran LCD,
//    avec le meme curseur, recopiee sur la television ;
//  - en partie (osd.cpp) : SELECT + START tenus 1 s mettent le jeu en pause
//    et ouvrent la liste par-dessus l'image du jeu assombrie.
#ifndef TV_MENU_H
#define TV_MENU_H

#include <stdint.h>
#include <stdbool.h>

// Drapeaux d'une ligne
#define TVM_BAD      0x01   // ROM en liste noire ("!")
#define TVM_FLASH    0x02   // jeu en memoire interne
#define TVM_CURRENT  0x04   // jeu en cours (partie en pause)
#define TVM_FAV      0x08   // Favori (etoile)
#define TVM_DIM      0x10   // Ligne grisee (action impossible)

// Nom affiche (UTF-8, sans ".nes") de la ligne row ; *flags : TVM_*.
typedef const char *(*TvMenuRowFn)(int row, uint8_t *flags);
// Valeur affichee a droite de la ligne (ex. "< 2 >", "35 %"), ou NULL.
typedef const char *(*TvMenuValueFn)(int row);

struct TvMenuView {
  const char *title;      // "Jeux"
  const char *badge;      // a droite du titre ("12 jeux"), ou NULL
  const char *subtitle;   // sous le titre, ou NULL
  int count;              // nombre de lignes
  TvMenuRowFn row;
  int cursor;             // ligne sous le curseur, -1 : aucune
  const char *hint;       // bas de l'ecran (touches), ou NULL
  const char *message;    // remplace l'indication (refus, chargement...), ou NULL
  bool message_error;
  TvMenuValueFn value;    // Valeurs a droite (menu pause), ou NULL
};

// Lignes visibles a la fois.
#define TVM_VISIBLE_ROWS 7   // 8 avant (bas de l'ecran coupe par certains televiseurs)

// Dessine la liste. bg_lines/bg_lut : image du jeu (indices de palette et
// table indice -> RGB332) affichee assombrie derriere la liste ; NULL : fond
// uni. Faux si la memoire de l'image (60 Ko de PSRAM) manque.
bool tv_menu_render(const TvMenuView &v, uint8_t *const *bg_lines, const uint8_t *bg_lut);

// Ecran d'information (deux lignes centrees), meme fond que ci-dessus.
bool tv_menu_render_notice(const char *line1, const char *line2, uint8_t *const *bg_lines,
                           const uint8_t *bg_lut);

// Envoie la derniere image dessinee a la carte video. Faux si la liaison
// etait encore occupee par l'image precedente (a redemander un peu plus
// tard).
bool tv_menu_submit();

// La liste repart du haut (nouvelle liste, nouvelle ouverture).
void tv_menu_reset_scroll();

// Lignes de la derniere image dessinee (RGB332, 256 x 240), pour l'afficher
// aussi sur l'ecran LCD (menu pause) ; NULL si aucune.
uint8_t *const *tv_menu_lines();
// Ligne de la liste sous le point (x, y) de l'image 256 x 240 (doigt sur
// l'ecran LCD), -1 : aucune. Selon le dernier dessin.
int tv_menu_row_at(int x, int y);

// Symboles des polices de l'interface (FontAwesome, UTF-8)
#define TVM_SYM_STAR  "\xEF\x80\x85"   // U+F005
#define TVM_SYM_CLOCK "\xEF\x80\x97"   // U+F017
#define TVM_SYM_LEFT  "\xEF\x81\x93"   // U+F053 (LV_SYMBOL_LEFT)
#define TVM_SYM_RIGHT "\xEF\x81\x94"   // U+F054 (LV_SYMBOL_RIGHT)

#endif
