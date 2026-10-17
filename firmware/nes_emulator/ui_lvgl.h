// ui_lvgl.h - menus de la console avec LVGL 8.3 (liste des jeux, Reglages,
// Video, Portail). Voir ui_lvgl.cpp.
#ifndef UI_LVGL_H
#define UI_LVGL_H

// Applique la luminosite enregistree (NVS "nesui"/"bl") au retroeclairage
// GPIO45. A appeler tot (avant le logo), apres le ledcAttachChannel() du .ino.
void ui_lvgl_apply_saved_brightness();

// Lance l'interface LVGL (initialise LVGL au premier appel), relit la
// carte SD et ne rend la main que lorsqu'un jeu a ete choisi : renvoie son
// index dans la liste (>= 0). Sert le portail web pendant tout ce temps.
int ui_lvgl_run_menu();

#endif
