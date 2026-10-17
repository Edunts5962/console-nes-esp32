// touch_controls.h - le tactile FT6336 comme manette : deux bandes de 32 px
// a gauche (croix) et a droite (A, B, START, SELECT) de l'image du jeu, et
// etalonnage a 3 points.
#ifndef TOUCH_CONTROLS_H
#define TOUCH_CONTROLS_H

#include <stdint.h>

// A appeler UNE FOIS dans setup(), apres audio_codec_init() (le bus I2C
// partage doit deja exister) et apres tft.init() (l'etalonnage dessine
// directement a l'ecran). Tente de recharger un etalonnage existant.
// S'il n'y en a pas :
//  - menus LVGL (USE_LVGL_UI = 1) : rien n'est dessine ici,
//    g_touch_calibration_needed passe a true et l'ecran d'etalonnage LVGL
//    s'ouvre au debut des menus (ui_lvgl_run_menu()) ;
//  - interface sans LVGL (USE_LVGL_UI = 0) : l'etalonnage a 3 points de ce
//    fichier est lance ici (bloquant).
void touch_controls_init_and_calibrate();
// Point touche lors de la derniere lecture (coordonnees de l'ecran 320 x
// 240) ; faux si aucun doigt.
bool touch_controls_point(int *x, int *y);

// Etat du tactile apres touch_controls_init_and_calibrate(), lu par
// ui_lvgl.cpp.
//  - g_touch_available : le FT6336 a repondu a l'initialisation.
//  - g_touch_calibration_needed : aucun etalonnage valide n'est enregistre
//    en NVS ; avec les menus LVGL, l'etalonnage du premier demarrage est
//    alors fait par l'ecran d'etalonnage de ui_lvgl.cpp.
extern bool g_touch_available;
extern bool g_touch_calibration_needed;

// Dessine, UNE SEULE FOIS, les etiquettes de boutons dans les deux
// bandes de 32 px de large qui restent libres a gauche et a droite de
// l'image du jeu (256 px de large, centree dans un ecran de 320 px).
// Ces bandes ne sont JAMAIS ecrites par le rendu du jeu lui-meme (voir
// vid_flush() dans osd.cpp) : ce qui est dessine ici reste donc visible
// tout au long d'une partie, sans avoir besoin d'etre redessine a chaque
// image. A appeler juste avant de lancer nofrendo_main().
void touch_controls_draw_gameplay_overlay();

// Dessinent les bandes tactiles de 32 px des ecrans menu / reglages (memes
// zones qu'en jeu, etiquettes adaptees), pour ne pas toucher "a l'aveugle".
// A appeler a chaque redessin complet de l'ecran correspondant (osd.cpp
// limite le contenu des menus a la bande centrale TOUCH_CONTENT_X0..X1).
void touch_controls_draw_menu_overlay();
void touch_controls_draw_settings_overlay();

// Ecran "PORTAIL WIFI" (run_wifi_portal_screen(), osd.cpp).
void touch_controls_draw_portal_overlay();

// LEFT/RIGHT (bande gauche) basculent "Sortie" (ECRAN LCD/TELEVISEUR), B
// (bande droite) bascule "Norme" (NTSC/PAL), A ferme l'ecran.
void touch_controls_draw_video_output_overlay();

// Bande centrale (entre les deux colonnes de boutons tactiles) que tout
// contenu dessine par-dessus (liste de jeux, texte, barres...) doit
// respecter pour ne pas recouvrir les etiquettes ci-dessus. Memes valeurs
// que la zone jamais touchee par le rendu du jeu (256 px centres dans un
// ecran de 320 px) - voir le commentaire plus haut. En dur (32 / 288) et
// non derive de DISPLAY_WIDTH : ce fichier est inclus par controller.cpp,
// qui n'inclut pas tft_driver.h (DISPLAY_WIDTH n'y serait pas defini) -
// et de toute facon l'ecran de cette carte est fixe a 320x240.
#define TOUCH_CONTENT_X0 32
#define TOUCH_CONTENT_X1 288

// Equivalent tactile de nes_get_gamepad_state() (meme bitmask de retour :
// NES_A=0x01, NES_B=0x02, NES_SELECT=0x04, NES_START=0x08, NES_UP=0x10,
// NES_DOWN=0x20, NES_LEFT=0x40, NES_RIGHT=0x80). Lit le point de contact
// courant (etalonne) et le traduit en bouton selon les zones - fonctionne
// aussi bien dans le menu que pendant une partie (memes zones dans les deux
// cas ; les deux contextes affichent aussi les etiquettes correspondantes,
// voir plus haut). Puce tactile a un seul point de contact a la fois : un seul
// bouton "tactile" possible a la fois (pas de diagonale + bouton d'action
// simultanes via le tactile).
int touch_controls_get_gamepad_state();

#endif
