// ui_lvgl.cpp - menus de la console avec LVGL 8.3.
//
// Onglets Jeux / Reglages / Video / Portail : liste des jeux (pages de 50,
// recherche au clavier a l'ecran, favoris, recents, fiche d'un jeu par
// appui long : jouer, supprimer), themes de couleurs, luminosite, volume,
// mode Wi-Fi (Auto / Box / Point d'acces), sortie Televiseur et norme video,
// QR codes du portail, etalonnage et test du tactile, aide "Commandes".
//
// Principe :
//  - LVGL ne tourne QUE pendant les menus (setup() -> show_menu() ->
//    ui_lvgl_run_menu()). Des qu'un jeu est choisi, lv_timer_handler() n'est
//    plus appele : l'emulation garde tout le processeur.
//  - Ecran : objet TFT_eSPI my_lcd du .ino. Tactile : ft6336_touch_read()
//    du projet (le bus I2C est deja pilote par le pilote i2c_master du
//    codec, pas de bibliotheque basee sur Wire).
//  - Manettes : la manette filaire et celle du telephone pilotent aussi les
//    menus (groupe LVGL "clavier").
//  - Polices : ui_font_12/14/20.c fournies dans le croquis (Montserrat avec
//    accents francais), independantes des LV_FONT_MONTSERRAT_xx de
//    lv_conf.h.
//  - Compatible avec LV_TICK_CUSTOM 0 ou 1, LV_COLOR_16_SWAP 0 ou 1,
//    LV_USE_THEME_DEFAULT 0 ou 1 (lv_conf.h). Le QR code est genere par
//    nes_qrcodegen.c : le reglage LV_USE_QRCODE n'a aucune importance.
#include "hw_config.h"

#if USE_LVGL_UI

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <lvgl.h>
#include <TFT_eSPI.h>
#include <Preferences.h>
#include "tft_driver.h"      // DISPLAY_WIDTH / DISPLAY_HEIGHT, my_lcd
#include "ft6336_touch.h"
#include "osd_menu_api.h"
#include "video_link.h"
#include "tv_menu.h"         // Liste des jeux sur la television
#include "wifi_boot.h"
#include "wifi_portal.h"
// WIFI_AP_SSID / WIFI_AP_PASSWORD (QR "rejoindre le reseau") ; modele utilise
// si wifi_config.h n'existe pas (voir wifi_config.example.h)
#if __has_include("wifi_config.h")
#include "wifi_config.h"
#else
#include "wifi_config.example.h"
#endif
#include "touch_controls.h"   // g_touch_available, g_touch_calibration_needed
#include "nes_qrcodegen.h"    // generateur de QR code integre
#include "bluetooth_controller.h"   // Bouton MENU de la manette du telephone
#include "games_link.h"             // Liste des jeux et lancement depuis le telephone
#include "ui_lvgl.h"
#include <ctype.h>

#if LVGL_VERSION_MAJOR != 8
#error "ui_lvgl.cpp est ecrit pour LVGL 8.x (8.3.11 conseillee) - LVGL 9.x n'est pas pris en charge"
#endif
#if LV_COLOR_DEPTH != 16
#error "lv_conf.h : LV_COLOR_DEPTH doit valoir 16 (ecran ILI9341 en RGB565)"
#endif

extern "C" int controller_read_physical_buttons();   // controller.cpp

LV_FONT_DECLARE(ui_font_12);
LV_FONT_DECLARE(ui_font_14);
LV_FONT_DECLARE(ui_font_20);

// ============================================================================
// Themes de couleurs - choisis dans Reglages > THEME, memorises en NVS
// ("nesui"/"theme") et appliques tout de suite (l'interface est reconstruite,
// voir ui_apply_theme()). Contrastes verifies apres conversion en RGB565
// (ecran ILI9341) : texte principal >= 14:1, texte secondaire >= 5,7:1, texte
// sur l'accent >= 4,6:1.
// ============================================================================
struct UiTheme {
  const char *name;
  bool dark;         // theme LVGL sombre ou clair (curseurs, interrupteur)
  uint32_t bg;       // fond
  uint32_t surf;     // cartes, barres
  uint32_t surf2;    // appui, touches
  uint32_t line;     // filets, pistes des curseurs
  uint32_t txt;      // texte principal
  uint32_t mut;      // texte secondaire
  uint32_t dim;      // desactive
  uint32_t acc;      // accent (selection, boutons principaux)
  uint32_t acc_txt;  // accent pour du TEXTE sur fond sombre/clair (plus lisible)
  uint32_t acc_d;    // accent attenue (ligne appuyee, onglet actif)
  uint32_t acc_ink;  // texte sur l'accent
  uint32_t grad;     // fin du degrade des boutons principaux
  uint32_t acc2;     // accent secondaire
  uint32_t warm;     // attention (liste noire, connexion)
  uint32_t cyan;     // point d'acces
  uint32_t red;      // erreur, suppression
  uint32_t flash;    // jeu en memoire interne
  uint32_t sec;      // intitules de section
  uint32_t knob;     // bouton des curseurs et de l'interrupteur
};

static const UiTheme s_themes[] = {
  // name, dark, bg, surf, surf2, line, txt, mut, dim, acc, acc_txt, acc_d, acc_ink, grad, acc2, warm, cyan, red, flash, sec, knob
  {"Menthe", true, 0x0F1419, 0x18212A, 0x222C36, 0x2A3642, 0xE8EEF2, 0x8C9BA8, 0x4A5560, 0x3CC08E, 0x3CC08E, 0x1E5E47, 0x04140D, 0x1FA5A0, 0x4FC3F7, 0xF2B34A, 0x4FC3F7, 0xE4574C, 0xB18CFF, 0x8C9BA8, 0xE8EEF2},
  {"Galaxie", true, 0x0B0A1A, 0x17152E, 0x232046, 0x302B5C, 0xF1EEFF, 0xA29CCB, 0x514B85, 0x8052F5, 0xA78BFA, 0x2A1E5E, 0xFFFFFF, 0xC026D3, 0x22D3EE, 0xFBBF24, 0x22D3EE, 0xFF5A4E, 0xF472B6, 0x22D3EE, 0xF1EEFF},
  {"Oc\xC3\xA9" "an", true, 0x061A24, 0x0C2733, 0x143646, 0x1D4658, 0xE8F8FC, 0x8DB4C4, 0x3E6272, 0x2DD4BF, 0x2DD4BF, 0x0F5750, 0x00231F, 0x38BDF8, 0x60A5FA, 0xFCD34D, 0x60A5FA, 0xFB7185, 0xC4B5FD, 0x60A5FA, 0xE8F8FC},
  {"Arcade", true, 0x0C1020, 0x161C36, 0x202849, 0x2C365E, 0xF2F5FF, 0x9AA6CC, 0x4B5580, 0xFFCC1A, 0xFFCC1A, 0x5A4708, 0x1A1400, 0xFF9416, 0x4D8DFF, 0xFF8A3D, 0x38D6FF, 0xFF4D5E, 0xC08CFF, 0x5C9BFF, 0xF2F5FF},
  {"Volcan", true, 0x160C0E, 0x26151A, 0x341F25, 0x462A31, 0xFFF1EC, 0xC69E98, 0x6E4B4E, 0xFF7A2F, 0xFF8A45, 0x6A2D0E, 0x230B00, 0xFF4F6B, 0xFFC53D, 0xFFE066, 0x4FC3F7, 0xFF4F6B, 0xC29BFF, 0xFFC53D, 0xFFF1EC},
  {"Clair", false, 0xEEF1F7, 0xFFFFFF, 0xE2E8F2, 0xD0D8E6, 0x18202E, 0x5B667A, 0xA3ADBF, 0x5B5BF0, 0x4F46E5, 0xDCDDFE, 0xFFFFFF, 0x7C3AED, 0xF97316, 0xB45309, 0x0284C7, 0xDC2626, 0x9333EA, 0xC2410C, 0x5B5BF0},
};
#define THEME_COUNT   ((int)(sizeof(s_themes) / sizeof(s_themes[0])))
#define THEME_DEFAULT 1   // Galaxie
static int s_theme_id = THEME_DEFAULT;
static const UiTheme *s_th = &s_themes[THEME_DEFAULT];

#define COL_BG      (s_th->bg)
#define COL_SURF    (s_th->surf)
#define COL_SURF2   (s_th->surf2)
#define COL_LINE    (s_th->line)
#define COL_TXT     (s_th->txt)
#define COL_MUT     (s_th->mut)
#define COL_DIM     (s_th->dim)
#define COL_ACC     (s_th->acc)
#define COL_ACC_TXT (s_th->acc_txt)
#define COL_ACC_D   (s_th->acc_d)
#define COL_ACC_INK (s_th->acc_ink)
#define COL_GRAD    (s_th->grad)
#define COL_ACC2    (s_th->acc2)
#define COL_WARM    (s_th->warm)
#define COL_CYAN    (s_th->cyan)
#define COL_RED     (s_th->red)
#define COL_FLASH   (s_th->flash)
#define COL_SEC     (s_th->sec)
#define COL_KNOB    (s_th->knob)

static inline lv_color_t C(uint32_t hex) { return lv_color_hex(hex); }

// Masques de boutons (memes valeurs que controller.cpp / osd.cpp)
#define NES_A      0x01
#define NES_B      0x02
#define NES_SELECT 0x04
#define NES_START  0x08
#define NES_UP     0x10
#define NES_DOWN   0x20
#define NES_LEFT   0x40
#define NES_RIGHT  0x80

// Geometrie
#define TOPBAR_H   28
#define NAV_H      38
#define CONTENT_Y  TOPBAR_H
#define CONTENT_H  (DISPLAY_HEIGHT - TOPBAR_H - NAV_H)   // 174 px

enum { TAB_GAMES = 0, TAB_SETTINGS, TAB_VIDEO, TAB_PORTAL, TAB_COUNT };

// ============================================================================
// Luminosite (nouveau reglage, NVS "nesui"/"bl", en %)
// ============================================================================
#define UI_NVS_NAMESPACE "nesui"
#define BL_MIN_PCT 10
#define BL_DEFAULT_PCT 80          // = 204/255, niveau de depart

static int s_bl_pct = BL_DEFAULT_PCT;
static int s_bl_saved = -1;

static void bl_apply(int pct) {
  if (pct < BL_MIN_PCT) pct = BL_MIN_PCT;
  if (pct > 100) pct = 100;
  s_bl_pct = pct;
  ledcWrite(LCD_BL_PIN, (uint32_t)(pct * 255 / 100));
}

void ui_lvgl_apply_saved_brightness() {
  Preferences p;
  if (p.begin(UI_NVS_NAMESPACE, true)) {         // lecture seule
    s_bl_saved = p.getUChar("bl", BL_DEFAULT_PCT);
    p.end();
  } else {
    s_bl_saved = BL_DEFAULT_PCT;                 // espace pas encore cree
  }
  bl_apply(s_bl_saved);
  Serial.printf("[UI] Luminosite : %d %%\n", s_bl_pct);
}

static void bl_commit() {
  if (s_bl_pct == s_bl_saved) return;
  Preferences p;
  if (p.begin(UI_NVS_NAMESPACE, false)) {
    p.putUChar("bl", (uint8_t)s_bl_pct);
    p.end();
    s_bl_saved = s_bl_pct;
    Serial.printf("[UI] Luminosite enregistree : %d %%\n", s_bl_pct);
  }
}

// Theme de couleurs (NVS "nesui"/"theme", numero dans s_themes[]).
static void theme_select(int id) {
  if (id < 0 || id >= THEME_COUNT) id = THEME_DEFAULT;
  s_theme_id = id;
  s_th = &s_themes[id];
}

static void theme_load() {
  int id = THEME_DEFAULT;
  Preferences p;
  if (p.begin(UI_NVS_NAMESPACE, true)) {
    id = p.getUChar("theme", THEME_DEFAULT);
    p.end();
  }
  theme_select(id);
  Serial.printf("[UI] Theme : %s\n", s_th->name);
}

static void theme_save() {
  Preferences p;
  if (p.begin(UI_NVS_NAMESPACE, false)) {
    p.putUChar("theme", (uint8_t)s_theme_id);
    p.end();
  }
}

// ============================================================================
// Pilotes LVGL : ecran, tactile, manette
// ============================================================================
static lv_disp_draw_buf_t s_draw_buf;
// Tampon de dessin : 320 x 24 pixels = 15 360 octets de RAM interne,
// alloue au lancement des menus et RENDU avant le jeu (voir la fin de
// ui_lvgl_run_menu()) - l'emulateur et le Wi-Fi recuperent cette memoire.
#define DRAW_BUF_PIXELS (DISPLAY_WIDTH * 24)
static lv_color_t *s_buf1 = NULL;
static lv_disp_t *s_disp = NULL;
static lv_indev_t *s_touch_indev = NULL;
static lv_indev_t *s_keypad_indev = NULL;

static void disp_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = (uint32_t)(area->x2 - area->x1 + 1);
  uint32_t h = (uint32_t)(area->y2 - area->y1 + 1);
  my_lcd.setAddrWindow(area->x1, area->y1, w, h);
#if LV_COLOR_16_SWAP
  // Pixels deja dans l'ordre d'octets de l'ecran : ne rien inverser.
  bool prev = my_lcd.getSwapBytes();
  my_lcd.setSwapBytes(false);
  my_lcd.pushColors((uint16_t *)&color_p->full, w * h, false);
  my_lcd.setSwapBytes(prev);
#else
  // inversion des octets faite par TFT_eSPI (swap = true)
  my_lcd.pushColors((uint16_t *)&color_p->full, w * h, true);
#endif
  lv_disp_flush_ready(drv);
}

// Vrai apres un ecran plein format (etalonnage, test) : le doigt encore
// pose au retour est ignore jusqu'a ce qu'il soit releve, sinon son
// relachement "cliquerait" sur ce qui se trouve dessous (un jeu, un bouton).
static bool s_touch_wait_release = false;

static void touch_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  (void)drv;
  int x, y;
  bool down = ft6336_touch_read(&x, &y);
  if (s_touch_wait_release) {
    if (down) { data->state = LV_INDEV_STATE_REL; return; }
    s_touch_wait_release = false;
  }
  if (down) {                            // coordonnees ecran deja etalonnees
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= DISPLAY_WIDTH) x = DISPLAY_WIDTH - 1;
    if (y >= DISPLAY_HEIGHT) y = DISPLAY_HEIGHT - 1;
    data->state = LV_INDEV_STATE_PR;
    data->point.x = (lv_coord_t)x;
    data->point.y = (lv_coord_t)y;
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

// Vrai pendant l'ecran de recherche - HAUT/BAS de la manette deplacent alors
// la selection dans le clavier (au lieu de passer d'un element a l'autre).
static bool s_keypad_arrows = false;
// Fiche d'un jeu ouverte - GAUCHE/DROITE passent d'un bouton a l'autre
// (boutons cote a cote).
static bool s_keypad_lr_nav = false;

// Demandes posees par la manette et traitees dans la boucle principale
static volatile bool s_req_next_tab = false;
static volatile bool s_req_tab_games = false;
static volatile bool s_req_tab_video = false;

#if HAS_PHYSICAL_CONTROLLER
// Manette 74HC165 = peripherique "clavier" de LVGL :
//   HAUT/BAS -> element precedent/suivant, GAUCHE/DROITE -> curseurs et
//   boutons segmentes, A -> valider ; B -> onglet Jeux, SELECT -> onglet
//   suivant, START -> onglet Video (comme START ouvrait "Sortie video").
static int s_kp_prev = 0;   // etat precedent des boutons (fronts montants)

static void keypad_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  (void)drv;
  static uint32_t last_key = LV_KEY_ENTER;
  static bool last_pr = false;
  int s = controller_read_physical_buttons();
  int pressed_now = s & ~s_kp_prev;        // fronts montants
  s_kp_prev = s;
  if (pressed_now & NES_SELECT) s_req_next_tab = true;
  if (pressed_now & NES_B) s_req_tab_games = true;
  if (pressed_now & NES_START) s_req_tab_video = true;

  uint32_t key = 0;
  if (s & NES_UP) key = s_keypad_arrows ? LV_KEY_UP : LV_KEY_PREV;
  else if (s & NES_DOWN) key = s_keypad_arrows ? LV_KEY_DOWN : LV_KEY_NEXT;
  else if (s & NES_LEFT) key = s_keypad_lr_nav ? LV_KEY_PREV : LV_KEY_LEFT;
  else if (s & NES_RIGHT) key = s_keypad_lr_nav ? LV_KEY_NEXT : LV_KEY_RIGHT;
  else if (s & NES_A) key = LV_KEY_ENTER;

  if (key) {
    // (Pas pendant une attente du relachement - lv_indev_wait_release() : un
    // faux relachement la leverait et une fleche appuyee pendant que A est
    // encore tenu deviendrait active.)
    if (last_pr && key != last_key && !(s_keypad_indev && s_keypad_indev->proc.wait_until_release)) {
      data->key = last_key;
      data->state = LV_INDEV_STATE_REL;
      data->continue_reading = true;
      last_pr = false;
      return;
    }
    last_key = key;
    data->key = key;
    data->state = LV_INDEV_STATE_PR;
    last_pr = true;
  } else {
    data->key = last_key;
    data->state = LV_INDEV_STATE_REL;
    last_pr = false;
  }
}
#endif

static bool s_lvgl_ready = false;

// Horloge LVGL (si LV_TICK_CUSTOM vaut 0) et "pompe" utilisee par les
// ecrans bloquants (etalonnage, test du tactile) : LVGL continue de
// dessiner et le portail web de repondre pendant qu'ils attendent.
static uint32_t s_tick_last = 0;

static void ui_tick() {
#if LV_TICK_CUSTOM == 0
  uint32_t now = millis();
  lv_tick_inc(now - s_tick_last);
  s_tick_last = now;
#endif
}

static void ui_pump() {
  ui_tick();
  lv_timer_handler();
#if ENABLE_WIFI_PORTAL
  wifi_portal_loop();
#endif
  delay(5);
}

static void ui_pump_for(uint32_t ms) {
  uint32_t t0 = millis();
  while (millis() - t0 < ms) ui_pump();
}

static bool ensure_draw_buffer() {
  if (s_buf1) return true;
  s_buf1 = (lv_color_t *)heap_caps_malloc(DRAW_BUF_PIXELS * sizeof(lv_color_t),
                                          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!s_buf1) {
    // Repli (lent mais fonctionnel) : PSRAM
    s_buf1 = (lv_color_t *)heap_caps_malloc(DRAW_BUF_PIXELS * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  }
  if (!s_buf1) return false;
  lv_disp_draw_buf_init(&s_draw_buf, s_buf1, NULL, DRAW_BUF_PIXELS);
  return true;
}

// Theme LVGL (curseurs, interrupteur, sablier) aux couleurs du theme choisi.
// Peut etre rappele (changement de theme, voir ui_apply_theme()).
static void theme_lvgl_apply() {
#if LV_USE_THEME_DEFAULT
  lv_theme_t *th = lv_theme_default_init(s_disp, C(COL_ACC), C(COL_ACC2), s_th->dark, &ui_font_14);
  lv_disp_set_theme(s_disp, th);
#endif
}

static void styles_init();

static void lvgl_init_once() {
  if (s_lvgl_ready) {
    ensure_draw_buffer();
    return;
  }
  theme_load();   // Avant toute couleur
  lv_init();
  s_tick_last = millis();
  if (!ensure_draw_buffer()) {
    Serial.println("[UI] ERREUR : pas de memoire pour le tampon de dessin LVGL");
  }

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = DISPLAY_WIDTH;
  disp_drv.ver_res = DISPLAY_HEIGHT;
  disp_drv.flush_cb = disp_flush;
  disp_drv.draw_buf = &s_draw_buf;
  s_disp = lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t touch_drv;
  lv_indev_drv_init(&touch_drv);
  touch_drv.type = LV_INDEV_TYPE_POINTER;
  touch_drv.read_cb = touch_read;
  s_touch_indev = lv_indev_drv_register(&touch_drv);

#if HAS_PHYSICAL_CONTROLLER
  static lv_indev_drv_t kp_drv;
  lv_indev_drv_init(&kp_drv);
  kp_drv.type = LV_INDEV_TYPE_KEYPAD;
  kp_drv.read_cb = keypad_read;
  s_keypad_indev = lv_indev_drv_register(&kp_drv);
#endif

  // Theme LVGL aux couleurs du theme choisi, police avec accents : sert de
  // base aux curseurs, interrupteur, boutons segmentes (le reste est style a
  // la main). Styles partages crees ici : l'ecran "Chargement..." les utilise
  // deja.
  theme_lvgl_apply();
  styles_init();

  s_lvgl_ready = true;
  Serial.println("[UI] LVGL 8 initialise (ecran 320x240, tactile FT6336"
#if HAS_PHYSICAL_CONTROLLER && (PAD_PROTOCOL == PAD_PROTOCOL_NES)
                 ", manette NES (CD4021)"
#elif HAS_PHYSICAL_CONTROLLER
                 ", manette 74HC165"
#endif
                 ")");
}

// ============================================================================
// Styles partages (un seul exemplaire en memoire, quel que soit le nombre
// de jeux affiches)
// ============================================================================
static lv_style_t st_plain;       // conteneur transparent sans marge
static lv_style_t st_card;        // carte (fond SURF, arrondi)
static lv_style_t st_section;     // intitule de section (MAJUSCULES)
static lv_style_t st_kb_key;      // Touche du clavier de recherche
static lv_style_t st_row;         // ligne de jeu
static lv_style_t st_row_pr;      // ligne appuyee
static lv_style_t st_row_focus;   // ligne selectionnee a la manette
static lv_style_t st_row_name;    // nom du jeu (1 ligne, "..." si trop long)
static lv_style_t st_row_name_bad;
static lv_style_t st_row_flash;     // Ligne d'un jeu en memoire interne (filet colore a gauche)
static lv_style_t st_row_sel;       // Jeu choisi par SELECT (manette du telephone)
static lv_style_t st_nav;         // bouton de la barre de navigation
static lv_style_t st_nav_on;      // onglet actif
static lv_style_t st_nav_pr;      // onglet appuye
static lv_style_t st_scrollbar;   // barre de defilement fine
static lv_style_t st_pill;        // pastille d'etat
static lv_style_t st_bm_main;     // boutons segmentes : cadre
static lv_style_t st_bm_item;     // boutons segmentes : bouton
static lv_style_t st_bm_on;       // boutons segmentes : bouton choisi
static lv_style_t st_bm_pr;       // boutons segmentes : appui
static lv_style_t st_bm_off;      // boutons segmentes : bouton desactive
static lv_style_t st_focus_key;   // selection a la manette (contour accent)
static lv_style_t st_btn_main;    // Bouton principal (degrade de l'accent)

// Toutes les styles partages, pour les reinitialiser au changement de theme
// (voir ui_apply_theme()).
static lv_style_t *const s_all_styles[] = {
  &st_plain, &st_card, &st_section, &st_kb_key, &st_row, &st_row_pr, &st_row_focus,
  &st_row_name, &st_row_name_bad, &st_row_flash, &st_row_sel,
  &st_nav, &st_nav_on, &st_nav_pr, &st_scrollbar, &st_pill,
  &st_bm_main, &st_bm_item, &st_bm_on, &st_bm_pr, &st_bm_off, &st_focus_key, &st_btn_main,
};

// Degrade horizontal : de la couleur de fond vers "to".
static void style_hgrad(lv_style_t *st, uint32_t to) {
  lv_style_set_bg_grad_color(st, C(to));
  lv_style_set_bg_grad_dir(st, LV_GRAD_DIR_HOR);
}

// Cree (ou recree, apres un changement de theme) les styles partages avec
// les couleurs du theme courant. Aucun objet ne doit encore les utiliser
// quand on les recree (ui_apply_theme() detruit l'interface avant).
static void styles_init() {
  static bool inited = false;
  if (inited) {
    for (size_t i = 0; i < sizeof(s_all_styles) / sizeof(s_all_styles[0]); i++) lv_style_reset(s_all_styles[i]);
  }
  inited = true;

  lv_style_init(&st_plain);
  lv_style_set_bg_opa(&st_plain, LV_OPA_TRANSP);
  lv_style_set_border_width(&st_plain, 0);
  lv_style_set_pad_all(&st_plain, 0);
  lv_style_set_radius(&st_plain, 0);

  // Cartes plus arrondies, fin filet pour les detacher du fond
  lv_style_init(&st_card);
  lv_style_set_bg_color(&st_card, C(COL_SURF));
  lv_style_set_bg_opa(&st_card, LV_OPA_COVER);
  lv_style_set_radius(&st_card, 12);
  lv_style_set_pad_all(&st_card, 10);
  lv_style_set_pad_row(&st_card, 10);
  lv_style_set_border_width(&st_card, 1);
  lv_style_set_border_color(&st_card, C(COL_LINE));

  // Intitules de section dans la couleur du theme
  lv_style_init(&st_section);
  lv_style_set_text_color(&st_section, C(COL_SEC));
  lv_style_set_text_font(&st_section, &ui_font_12);
  lv_style_set_text_letter_space(&st_section, 1);
  lv_style_set_pad_top(&st_section, 4);
  lv_style_set_pad_left(&st_section, 2);

  lv_style_init(&st_row);
  lv_style_set_bg_color(&st_row, C(COL_SURF));
  lv_style_set_bg_opa(&st_row, LV_OPA_COVER);
  lv_style_set_radius(&st_row, 10);
  // Filet gauche de 3 px sur TOUTES les lignes, invisible (couleur du fond)
  // sauf pour un jeu en memoire interne ou la ligne selectionnee a la manette
  // - les noms restent ainsi alignes.
  lv_style_set_border_width(&st_row, 3);
  lv_style_set_border_side(&st_row, LV_BORDER_SIDE_LEFT);
  lv_style_set_border_color(&st_row, C(COL_SURF));
  lv_style_set_pad_all(&st_row, 0);

  lv_style_set_width(&st_row, LV_PCT(100));
  lv_style_set_height(&st_row, 40);

  // Taille et position des deux labels d'une ligne dans le style partage
  // (et non en style local) : economise ~200 octets de tas LVGL par jeu.
  // Hauteur fixee a UNE ligne : indispensable pour que LV_LABEL_LONG_DOT
  // remplace la fin d'un nom trop long par "..." au lieu de le couper sur
  // deux lignes (LVGL 8 n'ajoute pas de points a un texte d'une ligne dont
  // la hauteur s'adapte au contenu).
  lv_style_init(&st_row_name);
  lv_style_set_text_font(&st_row_name, &ui_font_14);
  lv_style_set_text_color(&st_row_name, C(COL_TXT));
  lv_style_set_width(&st_row_name, DISPLAY_WIDTH - 16 - 24);
  lv_style_set_height(&st_row_name, 18);
  lv_style_set_x(&st_row_name, 9);    // + 3 px de filet = 12 px
  lv_style_set_y(&st_row_name, 4);

  lv_style_init(&st_row_name_bad);
  lv_style_set_text_color(&st_row_name_bad, C(COL_WARM));

  // Jeu en memoire interne. Style PARTAGE (quelques octets par ligne
  // seulement, aucun objet en plus) : filet colore a gauche ; le sous-titre
  // prend la meme couleur (games_draw_subtitles). Le filet de la selection
  // manette (st_row_focus, etat FOCUS_KEY) reste prioritaire.
  lv_style_init(&st_row_flash);
  lv_style_set_border_color(&st_row_flash, C(COL_FLASH));

  // Ligne appuyee / selectionnee = degrade accent -> carte
  lv_style_init(&st_row_pr);
  lv_style_set_bg_color(&st_row_pr, C(COL_ACC_D));
  style_hgrad(&st_row_pr, COL_SURF);
  lv_style_set_border_color(&st_row_pr, C(COL_ACC_D));

  lv_style_init(&st_row_focus);
  lv_style_set_bg_color(&st_row_focus, C(COL_ACC_D));
  style_hgrad(&st_row_focus, COL_SURF);
  lv_style_set_border_color(&st_row_focus, C(COL_ACC));
  lv_style_set_border_width(&st_row_focus, 3);
  lv_style_set_border_side(&st_row_focus, LV_BORDER_SIDE_LEFT);

  // Jeu choisi par SELECT (manette du telephone) - contour accent autour de la
  // ligne. Propriete qu'aucun autre style de ligne n'utilise : reste visible
  // avec ou sans le curseur. Pose sur la seule ligne choisie
  // (lv_obj_add_style), donc aucun octet de plus sur les autres lignes.
  lv_style_init(&st_row_sel);
  lv_style_set_outline_color(&st_row_sel, C(COL_ACC_TXT));
  lv_style_set_outline_width(&st_row_sel, 2);
  lv_style_set_outline_pad(&st_row_sel, 1);

  // Onglets en "pastille" (fond colore arrondi derriere l'onglet actif), sur
  // la barre du bas (fond SURF, voir build_ui()).
  lv_style_init(&st_nav);
  lv_style_set_bg_color(&st_nav, C(COL_SURF));
  lv_style_set_bg_opa(&st_nav, LV_OPA_TRANSP);
  lv_style_set_radius(&st_nav, 12);
  lv_style_set_text_color(&st_nav, C(COL_MUT));
  lv_style_set_text_font(&st_nav, &ui_font_12);
  lv_style_set_border_width(&st_nav, 0);
  lv_style_set_pad_all(&st_nav, 0);

  lv_style_init(&st_nav_on);
  lv_style_set_bg_color(&st_nav_on, C(COL_ACC_D));
  lv_style_set_bg_opa(&st_nav_on, LV_OPA_COVER);
  lv_style_set_text_color(&st_nav_on, C(COL_ACC_TXT));

  lv_style_init(&st_nav_pr);
  lv_style_set_bg_color(&st_nav_pr, C(COL_SURF2));
  lv_style_set_bg_opa(&st_nav_pr, LV_OPA_COVER);

  lv_style_init(&st_scrollbar);
  lv_style_set_width(&st_scrollbar, 3);
  lv_style_set_bg_color(&st_scrollbar, C(COL_MUT));
  lv_style_set_bg_opa(&st_scrollbar, LV_OPA_60);
  lv_style_set_radius(&st_scrollbar, 2);
  lv_style_set_pad_right(&st_scrollbar, 1);

  lv_style_init(&st_pill);
  lv_style_set_bg_opa(&st_pill, LV_OPA_COVER);
  lv_style_set_radius(&st_pill, LV_RADIUS_CIRCLE);
  lv_style_set_pad_hor(&st_pill, 8);
  lv_style_set_pad_ver(&st_pill, 2);
  lv_style_set_text_font(&st_pill, &ui_font_12);

  lv_style_init(&st_bm_main);
  lv_style_set_bg_color(&st_bm_main, C(COL_BG));
  lv_style_set_bg_opa(&st_bm_main, LV_OPA_COVER);
  lv_style_set_border_color(&st_bm_main, C(COL_LINE));
  lv_style_set_border_width(&st_bm_main, 1);
  lv_style_set_radius(&st_bm_main, 10);
  lv_style_set_pad_all(&st_bm_main, 3);
  lv_style_set_pad_row(&st_bm_main, 4);
  lv_style_set_pad_column(&st_bm_main, 4);

  lv_style_init(&st_bm_item);
  lv_style_set_bg_color(&st_bm_item, C(COL_BG));
  lv_style_set_bg_opa(&st_bm_item, LV_OPA_COVER);
  lv_style_set_text_color(&st_bm_item, C(COL_MUT));
  lv_style_set_text_font(&st_bm_item, &ui_font_14);
  lv_style_set_radius(&st_bm_item, 8);
  lv_style_set_border_width(&st_bm_item, 0);
  lv_style_set_shadow_width(&st_bm_item, 0);

  lv_style_init(&st_bm_on);
  lv_style_set_bg_color(&st_bm_on, C(COL_ACC));
  style_hgrad(&st_bm_on, COL_GRAD);
  lv_style_set_text_color(&st_bm_on, C(COL_ACC_INK));

  lv_style_init(&st_bm_pr);
  lv_style_set_bg_color(&st_bm_pr, C(COL_ACC_D));

  lv_style_init(&st_focus_key);
  lv_style_set_outline_color(&st_focus_key, C(COL_TXT));   // Visible aussi autour d'un bouton colore
  lv_style_set_outline_width(&st_focus_key, 2);
  lv_style_set_outline_pad(&st_focus_key, 1);

  // Touches du clavier de recherche
  lv_style_init(&st_kb_key);
  lv_style_set_bg_color(&st_kb_key, C(COL_SURF2));
  lv_style_set_bg_opa(&st_kb_key, LV_OPA_COVER);
  lv_style_set_text_color(&st_kb_key, C(COL_TXT));
  lv_style_set_text_font(&st_kb_key, &ui_font_14);
  lv_style_set_radius(&st_kb_key, 6);
  lv_style_set_border_width(&st_kb_key, 0);
  lv_style_set_shadow_width(&st_kb_key, 0);

  lv_style_init(&st_bm_off);
  lv_style_set_bg_color(&st_bm_off, C(COL_BG));
  lv_style_set_text_color(&st_bm_off, C(COL_DIM));

  // Bouton principal (Reetalonner, Envoyer des jeux, Jouer...)
  lv_style_init(&st_btn_main);
  lv_style_set_bg_color(&st_btn_main, C(COL_ACC));
  lv_style_set_bg_opa(&st_btn_main, LV_OPA_COVER);
  style_hgrad(&st_btn_main, COL_GRAD);
  lv_style_set_text_color(&st_btn_main, C(COL_ACC_INK));
}

// ============================================================================
// Petits constructeurs
// ============================================================================
static lv_obj_t *make_plain(lv_obj_t *parent) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_add_style(o, &st_plain, 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

// Styles de texte partages (police + couleur), crees a la demande : chaque
// label y fait reference au lieu d'avoir son propre style local - c'est ce
// qui permet d'afficher 50 jeux dans le tas LVGL par defaut de 48 Ko.
struct TextStyle {
  const lv_font_t *font;
  uint32_t color;
  lv_style_t style;
};
#define TEXT_STYLE_MAX 16
static TextStyle s_text_styles[TEXT_STYLE_MAX];
static int s_text_style_count = 0;

static lv_style_t *text_style(const lv_font_t *font, uint32_t color) {
  for (int i = 0; i < s_text_style_count; i++) {
    if (s_text_styles[i].font == font && s_text_styles[i].color == color) return &s_text_styles[i].style;
  }
  if (s_text_style_count >= TEXT_STYLE_MAX) return NULL;
  TextStyle *t = &s_text_styles[s_text_style_count++];
  t->font = font;
  t->color = color;
  lv_style_init(&t->style);
  lv_style_set_text_font(&t->style, font);
  lv_style_set_text_color(&t->style, C(color));
  return &t->style;
}

// Au changement de theme, une fois l'interface detruite.
static void text_styles_reset() {
  for (int i = 0; i < s_text_style_count; i++) lv_style_reset(&s_text_styles[i].style);
  s_text_style_count = 0;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *txt, const lv_font_t *font, uint32_t color) {
  lv_obj_t *l = lv_label_create(parent);
  lv_style_t *st = text_style(font, color);
  if (st) {
    lv_obj_add_style(l, st, 0);
  } else {
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, C(color), 0);
  }
  lv_label_set_text(l, txt);
  return l;
}

// Colonne defilante qui remplit un onglet
static lv_obj_t *make_scroll_column(lv_obj_t *parent) {
  lv_obj_t *col = lv_obj_create(parent);
  lv_obj_remove_style_all(col);
  lv_obj_add_style(col, &st_plain, 0);
  lv_obj_add_style(col, &st_scrollbar, LV_PART_SCROLLBAR);
  lv_obj_set_size(col, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_pad_hor(col, 8, 0);
  lv_obj_set_style_pad_ver(col, 6, 0);
  lv_obj_set_style_pad_row(col, 6, 0);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(col, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_AUTO);
  return col;
}

static lv_obj_t *make_card(lv_obj_t *parent) {
  lv_obj_t *c = lv_obj_create(parent);
  lv_obj_remove_style_all(c);
  lv_obj_add_style(c, &st_card, 0);
  lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  return c;
}

static void make_section(lv_obj_t *parent, const char *txt) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_add_style(l, &st_section, 0);
  lv_label_set_text(l, txt);
}

// Ligne "libelle a gauche / valeur a droite" dans une carte ; renvoie le
// label de valeur (pour le mettre a jour plus tard).
static lv_obj_t *make_kv_row(lv_obj_t *card, const char *key, const char *val, uint32_t val_color) {
  lv_obj_t *row = make_plain(card);
  lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
  make_label(row, key, &ui_font_14, COL_TXT);
  lv_obj_t *v = make_label(row, val, &ui_font_14, val_color);
  lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
  return v;
}

static lv_obj_t *make_segmented(lv_obj_t *parent, const char **map) {
  lv_obj_t *bm = lv_btnmatrix_create(parent);
  lv_obj_remove_style_all(bm);
  lv_obj_add_style(bm, &st_bm_main, 0);
  lv_obj_add_style(bm, &st_bm_item, LV_PART_ITEMS);
  lv_obj_add_style(bm, &st_bm_pr, (lv_style_selector_t)LV_PART_ITEMS | (lv_style_selector_t)LV_STATE_PRESSED);
  lv_obj_add_style(bm, &st_bm_on, (lv_style_selector_t)LV_PART_ITEMS | (lv_style_selector_t)LV_STATE_CHECKED);
  lv_obj_add_style(bm, &st_bm_off, (lv_style_selector_t)LV_PART_ITEMS | (lv_style_selector_t)LV_STATE_DISABLED);
  lv_obj_add_style(bm, &st_focus_key, (lv_style_selector_t)LV_PART_ITEMS | (lv_style_selector_t)LV_STATE_FOCUS_KEY);
  lv_obj_set_size(bm, LV_PCT(100), 38);
  lv_btnmatrix_set_map(bm, map);
  // CLICK_TRIG : le choix ne change qu'au RELACHEMENT sur le bouton (un
  // doigt qui glisse pour faire defiler l'onglet ne change plus rien) ;
  // pas de SCROLL_CHAIN : glisser sur les boutons ne fait pas defiler.
  lv_btnmatrix_set_btn_ctrl_all(bm, (lv_btnmatrix_ctrl_t)(LV_BTNMATRIX_CTRL_CHECKABLE | LV_BTNMATRIX_CTRL_CLICK_TRIG));
  lv_btnmatrix_set_one_checked(bm, true);
  lv_obj_clear_flag(bm, LV_OBJ_FLAG_SCROLL_CHAIN);
  // Selectionne a la manette sous le bas de l'ecran -> la colonne defile
  // jusqu'a lui (LVGL ne le fait d'office que pour boutons et interrupteurs,
  // pas pour les boutons segmentes ni les curseurs).
  lv_obj_add_flag(bm, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
  return bm;
}

// ============================================================================
// Etat de l'interface
// ============================================================================
static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_title = NULL;
static lv_obj_t *s_wifi_icon = NULL;
static lv_obj_t *s_bt_icon = NULL;     // Manette du telephone (Bluetooth)
static int s_last_bt = -1;             // dernier etat affiche (-1 = a redessiner)
// Onglet Portail en point d'acces - appareils relies au reseau NES-WiFi et
// requetes recues (portal_ap_info_update()). s_ap_info_key : dernier etat
// affiche.
static lv_obj_t *s_ap_info = NULL;
static int s_ap_info_key = -1;
static lv_obj_t *s_wifi_text = NULL;
static lv_obj_t *s_panels[TAB_COUNT];
static lv_obj_t *s_nav_btns[TAB_COUNT];
static lv_group_t *s_groups[TAB_COUNT];
static int s_tab = TAB_GAMES;

static volatile int s_launch_index = -1;
static bool s_launch_from_phone = false;   // Lancement demande par le telephone

// Onglet Jeux
static lv_obj_t *s_games_col = NULL;
// Recherche. La barre de recherche est le PREMIER enfant de s_games_col ; les
// lignes de jeux suivent (jeu n = enfant n + 1).
#define UI_SYMBOL_SEARCH "\xEF\x80\x82"   // loupe (U+F002), ajoutee aux polices ui_font_*
#define UI_SYMBOL_STAR   "\xEF\x80\x85"   // Etoile (U+F005), favoris
#define UI_SYMBOL_CLOCK  "\xEF\x80\x97"   // Horloge (U+F017), recents
// Enfant 0 = recherche + Tous / Favoris / Recents, enfant 1 = "Reprendre" s'il
// est affiche ; puis les jeux : jeu n = enfant n + s_row_first.
static int s_row_first = 1;
#define GAME_ROW_FIRST s_row_first
static char s_search[32] = "";            // mots-cles du filtre en cours ("" = aucun)
static int s_rows_built = 0;              // nombre de lignes de jeux construites
static int s_rows_visible = 0;            // lignes affichees par le filtre
// Manette du telephone dans la liste des jeux (voir tel_nav_poll())
static int s_tel_prev = 0;                // etat precedent des boutons (fronts montants)
static int s_tel_cursor = -1;             // ligne sous le curseur, -1 = aucune
static uint32_t s_tel_repeat_at = 0;      // prochaine repetition d'une fleche maintenue
// Liste tout juste relue (envoi par le portail, suppression...) : A et START
// ignores un court instant - le curseur vient de repartir du haut, l'appui
// visait sans doute le jeu d'avant.
static uint32_t s_tel_launch_guard = 0;
static int s_tel_arrows_prev = 0;         // Fleches retenues au tour precedent (diagonales filtrees)
static uint32_t s_list_builds = 0;        // Nombre de constructions de la liste (copie TV a jour)
static lv_obj_t *s_search_bar = NULL;
static lv_obj_t *s_search_lbl = NULL;
static lv_obj_t *s_search_clear = NULL;
// En-tete de la liste
static lv_obj_t *s_mode_bm = NULL;        // Tous / Favoris / Recents
static lv_obj_t *s_resume_btn = NULL;     // "Reprendre" (dernier jeu joue)
static const char *s_resume_sub = "";     // son sous-titre (dessine par la colonne)
static bool s_launch_resume = false;      // le prochain lancement charge la reprise automatique
// Textes des lignes (labels en texte statique) : alloues en PSRAM au
// premier usage, pour ne pas prendre 4,8 Ko de RAM interne.
typedef char GameDisp[64];   // nom affiche (sans ".nes")
typedef char GameSub[64];    // "256 Ko", "Liste noire - 40 Ko", "<icone> Interne - 256 Ko"
static GameDisp *s_game_disp = NULL;
static GameSub *s_game_sub = NULL;
// Couleur du sous-titre de chaque ligne (dessine sans label, voir
// games_draw_subtitles()).
#define GF_BAD   0x01   // liste noire
#define GF_FLASH 0x02   // memoire interne
#define GF_FAV   0x04   // Favori (etoile)
static uint8_t s_game_flags[MAX_GAMES];

// Onglet Reglages
static lv_obj_t *s_vol_val = NULL;
static lv_obj_t *s_bl_val = NULL;
[[maybe_unused]] static lv_obj_t *s_wifi_switch = NULL;
[[maybe_unused]] static lv_obj_t *s_wifi_pill = NULL;
[[maybe_unused]] static lv_obj_t *s_wifi_detail = NULL;
static lv_obj_t *s_about_games = NULL;
static lv_obj_t *s_about_storage = NULL; // Espace libre carte SD + memoire interne

// Onglet Video
static lv_obj_t *s_bm_output = NULL;
static lv_obj_t *s_bm_norm = NULL;

[[maybe_unused]] static int s_last_wifi_status = -1;

// Les groupes (navigation a la manette) ne sont remplis que si la manette
// physique est compilee : economise un noeud de liste par element sinon.
static void group_add(lv_group_t *g, lv_obj_t *obj) {
#if HAS_PHYSICAL_CONTROLLER
  lv_group_add_obj(g, obj);
#else
  (void)g;
  (void)obj;
#endif
}

static void show_tab(int tab);
static void update_wifi_ui();
static void portal_refresh();
static void portal_ap_info_update();

// Demandes posees par les boutons et traitees dans la boucle principale :
// ces ecrans ont leur propre boucle et ne peuvent pas etre lances depuis un
// rappel LVGL (lv_timer_handler() n'est pas reentrant).
static volatile bool s_req_calibrate = false;
static volatile bool s_req_touch_test = false;
static volatile bool s_req_qr_full = false;
static volatile bool s_req_qr_next = false;
static volatile bool s_req_qr_close = false;
static volatile bool s_req_search_open = false;
static volatile bool s_req_search_close = false;
// Fiche d'un jeu (appui long) - voir sheet_open()
static volatile int s_req_sheet_open = -1;         // index du jeu
static volatile bool s_req_sheet_close = false;
static volatile bool s_req_sheet_confirm = false;  // "Supprimer" -> confirmation
static volatile bool s_req_sheet_play = false;
static volatile bool s_req_sheet_delete = false;   // suppression confirmee
// Pages de la liste des jeux. Changement de page ou de filtre demande par un
// evenement (bouton de la liste, croix du telephone) : traite par la boucle
// principale, la liste etant reconstruite (on ne detruit pas l'objet dont
// l'evenement est en cours).
static volatile bool s_req_view = false;           // reconstruire la liste (s_req_view_page)
static volatile int s_req_view_page = 0;
static volatile int s_req_view_cursor = -2;        // apres : curseur du telephone (0 : 1er jeu, -1 : dernier, -2 : aucun)
static volatile int s_req_mode = -1;               // Tous / Favoris / Recents (OSD_VIEW_*)
static volatile bool s_req_resume = false;         // carte "Reprendre"
static volatile bool s_req_pages_open = false;     // "Aller a la page"
static volatile int s_req_pages_go = -1;           // page choisie
static volatile bool s_req_pages_close = false;
static volatile bool s_req_sheet_fav = false;      // etoile de la fiche
static volatile bool s_req_sheet_resume = false;   // "Reprendre" de la fiche
static bool s_list_dirty = false;                  // favoris changes : liste a refaire en fermant la fiche
static lv_obj_t *s_sheet = NULL;                   // fiche ouverte (fond assombri plein ecran)

static void flag_event(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  // Doigt leve HORS du bouton = rien (LVGL garde l'appui sur le bouton ou le
  // doigt s'est pose, et l'aurait "clique" quand meme) - indispensable pour le
  // bouton rouge "Supprimer".
  lv_indev_t *in = lv_indev_get_act();
  if (in && lv_indev_get_type(in) == LV_INDEV_TYPE_POINTER) {
    lv_point_t pt;
    lv_indev_get_point(in, &pt);
    lv_obj_t *btn = lv_event_get_current_target(e);
    if (btn && !_lv_area_is_point_on(&btn->coords, &pt, 0)) return;
  }
  volatile bool *flag = (volatile bool *)lv_event_get_user_data(e);
  if (flag) *flag = true;
}

// Bouton plein (fond explicite : fonctionne aussi sans theme par defaut)
static lv_obj_t *make_button(lv_obj_t *parent, const char *txt, uint32_t bg, uint32_t fg) {
  lv_obj_t *btn = lv_btn_create(parent);
  if (bg == COL_ACC) {
    lv_obj_add_style(btn, &st_btn_main, 0);   // Degrade de l'accent
  } else {
    lv_obj_set_style_bg_color(btn, C(bg), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
  }
  lv_obj_set_style_radius(btn, 12, 0);
  lv_obj_set_style_pad_hor(btn, 10, 0);
  lv_obj_set_style_pad_ver(btn, 8, 0);
  lv_obj_set_style_shadow_width(btn, 0, 0);
  lv_obj_add_style(btn, &st_focus_key, LV_STATE_FOCUS_KEY);
  lv_obj_t *l = make_label(btn, txt, &ui_font_14, fg);
  lv_obj_center(l);
  return btn;
}

// ============================================================================
// QR code : genere par nes_qrcodegen (Nayuki, licence MIT) et dessine dans une
// image en PSRAM - ne depend plus de LV_USE_QRCODE.
// ============================================================================
struct QrImage {
  lv_img_dsc_t dsc;
  lv_color_t *pixels;
};

static void qr_image_delete_cb(lv_event_t *e) {
  QrImage *q = (QrImage *)lv_event_get_user_data(e);
  if (!q) return;
  // LVGL garde en cache les images deja decodees, reperees par l'adresse de
  // leur descripteur : on l'oublie AVANT de liberer, sinon un prochain QR
  // alloue a la meme adresse afficherait le contenu precedent.
  lv_img_cache_invalidate_src(&q->dsc);
  if (q->pixels) heap_caps_free(q->pixels);
  free(q);
}

// Cree une image carree d'au plus max_px de cote (marge blanche de 2
// modules comprise). Renvoie NULL si le texte ne tient pas ou si la
// memoire manque.
static lv_obj_t *make_qr_image(lv_obj_t *parent, const char *text, int max_px) {
  static uint8_t qr[nesqr_BUFFER_LEN_FOR_VERSION(10)];
  static uint8_t tmp[nesqr_BUFFER_LEN_FOR_VERSION(10)];
  if (!nesqr_encodeText(text, tmp, qr, nesqr_Ecc_MEDIUM, nesqr_VERSION_MIN, 10, nesqr_Mask_AUTO, true)) {
    Serial.println("[UI] QR code : texte trop long");
    return NULL;
  }
  const int n = nesqr_getSize(qr);
  const int quiet = 2;
  const int total = n + 2 * quiet;
  int scale = max_px / total;
  if (scale < 1) scale = 1;
  const int px = total * scale;

  QrImage *q = (QrImage *)malloc(sizeof(QrImage));
  if (!q) return NULL;
  q->pixels = (lv_color_t *)heap_caps_malloc((size_t)px * px * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  if (!q->pixels) q->pixels = (lv_color_t *)malloc((size_t)px * px * sizeof(lv_color_t));
  if (!q->pixels) {
    free(q);
    return NULL;
  }
  const lv_color_t black = lv_color_black();
  const lv_color_t white = lv_color_white();
  for (int y = 0; y < px; y++) {
    int my = y / scale - quiet;
    for (int x = 0; x < px; x++) {
      int mx = x / scale - quiet;
      bool dark = (mx >= 0 && mx < n && my >= 0 && my < n) && nesqr_getModule(qr, mx, my);
      q->pixels[y * px + x] = dark ? black : white;
    }
  }
  memset(&q->dsc, 0, sizeof(q->dsc));
  q->dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
  q->dsc.header.w = (uint32_t)px;
  q->dsc.header.h = (uint32_t)px;
  q->dsc.data_size = (uint32_t)px * px * sizeof(lv_color_t);
  q->dsc.data = (const uint8_t *)q->pixels;

  lv_obj_t *img = lv_img_create(parent);
  lv_img_set_src(img, &q->dsc);
  lv_obj_add_event_cb(img, qr_image_delete_cb, LV_EVENT_DELETE, q);
  return img;
}

// Texte du QR "rejoindre le reseau" du point d'acces.
static void wifi_join_qr_text(char *out, size_t n) {
  char ssid[48], pass[72];   // echappes : au plus 2 x la longueur utile
  auto esc = [](const char *in, char *o, size_t on) {
    size_t k = 0;
    for (; *in && k + 2 < on; in++) {
      if (*in == '\\' || *in == ';' || *in == ',' || *in == ':' || *in == '"') o[k++] = '\\';
      o[k++] = *in;
    }
    o[k] = '\0';
  };
  esc(WIFI_AP_SSID, ssid, sizeof(ssid));
  esc(WIFI_AP_PASSWORD, pass, sizeof(pass));
  if (strlen(WIFI_AP_PASSWORD) >= 8) snprintf(out, n, "WIFI:T:WPA;S:%s;P:%s;;", ssid, pass);
  else snprintf(out, n, "WIFI:T:nopass;S:%s;;", ssid);
}

// ============================================================================
// Onglet Jeux
// ============================================================================
static void format_size(uint32_t bytes, char *out, size_t n) {
  if (bytes == 0) {
    out[0] = '\0';
  } else if (bytes < 1024UL * 1024UL) {
    snprintf(out, n, "%lu Ko", (unsigned long)((bytes + 1023UL) / 1024UL));
  } else {
    unsigned long tenths = (unsigned long)(((uint64_t)bytes * 10ULL + 524288ULL) / 1048576ULL);
    snprintf(out, n, "%lu,%lu Mo", tenths / 10UL, tenths % 10UL);
  }
}

// Espace libre d'un emplacement, ex. "6,3 Mo libres", "14,2 Go libres".
static void format_free(int where, char *out, size_t n) {
  uint32_t free_kb, total_kb;
  if (!osd_storage_info(where, &free_kb, &total_kb)) {
    snprintf(out, n, "%s", where == OSD_WHERE_FLASH ? "indisponible" : "absente");
    return;
  }
  unsigned long tenths;
  const char *unit;
  if (free_kb >= 1024UL * 1024UL) { tenths = (unsigned long)(((uint64_t)free_kb * 10ULL + 524288ULL) / 1048576ULL); unit = "Go"; }
  else { tenths = (unsigned long)(((uint64_t)free_kb * 10ULL + 512ULL) / 1024ULL); unit = "Mo"; }
  snprintf(out, n, "%lu,%lu %s libres", tenths / 10UL, tenths % 10UL, unit);
}

static void update_storage_labels() {
  if (!s_about_storage) return;
  char sd[32], fl[32];
  format_free(OSD_WHERE_SD, sd, sizeof(sd));
  format_free(OSD_WHERE_FLASH, fl, sizeof(fl));
  lv_label_set_text_fmt(s_about_storage, "%s\n#%06X %s#", sd, (unsigned)COL_FLASH, fl);   // couleur "memoire interne" du theme
}

// Un seul rappel pour toute la liste (sur la colonne) : les lignes
// transmettent leur clic a leur parent (LV_OBJ_FLAG_EVENT_BUBBLE) - evite
// d'allouer un rappel par jeu. L'index du jeu = position de la ligne.
// Un doigt pose pour ARRETER une liste lancee d'un coup de doigt ne doit
// pas lancer le jeu qui passe dessous : un appui qui suit de moins de
// 100 ms la fin d'un defilement est ignore.
static uint32_t s_list_scroll_end = 0;
static bool s_ignore_click = false;

// Appui du doigt moins de 100 ms apres la fin d'un defilement de la liste ?
// (partage avec la barre de recherche, qui defile avec la liste.)
static bool press_stops_scroll() {
  lv_indev_t *indev = lv_indev_get_act();
  return indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER &&
         lv_tick_elaps(s_list_scroll_end) < 100;
}

// Sous-titres des lignes visibles, dessines par la colonne apres ses enfants
// (donc par-dessus le fond de chaque ligne).
static void games_draw_subtitles(lv_event_t *e) {
  if (!s_games_col || !s_game_sub) return;
  lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
  if (!ctx || !ctx->clip_area) return;
  lv_draw_label_dsc_t d;
  lv_draw_label_dsc_init(&d);
  d.font = &ui_font_12;
  const lv_coord_t lh = lv_font_get_line_height(&ui_font_12);
  if (s_resume_btn) {   // Sous-titre de la carte "Reprendre"
    lv_area_t a;
    a.x1 = s_resume_btn->coords.x1 + 12;
    a.x2 = s_resume_btn->coords.x2 - 8;
    a.y1 = s_resume_btn->coords.y1 + 25;
    a.y2 = a.y1 + lh - 1;
    lv_area_t vis;
    if (_lv_area_intersect(&vis, &a, ctx->clip_area)) {
      d.color = C(COL_MUT);
      lv_draw_label(ctx, &d, &a, s_resume_sub, NULL);
    }
  }
  for (int i = 0; i < s_rows_built; i++) {
    lv_obj_t *row = lv_obj_get_child(s_games_col, i + GAME_ROW_FIRST);
    if (!row) break;
    if (lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN)) continue;
    lv_area_t a;
    a.x1 = row->coords.x1 + 12;
    a.x2 = row->coords.x2 - 8;
    a.y1 = row->coords.y1 + 25;
    a.y2 = a.y1 + lh - 1;
    lv_area_t vis;
    if (!_lv_area_intersect(&vis, &a, ctx->clip_area)) continue;
    uint8_t f = s_game_flags[i];
    d.color = C((f & GF_BAD) ? COL_WARM : (f & GF_FLASH) ? COL_FLASH : COL_MUT);
    lv_draw_label(ctx, &d, &a, s_game_sub[i], NULL);
    if (f & GF_FAV) {   // Favori, etoile a droite du sous-titre
      lv_area_t st = a;
      st.x1 = a.x2 - 14;
      lv_draw_label_dsc_t ds = d;
      ds.color = C(COL_WARM);
      lv_draw_label(ctx, &ds, &st, UI_SYMBOL_STAR, NULL);
    }
  }
}

static void games_col_event(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t *target = lv_event_get_target(e);
  if (code == LV_EVENT_DRAW_POST_END) {
    if (target == s_games_col) games_draw_subtitles(e);
    return;
  }
  if (code == LV_EVENT_SCROLL_END && target == s_games_col) {
    s_list_scroll_end = lv_tick_get();
    return;
  }
  if (code == LV_EVENT_PRESSED) {
    s_ignore_click = press_stops_scroll();
    return;
  }
  // Appui long (ou A maintenu) sur un jeu -> sa fiche (Jouer / Supprimer). Le
  // lancement passe donc sur SHORT_CLICKED, que LVGL n'envoie pas apres un
  // appui long.
  if (code == LV_EVENT_LONG_PRESSED) {
    if (target && lv_obj_get_parent(target) == s_games_col) {
      int idx = (int)lv_obj_get_index(target) - GAME_ROW_FIRST;
      if (idx >= 0 && idx < s_rows_built) s_req_sheet_open = idx;
      // La fiche attend le relachement (lv_indev_wait_release) : LVGL
      // n'enverra pas RELEASED a la ligne, qui resterait dessinee appuyee.
      lv_obj_clear_state(target, LV_STATE_PRESSED);
    }
    return;
  }
  if (code != LV_EVENT_SHORT_CLICKED) return;
  if (s_ignore_click) {
    s_ignore_click = false;
    return;
  }
  if (target && lv_obj_get_parent(target) == s_games_col) {
    int idx = (int)lv_obj_get_index(target) - GAME_ROW_FIRST;   // Barre de recherche en tete
    if (idx >= 0 && idx < s_rows_built) s_launch_index = idx;
  }
}

static void goto_portal_event(lv_event_t *e) {
  if (lv_event_get_code(e) == LV_EVENT_CLICKED) show_tab(TAB_PORTAL);
}

[[maybe_unused]] static void goto_settings_event(lv_event_t *e) {
  if (lv_event_get_code(e) == LV_EVENT_CLICKED) show_tab(TAB_SETTINGS);
}

// ============================================================================
// Recherche : mots-cles tapes au clavier a l'ecran ; un jeu est garde s'il
// contient TOUS les mots (sans tenir compte des majuscules). Le filtre ne cree
// ni ne detruit aucune ligne : il cache les autres (LV_OBJ_FLAG_HIDDEN), donc
// aucune memoire LVGL en plus.
// ============================================================================
// hay : les hay_len premiers caracteres d'un nom (pas forcement termine
// par '\0' a cet endroit) ; needle : n caracteres.
static bool ci_contains(const char *hay, size_t hay_len, const char *needle, size_t n) {
  if (n > hay_len) return false;
  for (size_t s = 0; s + n <= hay_len; s++) {
    size_t i = 0;
    while (i < n && tolower((unsigned char)hay[s + i]) == tolower((unsigned char)needle[i])) i++;
    if (i == n) return true;
  }
  return false;
}

static bool name_matches(const char *name, size_t name_len, const char *query) {
  const char *q = query;
  while (*q) {
    while (*q == ' ') q++;
    if (!*q) break;
    const char *e = q;
    while (*e && *e != ' ') e++;
    if (!ci_contains(name, name_len, q, (size_t)(e - q))) return false;
    q = e;
  }
  return true;
}

// Nom complet du jeu i, sans l'extension ".nes" (*len = sa longueur).
// La recherche porte sur ce nom-la et JAMAIS sur s_game_disp[] : pour un
// nom trop long pour la ligne, LVGL (LV_LABEL_LONG_DOT) ecrit "..." a
// l'interieur meme de ce tampon, la fin du nom y serait perdue.
static const char *game_search_name(int i, size_t *len) {
  const char *f = osd_game_filename(i);
  if (!f) f = "";
  size_t l = strlen(f);
  if (l > 4 && strcasecmp(f + l - 4, ".nes") == 0) l -= 4;
  *len = l;
  return f;
}

static bool game_matches(int i, const char *query) {
  if (!query[0]) return true;
  size_t len;
  const char *nm = game_search_name(i, &len);
  return name_matches(nm, len, query);
}

static void update_title();

static void search_bar_refresh() {
  if (!s_search_lbl) return;
  if (s_search[0]) {
    const int found = osd_games_matching();   // Toutes les pages
    if (found > 0)
      lv_label_set_text_fmt(s_search_lbl, UI_SYMBOL_SEARCH " %s \xC2\xB7 %d", s_search, found);
    else
      lv_label_set_text_fmt(s_search_lbl, UI_SYMBOL_SEARCH " %s \xC2\xB7 0", s_search);
    lv_obj_set_style_text_color(s_search_lbl, C(found > 0 ? COL_ACC_TXT : COL_WARM), 0);
    lv_obj_clear_flag(s_search_clear, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_label_set_text(s_search_lbl, UI_SYMBOL_SEARCH "  Rechercher");
    lv_obj_set_style_text_color(s_search_lbl, C(COL_MUT), 0);
    lv_obj_add_flag(s_search_clear, LV_OBJ_FLAG_HIDDEN);
  }
}

// Manette : la selection de l'onglet Jeux va sur le premier jeu visible, sauf
// si elle est deja sur un jeu encore visible. Si aucun jeu n'est visible : sur
// la barre de recherche.
static void games_focus_default(lv_obj_t *first_visible) {
#if HAS_PHYSICAL_CONTROLLER
  lv_obj_t *f = lv_group_get_focused(s_groups[TAB_GAMES]);
  if (f && f != s_search_bar && !lv_obj_has_flag(f, LV_OBJ_FLAG_HIDDEN)) return;
  lv_obj_t *target = first_visible ? first_visible : s_search_bar;
  if (!target) return;
  lv_group_focus_obj(target);
  // Contour de selection visible tout de suite, meme si le groupe n'est
  // pas encore celui de la manette (liste construite onglet cache).
  if (s_keypad_indev) lv_obj_add_state(target, LV_STATE_FOCUS_KEY);
#else
  (void)first_visible;
#endif
}

// Montre/cache les lignes selon s_search ; met a jour la barre et le titre.
static void apply_search_filter() {
  s_rows_visible = 0;
  if (!s_games_col) return;
  lv_obj_t *first = NULL;
  for (int i = 0; i < s_rows_built; i++) {
    lv_obj_t *row = lv_obj_get_child(s_games_col, i + GAME_ROW_FIRST);
    if (!row) break;
    if (game_matches(i, s_search)) {
      lv_obj_clear_flag(row, LV_OBJ_FLAG_HIDDEN);
      if (!first) first = row;
      s_rows_visible++;
    } else {
      lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    }
  }
  games_focus_default(first);
  lv_obj_scroll_to_y(s_games_col, 0, LV_ANIM_OFF);
  search_bar_refresh();
  update_title();
}

// La barre et sa croix defilent avec la liste : meme garde-fou que les
// lignes (un doigt pose pour ARRETER la liste n'ouvre pas la recherche).
static void search_bar_event(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_PRESSED) { s_ignore_click = press_stops_scroll(); return; }
  if (code != LV_EVENT_CLICKED) return;
  if (s_ignore_click) { s_ignore_click = false; return; }
  s_req_search_open = true;
}

static void search_clear_event(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_PRESSED) { s_ignore_click = press_stops_scroll(); return; }
  if (code != LV_EVENT_CLICKED) return;
  if (s_ignore_click) { s_ignore_click = false; return; }
  s_search[0] = '\0';
  // Le filtre porte sur tous les jeux (pages) : liste reconstruite par la
  // boucle principale (cette croix est detruite avec elle).
  s_req_view_page = 0;
  s_req_view_cursor = -2;
  s_req_view = true;
}

// Barre "Rechercher un jeu" en tete de liste (enfant 0 de s_games_col).
static void mode_event(lv_event_t *e);
static const char *s_mode_map[] = {"Tous", UI_SYMBOL_STAR, UI_SYMBOL_CLOCK, ""};

static void build_search_bar(lv_group_t *g) {
  // Une rangee (enfant 0 de la colonne) : la recherche, puis Tous / Favoris /
  // Recents - la liste garde sa place a l'ecran.
  lv_obj_t *bar = make_plain(s_games_col);
  lv_obj_set_size(bar, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(bar, 6, 0);
  lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
  s_search_bar = lv_btn_create(bar);
  lv_obj_remove_style_all(s_search_bar);
  lv_obj_add_style(s_search_bar, &st_row, 0);
  lv_obj_add_style(s_search_bar, &st_row_pr, LV_STATE_PRESSED);
  lv_obj_add_style(s_search_bar, &st_row_focus, LV_STATE_FOCUS_KEY);
  lv_obj_set_height(s_search_bar, 34);
  lv_obj_set_flex_grow(s_search_bar, 1);
  lv_obj_clear_flag(s_search_bar, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_search_bar, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
  lv_obj_add_event_cb(s_search_bar, search_bar_event, LV_EVENT_ALL, NULL);

  s_search_lbl = make_label(s_search_bar, "", &ui_font_14, COL_MUT);
  lv_label_set_long_mode(s_search_lbl, LV_LABEL_LONG_DOT);
  lv_obj_align(s_search_lbl, LV_ALIGN_LEFT_MID, 12, 0);

  // Croix pour effacer le filtre (visible seulement si un filtre est actif)
  s_search_clear = lv_btn_create(s_search_bar);
  lv_obj_remove_style_all(s_search_clear);
  lv_obj_set_size(s_search_clear, 40, 34);
  lv_obj_align(s_search_clear, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_t *x = make_label(s_search_clear, LV_SYMBOL_CLOSE, &ui_font_14, COL_TXT);
  lv_obj_center(x);
  lv_obj_add_event_cb(s_search_clear, search_clear_event, LV_EVENT_ALL, NULL);

  s_mode_bm = make_segmented(bar, s_mode_map);
  lv_obj_set_size(s_mode_bm, 132, 34);
  lv_btnmatrix_set_btn_width(s_mode_bm, 0, 3);   // "Tous" plus large que les icones
  lv_btnmatrix_set_btn_width(s_mode_bm, 1, 2);
  lv_btnmatrix_set_btn_width(s_mode_bm, 2, 2);
  lv_btnmatrix_set_btn_ctrl(s_mode_bm, (uint16_t)osd_games_mode(), LV_BTNMATRIX_CTRL_CHECKED);
  lv_obj_add_event_cb(s_mode_bm, mode_event, LV_EVENT_VALUE_CHANGED, NULL);

  // largeur du texte : la rangee placee, place gardee pour la croix
  lv_obj_update_layout(bar);
  lv_obj_set_width(s_search_lbl, lv_obj_get_width(s_search_bar) - 12 - 3 - 36);
  lv_obj_set_height(s_search_lbl, lv_font_get_line_height(&ui_font_14));   // une ligne : "..." si trop long

  group_add(g, s_search_bar);
  group_add(g, s_mode_bm);
  search_bar_refresh();
}

// En-tete de la liste (enfant 1 de s_games_col) : Tous / Favoris / Recents,
// puis la carte "Reprendre" (dernier jeu joue ; premiere page, sans
// recherche). A la manette : GAUCHE/DROITE changent de liste.
static int s_resume_ci = -1;            // jeu de la carte "Reprendre" (catalogue)

static void game_display_name(const char *fname, char *out, size_t n) {
  snprintf(out, n, "%s", fname);
  const size_t l = strlen(out);
  if (l > 4 && strcasecmp(out + l - 4, ".nes") == 0) out[l - 4] = '\0';
}

// Coupe un texte UTF-8 a max_chars caracteres (jamais au milieu d'un
// caractere accentue), "..." au bout s'il etait plus long.
static void utf8_cut(char *s, int max_chars) {
  int chars = 0;
  for (char *p = s; *p; p++) {
    if (((uint8_t)*p & 0xC0) == 0x80) continue;   // suite d'un caractere
    if (chars == max_chars) {
      if ((size_t)(p - s) + 4 <= 63) memcpy(p, "\xE2\x80\xA6", 4);
      else *p = '\0';
      return;
    }
    chars++;
  }
}

static void mode_event(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
  const uint16_t b = lv_btnmatrix_get_selected_btn(lv_event_get_target(e));
  if (b <= OSD_VIEW_RECENTS && (int)b != osd_games_mode()) s_req_mode = b;
}

static void resume_event(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_PRESSED) { s_ignore_click = press_stops_scroll(); return; }
  // appui court seulement (comme les jeux : un appui long ne lance rien)
  if (code != LV_EVENT_SHORT_CLICKED) return;
  if (s_ignore_click) { s_ignore_click = false; return; }
  s_req_resume = true;
}

static void build_list_header(lv_group_t *g) {
  // enfant direct de la colonne (pas de conteneur : memoire LVGL comptee)
  lv_obj_t *hdr = s_games_col;
  s_row_first = 1;
  s_resume_btn = NULL;
  s_resume_ci = -1;
  if (s_search[0] || osd_games_page() != 0 || osd_games_mode() == OSD_VIEW_FAVS) return;
  bool has_state = false;
  const int ci = osd_resume_catalog(&has_state);
  if (ci < 0) return;
  s_resume_ci = ci;
  char nm[64], txt[96];
  game_display_name(osd_catalog_filename(ci), nm, sizeof(nm));
  lv_obj_t *b = lv_btn_create(hdr);
  lv_obj_remove_style_all(b);
  lv_obj_add_style(b, &st_row, 0);
  lv_obj_add_style(b, &st_row_pr, LV_STATE_PRESSED);
  lv_obj_add_style(b, &st_row_focus, LV_STATE_FOCUS_KEY);
  lv_obj_set_style_border_color(b, C(COL_ACC), 0);
  lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(b, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
  lv_obj_add_event_cb(b, resume_event, LV_EVENT_ALL, NULL);
  // Un seul label (memoire LVGL comptee) : nom sur la 1re ligne, le
  // sous-titre dessine par la colonne comme celui des jeux (s_resume_sub).
  snprintf(txt, sizeof(txt), LV_SYMBOL_PLAY "  Reprendre \xC2\xB7 %s", nm);
  lv_obj_t *l1 = lv_label_create(b);
  lv_obj_add_style(l1, &st_row_name, 0);
  lv_obj_set_style_text_color(l1, C(COL_ACC_TXT), 0);
  lv_label_set_long_mode(l1, LV_LABEL_LONG_DOT);
  lv_label_set_text(l1, txt);
  s_resume_sub = has_state ? "La partie repart l\xC3\xA0 o\xC3\xB9 tu l'as quitt\xC3\xA9" "e" : "Dernier jeu jou\xC3\xA9";
  group_add(g, b);
  s_resume_btn = b;
  s_row_first = 2;
}

static void update_title() {
  if (!s_title) return;
  switch (s_tab) {
    case TAB_GAMES: {
      int n = osd_games_total();   // Tous les jeux, pas seulement la page
      const int mode = osd_games_mode();
      const char *t = mode == OSD_VIEW_FAVS ? "Favoris" : mode == OSD_VIEW_RECENTS ? "R\xC3\xA9" "cents" : "Jeux";
      if (mode != OSD_VIEW_ALL) n = mode == OSD_VIEW_FAVS ? osd_fav_count() : osd_recent_count();
      if (n > 0 && s_search[0]) lv_label_set_text_fmt(s_title, "%s \xC2\xB7 %d/%d", t, osd_games_matching(), n);   // Filtre
      else if (mode != OSD_VIEW_ALL) lv_label_set_text_fmt(s_title, "%s \xC2\xB7 %d", t, osd_games_matching());
      else if (n > 0) lv_label_set_text_fmt(s_title, "Jeux \xC2\xB7 %d", n);   // "·"
      else lv_label_set_text(s_title, "Jeux");
      break;
    }
    case TAB_SETTINGS: lv_label_set_text(s_title, "R\xC3\xA9glages"); break;
    case TAB_VIDEO:    lv_label_set_text(s_title, "Sortie vid\xC3\xA9o"); break;
    default:           lv_label_set_text(s_title, "Portail web"); break;
  }
}

// Barre de pages en bas de la liste : "< Precedents", "51 a 100 sur 812",
// "Suivants >". Boutons de la manette comme les lignes (group_add).
static void pager_event(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_PRESSED) { s_ignore_click = press_stops_scroll(); return; }
  if (code != LV_EVENT_CLICKED) return;
  if (s_ignore_click) { s_ignore_click = false; return; }
  const int d = (int)(intptr_t)lv_event_get_user_data(e);
  s_req_view_page = osd_games_page() + d;
  s_req_view_cursor = -2;
  s_req_view = true;
}

static lv_obj_t *pager_button(lv_obj_t *parent, const char *txt, int dir, bool enabled, lv_group_t *g) {
  lv_obj_t *b = lv_btn_create(parent);
  lv_obj_remove_style_all(b);
  lv_obj_add_style(b, &st_row, 0);
  lv_obj_add_style(b, &st_row_pr, LV_STATE_PRESSED);
  lv_obj_add_style(b, &st_row_focus, LV_STATE_FOCUS_KEY);
  lv_obj_set_size(b, 84, 34);
  lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(b, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
  lv_obj_t *l = make_label(b, txt, &ui_font_14, enabled ? COL_TXT : COL_MUT);
  lv_obj_center(l);
  if (enabled) {
    lv_obj_add_event_cb(b, pager_event, LV_EVENT_ALL, (void *)(intptr_t)dir);
    if (g) group_add(g, b);
  } else {
    lv_obj_set_style_bg_opa(b, LV_OPA_20, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE);
  }
  return b;
}

// Le milieu de la barre ouvre "Aller a la page" (toutes les pages d'un coup,
// avec le premier et le dernier jeu de chacune).
static void pages_btn_event(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_PRESSED) { s_ignore_click = press_stops_scroll(); return; }
  if (code != LV_EVENT_CLICKED) return;
  if (s_ignore_click) { s_ignore_click = false; return; }
  s_req_pages_open = true;
}

static void build_pager(lv_group_t *g) {
  const int page = osd_games_page(), pages = osd_games_page_count();
  const int first = osd_games_page_first() + 1, last = osd_games_page_first() + osd_games_count();
  lv_obj_t *bar = make_plain(s_games_col);
  lv_obj_set_size(bar, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(bar, 6, 0);
  lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *prev = pager_button(bar, LV_SYMBOL_LEFT "  Pr\xC3\xA9" "c.", -1, page > 0, NULL);
  lv_obj_t *mid = lv_btn_create(bar);
  lv_obj_remove_style_all(mid);
  lv_obj_add_style(mid, &st_row, 0);
  lv_obj_add_style(mid, &st_row_pr, LV_STATE_PRESSED);
  lv_obj_add_style(mid, &st_row_focus, LV_STATE_FOCUS_KEY);
  lv_obj_set_height(mid, 34);
  lv_obj_set_flex_grow(mid, 1);
  lv_obj_clear_flag(mid, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(mid, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
  lv_obj_add_event_cb(mid, pages_btn_event, LV_EVENT_ALL, NULL);
  lv_obj_t *ml = make_label(mid, "", &ui_font_12, COL_TXT);
  lv_label_set_text_fmt(ml, "%d\xE2\x80\x93%d / %d\nPage %d/%d " LV_SYMBOL_DOWN, first, last, osd_games_matching(),
                        page + 1, pages);
  lv_obj_set_style_text_align(ml, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_line_space(ml, -1, 0);
  lv_obj_center(ml);
  lv_obj_t *next = pager_button(bar, "Suiv.  " LV_SYMBOL_RIGHT, +1, page < pages - 1, NULL);
  // ordre a la manette : Prec., Page, Suiv.
  if (page > 0) group_add(g, prev);
  group_add(g, mid);
  if (page < pages - 1) group_add(g, next);
}

// ============================================================================
// "Aller a la page" - grille des pages (5 par rangee), la page en cours
// cochee ; au-dessus, les jeux de la page designee (doigt appuye ou selection
// a la manette). Fenetre sur lv_layer_top(), comme la fiche.
// ============================================================================
#define PAGES_MAX ((MAX_GAMES_TOTAL + MAX_GAMES - 1) / MAX_GAMES)
static lv_obj_t *s_pages = NULL;
static lv_obj_t *s_pages_bm = NULL;
static lv_obj_t *s_pages_info = NULL;
static lv_group_t *s_pages_group = NULL;
static char s_pg_lbl[PAGES_MAX][4];
static const char *s_pg_map[PAGES_MAX + PAGES_MAX / 5 + 2];
static int s_pages_shown = -2;

static void pages_bm_event(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
  const uint16_t b = lv_btnmatrix_get_selected_btn(lv_event_get_target(e));
  if (b != LV_BTNMATRIX_BTN_NONE) s_req_pages_go = b;
}

static void pages_info_update() {
  if (!s_pages_bm || !s_pages_info) return;
  int p = lv_btnmatrix_get_selected_btn(s_pages_bm);
  if (p == LV_BTNMATRIX_BTN_NONE || p >= osd_games_page_count()) p = osd_games_page();
  if (p == s_pages_shown) return;
  s_pages_shown = p;
  const int first = p * MAX_GAMES;
  int last = first + MAX_GAMES - 1;
  if (last >= osd_games_matching()) last = osd_games_matching() - 1;
  char a[64], b[64];
  game_display_name(osd_games_name_at(first), a, sizeof(a));
  game_display_name(osd_games_name_at(last), b, sizeof(b));
  utf8_cut(a, 34);
  utf8_cut(b, 32);
  lv_label_set_text_fmt(s_pages_info, "Page %d \xC2\xB7 jeux %d \xC3\xA0 %d\n%s\n" LV_SYMBOL_RIGHT " %s", p + 1, first + 1, last + 1, a, b);
}

static void pages_close() {
  if (!s_pages) return;
  lv_obj_del(s_pages);
  s_pages = s_pages_bm = s_pages_info = NULL;
  s_keypad_arrows = false;
  if (s_pages_group) lv_group_remove_all_objs(s_pages_group);
  if (s_keypad_indev) lv_indev_wait_release(s_keypad_indev);
  show_tab(TAB_GAMES);
}

static void pages_open() {
  const int n = osd_games_page_count();
  if (s_pages || n < 2) return;
  if (!s_pages_group) s_pages_group = lv_group_create();
  lv_group_remove_all_objs(s_pages_group);
  int k = 0;
  for (int p = 0; p < n && p < PAGES_MAX; p++) {
    if (p > 0 && p % 5 == 0) s_pg_map[k++] = "\n";
    snprintf(s_pg_lbl[p], sizeof(s_pg_lbl[p]), "%d", p + 1);
    s_pg_map[k++] = s_pg_lbl[p];
  }
  s_pg_map[k] = "";
  const int rows = (n + 4) / 5;

  s_pages = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(s_pages);
  lv_obj_set_size(s_pages, DISPLAY_WIDTH, DISPLAY_HEIGHT);
  lv_obj_set_style_bg_color(s_pages, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(s_pages, LV_OPA_50, 0);
  lv_obj_clear_flag(s_pages, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_pages, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_pages, flag_event, LV_EVENT_CLICKED, (void *)&s_req_pages_close);

  lv_obj_t *card = lv_obj_create(s_pages);
  lv_obj_remove_style_all(card);
  lv_obj_add_style(card, &st_card, 0);
  lv_obj_set_style_radius(card, 16, 0);
  lv_obj_set_style_pad_all(card, 10, 0);
  lv_obj_set_style_pad_row(card, 6, 0);
  lv_obj_set_size(card, DISPLAY_WIDTH - 16, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(card, LV_ALIGN_BOTTOM_MID, 0, -8);

  lv_obj_t *head = make_plain(card);
  lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);
  make_label(head, "Aller \xC3\xA0 la page", &ui_font_20, COL_TXT);
  lv_obj_t *x = make_button(head, LV_SYMBOL_CLOSE, COL_SURF2, COL_TXT);
  lv_obj_set_size(x, 40, 30);
  lv_obj_align(x, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_add_event_cb(x, flag_event, LV_EVENT_CLICKED, (void *)&s_req_pages_close);

  s_pages_info = make_label(card, "", &ui_font_12, COL_MUT);
  lv_obj_set_width(s_pages_info, LV_PCT(100));
  lv_label_set_long_mode(s_pages_info, LV_LABEL_LONG_CLIP);

  s_pages_bm = make_segmented(card, s_pg_map);
  lv_obj_set_height(s_pages_bm, rows * 30 + (rows - 1) * 4 + 4);
  lv_obj_clear_flag(s_pages_bm, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
  lv_btnmatrix_set_btn_ctrl(s_pages_bm, (uint16_t)osd_games_page(), LV_BTNMATRIX_CTRL_CHECKED);
  lv_obj_add_event_cb(s_pages_bm, pages_bm_event, LV_EVENT_VALUE_CHANGED, NULL);
  group_add(s_pages_group, s_pages_bm);
  s_pages_shown = -2;
  pages_info_update();

  if (s_touch_indev) lv_indev_wait_release(s_touch_indev);
  if (s_keypad_indev) {
    lv_indev_wait_release(s_keypad_indev);
    s_keypad_arrows = true;   // HAUT/BAS d'une rangee a l'autre de la grille
    lv_indev_set_group(s_keypad_indev, s_pages_group);
    lv_group_focus_obj(s_pages_bm);
    lv_obj_add_state(s_pages_bm, LV_STATE_FOCUS_KEY);
  }
}

static void build_games_panel() {
  lv_obj_t *panel = s_panels[TAB_GAMES];
  // (Sauf fiche de jeu ouverte : le doigt ou A est alors sur un bouton de la
  // fiche, que la reconstruction ne touche pas.)
  if (s_tab == TAB_GAMES && !s_sheet) {
    if (s_touch_indev) lv_indev_wait_release(s_touch_indev);
    if (s_keypad_indev) lv_indev_wait_release(s_keypad_indev);
  }
  s_req_sheet_open = -1;
  s_tel_cursor = -1;       // Idem pour le curseur et le jeu choisi au telephone
  s_tel_launch_guard = millis() + 800;
  s_list_builds++;                       // La copie sur la television est redessinee
  lv_group_remove_all_objs(s_groups[TAB_GAMES]);   // avant lv_obj_clean() : evite 50 changements de focus
  lv_obj_clean(panel);
  s_games_col = NULL;
  s_search_bar = s_search_lbl = s_search_clear = NULL;
  s_mode_bm = s_resume_btn = NULL;
  s_rows_built = 0;
  s_rows_visible = 0;

  int n = osd_games_count();   // Jeux de la page affichee
  if (osd_games_total() <= 0) {
    // Etat vide : plus d'ecran d'erreur bloquant comme - le portail reste
    // utilisable pour envoyer des jeux.
    lv_obj_t *box = make_plain(panel);
    lv_obj_set_size(box, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 6, 0);
    uint32_t fk, tk;
    bool has_sd = osd_storage_info(OSD_WHERE_SD, &fk, &tk);
    bool has_flash = osd_storage_info(OSD_WHERE_FLASH, &fk, &tk);
    make_label(box, has_sd ? LV_SYMBOL_SD_CARD : LV_SYMBOL_DRIVE, &ui_font_20, COL_MUT);
    const char *msg = !has_sd ? "Carte SD absente et aucun jeu en m\xC3\xA9moire interne"
                    : has_flash ? "Aucun jeu (.nes) sur la carte SD ni en m\xC3\xA9moire interne"
                    : "Aucun jeu (.nes) \xC3\xA0 la racine de la carte SD";
    lv_obj_t *l1 = make_label(box, msg, &ui_font_14, COL_TXT);
    lv_obj_set_width(l1, 280);
    lv_label_set_long_mode(l1, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(l1, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *btn = lv_btn_create(box);
    lv_obj_set_style_bg_color(btn, C(COL_ACC), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_pad_hor(btn, 14, 0);
    lv_obj_set_style_pad_ver(btn, 8, 0);
    lv_obj_add_style(btn, &st_focus_key, LV_STATE_FOCUS_KEY);
    lv_obj_add_event_cb(btn, goto_portal_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = make_label(btn, LV_SYMBOL_WIFI "  Envoyer des jeux (Portail)", &ui_font_14, COL_ACC_INK);
    lv_obj_center(bl);
    group_add(s_groups[TAB_GAMES], btn);
    update_title();
    return;
  }

  if (!s_game_disp) s_game_disp = (GameDisp *)heap_caps_malloc(sizeof(GameDisp) * MAX_GAMES, MALLOC_CAP_SPIRAM);
  if (!s_game_disp) s_game_disp = (GameDisp *)malloc(sizeof(GameDisp) * MAX_GAMES);
  if (!s_game_sub) s_game_sub = (GameSub *)heap_caps_malloc(sizeof(GameSub) * MAX_GAMES, MALLOC_CAP_SPIRAM);
  if (!s_game_sub) s_game_sub = (GameSub *)malloc(sizeof(GameSub) * MAX_GAMES);
  if (!s_game_disp || !s_game_sub) {
    make_label(panel, "M\xC3\xA9moire insuffisante pour la liste", &ui_font_14, COL_RED);
    return;
  }

  s_games_col = make_scroll_column(panel);
  lv_obj_set_style_pad_row(s_games_col, 4, 0);
  lv_obj_add_event_cb(s_games_col, games_col_event, LV_EVENT_ALL, NULL);
  lv_obj_clear_flag(s_games_col, LV_OBJ_FLAG_SCROLL_WITH_ARROW);   // GAUCHE/DROITE ne font pas defiler
  build_search_bar(s_groups[TAB_GAMES]);   // enfant 0 de la colonne
  build_list_header(s_groups[TAB_GAMES]);  // "Reprendre" (enfant 1)

  uint32_t fk_, tk_;
  const bool show_where = osd_storage_info(OSD_WHERE_FLASH, &fk_, &tk_);
  int shown = 0;
  for (int i = 0; i < n && i < MAX_GAMES; i++) {
#if LV_MEM_CUSTOM == 0
    // Garde-fou : si le tas LVGL (LV_MEM_SIZE de lv_conf.h) est presque plein,
    // on arrete d'ajouter des lignes plutot que de laisser LVGL bloquer la
    // console (LV_ASSERT_MALLOC). Avec le reglage par defaut (48 Ko), les 50
    // jeux tiennent (mesure au simulateur, LVGL 8.3.11 : ~14 Ko encore libres
    // une fois tous les menus construits - ~10 Ko avant que le sous-titre des
    // lignes ne soit dessine sans label).
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    // 6 Ko gardes en reserve pour les ecrans ouverts plus tard, par-dessus
    // les menus : QR plein format (~3,5 Ko, le plus gourmand), etalonnage
    // (~1,5 Ko), test du tactile (~1,8 Ko), "Lancement..." (mesures au
    // simulateur ; les pixels du QR sont en PSRAM, hors de ce tas). Ces
    // ecrans font beaucoup de PETITES allocations : on regarde donc la
    // memoire libre TOTALE (le plus grand bloc libre, lui, retrecit vite
    // quand la liste est reconstruite apres un envoi par le portail, et
    // aurait vide la liste a tort).
    // Reserve ramenee de 8 a 6 Ko (le pire cas mesure - onglet Portail puis QR
    // plein format - en prend ~3,4 Ko ; il reste ~2,5 Ko de marge), pour
    // garder des jeux affiches avec un petit LV_MEM_SIZE malgre la barre de
    // recherche et le choix du mode Wi-Fi.
    if (mon.free_size < 6144 || mon.free_biggest_size < 2048) {
      Serial.printf("[UI] Tas LVGL presque plein : %d jeu(x) affiche(s) sur %d\n", shown, n);
      break;
    }
#endif
    // Nom affiche : nom du fichier sans l'extension ".nes"
    const char *fname = osd_game_filename(i);
    bool bad = osd_game_is_bad(i);
    if (bad) snprintf(s_game_disp[i], sizeof(s_game_disp[i]), "! %s", fname);
    else snprintf(s_game_disp[i], sizeof(s_game_disp[i]), "%s", fname);
    size_t len = strlen(s_game_disp[i]);
    if (len > 4 && (strcasecmp(s_game_disp[i] + len - 4, ".nes") == 0)) s_game_disp[i][len - 4] = '\0';

    char sz[16];
    format_size(osd_game_size(i), sz, sizeof(sz));
    // Emplacement du jeu (icone + mot), affiche des que la memoire interne
    // existe - la couleur n'est jamais le seul indice.
    bool in_flash = osd_game_where(i) == OSD_WHERE_FLASH;
    char loc[24];
    if (!show_where) loc[0] = '\0';
    else if (in_flash) snprintf(loc, sizeof(loc), LV_SYMBOL_DRIVE " Interne%s", sz[0] ? " \xC2\xB7 " : "");
    else snprintf(loc, sizeof(loc), LV_SYMBOL_SD_CARD " SD%s", sz[0] ? " \xC2\xB7 " : "");
    if (bad) snprintf(s_game_sub[i], sizeof(s_game_sub[i]), "Liste noire \xC2\xB7 %s%s", loc, sz);
    else snprintf(s_game_sub[i], sizeof(s_game_sub[i]), "%s%s", loc, sz);

    lv_obj_t *row = lv_btn_create(s_games_col);
    lv_obj_remove_style_all(row);
    lv_obj_add_style(row, &st_row, 0);
    lv_obj_add_style(row, &st_row_pr, LV_STATE_PRESSED);
    lv_obj_add_style(row, &st_row_focus, LV_STATE_FOCUS_KEY);
    if (in_flash) lv_obj_add_style(row, &st_row_flash, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLL_ON_FOCUS | LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *name = lv_label_create(row);
    lv_obj_add_style(name, &st_row_name, 0);
    if (bad) lv_obj_add_style(name, &st_row_name_bad, 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_label_set_text_static(name, s_game_disp[i]);

    // Sous-titre ("SD - 256 Ko") dessine directement par la colonne
    // (games_draw_subtitles) au lieu d'un label par ligne : ~100 octets de tas
    // LVGL economises par jeu, soit ~5 Ko pour 50 jeux.
    s_game_flags[i] = (bad ? GF_BAD : 0) | (in_flash ? GF_FLASH : 0) | (osd_game_is_fav(i) ? GF_FAV : 0);

    group_add(s_groups[TAB_GAMES], row);
    shown++;
  }
  s_rows_built = shown;
  if (n == 0) {
    // Filtre sans resultat (la barre de recherche reste, avec sa croix)
    // Ou liste des favoris / recents vide
    const int mode = osd_games_mode();
    const char *msg = s_search[0] ? "Aucun jeu ne correspond \xC3\xA0 cette recherche"
                    : mode == OSD_VIEW_FAVS ? "Aucun favori pour l'instant. Appui long sur un jeu, puis " UI_SYMBOL_STAR " : il appara\xC3\xAEtra ici."
                    : mode == OSD_VIEW_RECENTS ? "Aucun jeu lanc\xC3\xA9 r\xC3\xA9" "cemment."
                    : "Aucun jeu";
    lv_obj_t *none = make_label(s_games_col, msg, &ui_font_14, COL_MUT);
    lv_obj_set_width(none, LV_PCT(100));
    lv_label_set_long_mode(none, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(none, LV_TEXT_ALIGN_CENTER, 0);
  }
  if (osd_games_page_count() > 1) build_pager(s_groups[TAB_GAMES]);
  // Rappel discret du geste qui ouvre la fiche d'un jeu
  if (shown > 0) {
#if HAS_PHYSICAL_CONTROLLER
    lv_obj_t *hint = make_label(s_games_col, "Appui long (ou A maintenu) : favori, reprise, suppression", &ui_font_12, COL_MUT);
#else
    lv_obj_t *hint = make_label(s_games_col, "Appui long sur un jeu : favori, reprise, suppression", &ui_font_12, COL_MUT);
#endif
    lv_obj_set_width(hint, LV_PCT(100));
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
  }
  if (shown < n) {
    lv_obj_t *more = make_label(s_games_col, "", &ui_font_12, COL_WARM);
    lv_label_set_text_fmt(more, "%d jeu(x) non affich\xC3\xA9(s) : augmentez LV_MEM_SIZE dans lv_conf.h", n - shown);
    lv_obj_set_width(more, LV_PCT(100));
    lv_label_set_long_mode(more, LV_LABEL_LONG_WRAP);
  }
  // Le filtre de recherche survit a une relecture de la carte (envoi par le
  // portail) ; il met aussi a jour le titre.
  apply_search_filter();
}

// ============================================================================
// Onglet Reglages
// ============================================================================
static void vol_slider_event(lv_event_t *e) {
  lv_obj_t *sl = lv_event_get_target(e);
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_VALUE_CHANGED) {
    osd_set_volume((int)lv_slider_get_value(sl) * 5);
    lv_label_set_text_fmt(s_vol_val, "%d %%", osd_get_volume());
  } else if (code == LV_EVENT_RELEASED) {
    osd_volume_commit();
  }
}

static void bl_slider_event(lv_event_t *e) {
  lv_obj_t *sl = lv_event_get_target(e);
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_VALUE_CHANGED) {
    bl_apply((int)lv_slider_get_value(sl) * 10);
    lv_label_set_text_fmt(s_bl_val, "%d %%", s_bl_pct);
  } else if (code == LV_EVENT_RELEASED) {
    bl_commit();
  }
}

static lv_obj_t *make_slider(lv_obj_t *card, int min, int max, int val, lv_event_cb_t cb, lv_group_t *g) {
  lv_obj_t *sl = lv_slider_create(card);
  lv_obj_set_width(sl, LV_PCT(100));
  lv_obj_set_height(sl, 6);
  lv_slider_set_range(sl, min, max);
  lv_slider_set_value(sl, val, LV_ANIM_OFF);
  // Styles complets (fonctionne aussi avec LV_USE_THEME_DEFAULT 0)
  lv_obj_set_style_bg_color(sl, C(COL_LINE), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(sl, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(sl, C(COL_ACC), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_radius(sl, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(sl, C(COL_KNOB), LV_PART_KNOB);
  lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_KNOB);
  lv_obj_set_style_radius(sl, LV_RADIUS_CIRCLE, LV_PART_KNOB);
  lv_obj_set_style_pad_all(sl, 6, LV_PART_KNOB);
  lv_obj_add_style(sl, &st_focus_key, LV_STATE_FOCUS_KEY);
  lv_obj_set_ext_click_area(sl, 12);    // zone tactile plus large que le trait de 6 px
  lv_obj_add_flag(sl, LV_OBJ_FLAG_SCROLL_ON_FOCUS);   // Voir make_segmented()
  lv_obj_add_event_cb(sl, cb, LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_add_event_cb(sl, cb, LV_EVENT_RELEASED, NULL);
  group_add(g, sl);
  return sl;
}

#if ENABLE_WIFI_PORTAL
// wifi_boot_enable() bloque jusqu'a ~10 s (tentative Station, puis repli
// Point d'acces). Le rappel ne fait qu'afficher "connexion..." et noter
// l'heure ; la boucle principale l'appelle 300 ms plus tard, une fois
// l'interrupteur dessine en position ON (voir ui_lvgl_run_menu()).
static uint32_t s_wifi_enable_at = 0;

// Mode Wi-Fi (Auto / Box / Point d'acces), voir wifi_boot.h
static const char *s_map_wifimode[] = {"Auto", "Box", "Point d'acc\xC3\xA8s", ""};
static lv_obj_t *s_wifi_mode_desc = NULL;

static void wifi_mode_desc_update(int mode) {
  if (!s_wifi_mode_desc) return;
  static const char *d[] = {
    "Ta box d'abord ; si elle ne r\xC3\xA9pond pas en 10 s, la console cr\xC3\xA9" "e son r\xC3\xA9seau " WIFI_AP_SSID ".",
    "Uniquement ta box. Si elle ne r\xC3\xA9pond pas, le Wi-Fi reste coup\xC3\xA9.",
    "La console cr\xC3\xA9" "e directement son r\xC3\xA9seau " WIFI_AP_SSID " (mot de passe " WIFI_AP_PASSWORD "), sans essayer la box.",
  };
  lv_label_set_text(s_wifi_mode_desc, d[mode < 0 || mode > 2 ? 0 : mode]);
}

// Affichage "connexion..." (en-tete, pastille, interrupteur) pendant une
// (re)connexion demandee depuis Reglages : identique a l'etat
// WIFI_BOOT_CONNECTING, que l'ecran ne voit jamais (connexion bloquante).
static void wifi_ui_show_connecting() {
  if (s_wifi_icon) lv_obj_set_style_text_color(s_wifi_icon, C(COL_WARM), 0);
  if (s_wifi_text) lv_label_set_text(s_wifi_text, "connexion\xE2\x80\xA6");
  if (s_wifi_pill) {
    lv_label_set_text(s_wifi_pill, "connexion\xE2\x80\xA6");
    lv_obj_set_style_bg_color(s_wifi_pill, C(COL_WARM), 0);
    lv_obj_set_style_text_color(s_wifi_pill, C(COL_ACC_INK), 0);
  }
  if (s_wifi_detail)
    lv_label_set_text(s_wifi_detail, wifi_boot_get_mode() == NESWIFI_MODE_AP ? "" : "jusqu'\xC3\xA0 ~10 s");
  if (s_wifi_switch) lv_obj_add_state(s_wifi_switch, LV_STATE_CHECKED);
}

static void wifi_mode_event(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
  lv_obj_t *bm = lv_event_get_target(e);
  int mode = NESWIFI_MODE_AUTO;
  for (int i = 0; i < 3; i++) if (lv_btnmatrix_has_btn_ctrl(bm, i, LV_BTNMATRIX_CTRL_CHECKED)) mode = i;
  if (mode == (int)wifi_boot_get_mode()) return;
  wifi_boot_set_mode((wifi_mode_pref_t)mode);
  wifi_mode_desc_update(mode);
  // Wi-Fi allume (ou en echec) : on le relance tout de suite dans le
  // nouveau mode ; coupe : le mode servira a la prochaine activation.
  if (g_wifi_boot_status != WIFI_BOOT_DISABLED) {
    wifi_boot_disable();
    // Le changement d'etat qui vient d'avoir lieu (-> coupe) ne doit pas
    // etre repris par la boucle (update_wifi_ui() afficherait "coupe"
    // pendant toute la reconnexion) : on le marque comme deja vu.
    s_last_wifi_status = (int)g_wifi_boot_status;
    wifi_ui_show_connecting();
    s_wifi_enable_at = millis() + 300;
    if (s_wifi_enable_at == 0) s_wifi_enable_at = 1;
  } else if (s_wifi_enable_at) {
    // Connexion deja programmee (deux choix de suite) : elle utilisera le
    // nouveau mode ; seul le texte "jusqu'a ~10 s" est a remettre a jour.
    wifi_ui_show_connecting();
  }
}

// Reseau de la box (enregistre depuis le portail, ou wifi_config.h)
static lv_obj_t *s_wifi_net = NULL;       // nom du reseau
static lv_obj_t *s_wifi_forget = NULL;    // "Oublier" (reseau enregistre depuis le portail)
static lv_obj_t *s_wifi_mode_bm = NULL;
static volatile bool s_req_wifi_forget = false;

static lv_obj_t *s_wifi_net_row = NULL;
static lv_group_t *s_wifi_group = NULL;

static void wifi_net_update() {
  if (!s_wifi_net) return;
  const bool portal = wifi_boot_sta_from_portal();
  if (portal)
    lv_label_set_text_fmt(s_wifi_net, "Box : %s\n(enregistr\xC3\xA9" "e depuis le portail)", wifi_boot_sta_ssid());
  else
    lv_label_set_text_fmt(s_wifi_net, "Box : %s\nAutre box : portail web, R\xC3\xA9seau Wi-Fi", wifi_boot_sta_ssid());
  if (portal && !s_wifi_forget && s_wifi_net_row) {
    s_wifi_forget = make_button(s_wifi_net_row, "Oublier ce r\xC3\xA9seau", COL_SURF2, COL_TXT);
    lv_obj_set_style_pad_ver(s_wifi_forget, 5, 0);
    // juste sous le nom du reseau (la carte est une colonne)
    lv_obj_move_to_index(s_wifi_forget, (int32_t)lv_obj_get_index(s_wifi_net) + 1);
    lv_obj_add_event_cb(s_wifi_forget, flag_event, LV_EVENT_CLICKED, (void *)&s_req_wifi_forget);
    if (s_wifi_group) group_add(s_wifi_group, s_wifi_forget);
  }
  if (s_wifi_forget) {
    if (portal) lv_obj_clear_flag(s_wifi_forget, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_wifi_forget, LV_OBJ_FLAG_HIDDEN);
  }
  if (s_wifi_mode_bm) lv_btnmatrix_set_btn_ctrl(s_wifi_mode_bm, (uint16_t)wifi_boot_get_mode(), LV_BTNMATRIX_CTRL_CHECKED);
}

// Relance le Wi-Fi (nouveau reseau ou nouveau mode), s'il etait allume.
static void wifi_restart_if_on() {
  if (g_wifi_boot_status == WIFI_BOOT_DISABLED) return;
  wifi_boot_disable();
  s_last_wifi_status = (int)g_wifi_boot_status;
  wifi_ui_show_connecting();
  s_wifi_enable_at = millis() + 300;
  if (s_wifi_enable_at == 0) s_wifi_enable_at = 1;
}

static void wifi_switch_event(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
  lv_obj_t *sw = lv_event_get_target(e);
  if (lv_obj_has_state(sw, LV_STATE_CHECKED)) {
    wifi_ui_show_connecting();
    s_wifi_enable_at = millis() + 300;
    if (s_wifi_enable_at == 0) s_wifi_enable_at = 1;
  } else {
    s_wifi_enable_at = 0;
    wifi_boot_disable();
    update_wifi_ui();
  }
}
#endif

// ============================================================================
// Choix du theme (pastilles de couleur, Reglages > THEME)
// ============================================================================
static lv_obj_t *s_settings_col = NULL;
static lv_obj_t *s_theme_bm = NULL;
static volatile int s_req_theme = -1;       // theme demande (boucle principale)
static bool s_theme_req_keypad = false;     // choisi a la manette ?
static const char *s_theme_map[THEME_COUNT + 1];

// Chaque pastille est dessinee dans les couleurs de SON theme (degrade de
// l'accent) ; celle du theme courant est cerclee et cochee.
static void theme_bm_draw_event(lv_event_t *e) {
  lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
  if (!dsc || dsc->class_p != &lv_btnmatrix_class || dsc->type != LV_BTNMATRIX_DRAW_PART_BTN) return;
  if (dsc->id >= (uint32_t)THEME_COUNT || !dsc->rect_dsc) return;
  const UiTheme *t = &s_themes[dsc->id];
  lv_draw_rect_dsc_t *r = dsc->rect_dsc;
  r->radius = LV_RADIUS_CIRCLE;
  r->bg_opa = LV_OPA_COVER;
  r->bg_color = C(t->acc);
  r->bg_grad.dir = LV_GRAD_DIR_HOR;
  r->bg_grad.stops_count = 2;
  r->bg_grad.stops[0].color = C(t->acc);
  r->bg_grad.stops[0].frac = 0;
  r->bg_grad.stops[1].color = C(t->grad);
  r->bg_grad.stops[1].frac = 255;
  // Theme sombre : bord epais de sa couleur de fond (on voit ses deux
  // teintes). Theme clair : pastille claire cerclee de son accent.
  if (!t->dark) {
    r->bg_color = C(t->surf);
    r->bg_grad.stops[0].color = C(t->surf);
    r->bg_grad.stops[1].color = C(t->bg);
  }
  bool cur = (int)dsc->id == s_theme_id;
  r->border_color = cur ? C(COL_TXT) : C(t->dark ? t->bg : t->acc);
  r->border_width = cur ? 3 : 4;
  r->border_opa = LV_OPA_COVER;
  r->border_side = LV_BORDER_SIDE_FULL;
  if (dsc->label_dsc) dsc->label_dsc->color = C(t->dark ? t->acc_ink : t->acc);
}

static void theme_bm_event(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
  lv_obj_t *bm = lv_event_get_target(e);
  for (int i = 0; i < THEME_COUNT; i++) {
    if (lv_btnmatrix_has_btn_ctrl(bm, i, LV_BTNMATRIX_CTRL_CHECKED) && i != s_theme_id) {
      lv_indev_t *indev = lv_indev_get_act();
      s_theme_req_keypad = indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD;
      s_req_theme = i;   // applique dans la boucle principale (reconstruction)
      return;
    }
  }
}

static void build_theme_card(lv_obj_t *col, lv_group_t *g) {
  make_section(col, "TH\xC3\x88ME");
  lv_obj_t *card = make_card(col);
  make_kv_row(card, "Couleurs", s_th->name, COL_ACC_TXT);
  for (int i = 0; i < THEME_COUNT; i++) s_theme_map[i] = (i == s_theme_id) ? LV_SYMBOL_OK : " ";
  s_theme_map[THEME_COUNT] = "";
  s_theme_bm = make_segmented(card, s_theme_map);
  lv_obj_set_height(s_theme_bm, 44);
  lv_obj_set_style_bg_opa(s_theme_bm, LV_OPA_TRANSP, 0);   // pas de cadre : des pastilles
  lv_obj_set_style_border_width(s_theme_bm, 0, 0);
  lv_obj_set_style_pad_column(s_theme_bm, 8, 0);
  lv_btnmatrix_set_btn_ctrl(s_theme_bm, (uint16_t)s_theme_id, LV_BTNMATRIX_CTRL_CHECKED);
  lv_obj_add_event_cb(s_theme_bm, theme_bm_draw_event, LV_EVENT_DRAW_PART_BEGIN, NULL);
  lv_obj_add_event_cb(s_theme_bm, theme_bm_event, LV_EVENT_VALUE_CHANGED, NULL);
  group_add(g, s_theme_bm);
}

// ============================================================================
// Aide "Commandes" (Reglages) - texte constant (pas de copie dans le tas
// LVGL : lv_label_set_text_static).
// ============================================================================
static const char s_help_game[] =
  "SELECT + START tenus 1 s : menu pause (reprendre, sauvegarder, charger, "
  "volume, favori, changer de jeu, menu de la console)\n"
  "Doigt pos\xC3\xA9 1 s au milieu de l'image : menu pause\n"
  "SELECT + haut / bas : volume\n"
  "SELECT tenu 3 s : retour au menu de la console (la partie est gard\xC3\xA9" "e)";
static const char s_help_menu[] =
#if HAS_PHYSICAL_CONTROLLER
  "Haut / bas : choisir \xC2\xB7 A : jouer \xC2\xB7 A tenu : fiche du jeu\n"
  "Gauche / droite : Tous, Favoris, R\xC3\xA9" "cents \xC2\xB7 B : liste des jeux\n"
  "SELECT : onglet suivant \xC2\xB7 START : sortie vid\xC3\xA9o\n"
#endif
  "Toucher un jeu : jouer \xC2\xB7 appui long : fiche (favori, reprise, suppression)\n"
  "T\xC3\xA9l\xC3\xA9phone : croix, A pour lancer, SELECT pour changer de liste";
static const char s_help_save[] =
  "La partie est gard\xC3\xA9" "e en quittant le jeu : \xC2\xAB Reprendre \xC2\xBB la relance l\xC3\xA0 o\xC3\xB9 "
  "elle s'est arr\xC3\xAAt\xC3\xA9" "e. 3 emplacements de sauvegarde par jeu (menu pause). Les jeux \xC3\xA0 pile "
  "(Zelda...) gardent leur sauvegarde comme la vraie cartouche. Fichiers dans le dossier saves.";

static lv_obj_t *s_help = NULL;
static lv_group_t *s_help_group = NULL;
static volatile bool s_req_help_open = false, s_req_help_close = false;

static void help_block(lv_obj_t *card, const char *title, const char *text) {
  make_label(card, title, &ui_font_14, COL_ACC_TXT);
  lv_obj_t *l = lv_label_create(card);
  lv_obj_set_style_text_font(l, &ui_font_12, 0);
  lv_obj_set_style_text_color(l, C(COL_MUT), 0);
  lv_obj_set_width(l, LV_PCT(100));
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  lv_label_set_text_static(l, text);
}

// Fenetre d'aide (Reglages > Aide), sur lv_layer_top(), detruite a la
// fermeture. Defile au doigt ; a la manette, HAUT/BAS font defiler.
static void help_open() {
  if (s_help) return;
  if (!s_help_group) s_help_group = lv_group_create();
  lv_group_remove_all_objs(s_help_group);
  s_help = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(s_help);
  lv_obj_set_size(s_help, DISPLAY_WIDTH, DISPLAY_HEIGHT);
  lv_obj_set_style_bg_color(s_help, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(s_help, LV_OPA_50, 0);
  lv_obj_clear_flag(s_help, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_help, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_help, flag_event, LV_EVENT_CLICKED, (void *)&s_req_help_close);
  lv_obj_t *card = lv_obj_create(s_help);
  lv_obj_remove_style_all(card);
  lv_obj_add_style(card, &st_card, 0);
  lv_obj_set_style_radius(card, 16, 0);
  lv_obj_set_style_pad_all(card, 12, 0);
  lv_obj_set_style_pad_row(card, 6, 0);
  lv_obj_set_size(card, DISPLAY_WIDTH - 16, DISPLAY_HEIGHT - 16);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_center(card);
  lv_obj_t *head = make_plain(card);
  lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);
  make_label(head, "Commandes", &ui_font_20, COL_TXT);
  lv_obj_t *x = make_button(head, LV_SYMBOL_CLOSE, COL_SURF2, COL_TXT);
  lv_obj_set_size(x, 40, 30);
  lv_obj_align(x, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_add_event_cb(x, flag_event, LV_EVENT_CLICKED, (void *)&s_req_help_close);
  help_block(card, "Pendant le jeu", s_help_game);
  help_block(card, "Dans les menus", s_help_menu);
  help_block(card, "Sauvegardes", s_help_save);
  help_block(card, "Wi-Fi", "Autre box : portail web, volet \xC2\xAB R\xC3\xA9seau Wi-Fi de la box \xC2\xBB (sans "
                          "recompiler). Mise \xC3\xA0 jour du logiciel : portail, Maintenance.");
  group_add(s_help_group, card);   // HAUT/BAS : defilement de la carte
  if (s_touch_indev) lv_indev_wait_release(s_touch_indev);
  if (s_keypad_indev) {
    lv_indev_wait_release(s_keypad_indev);
    s_keypad_arrows = true;
    lv_indev_set_group(s_keypad_indev, s_help_group);
  }
}

static void help_close() {
  if (!s_help) return;
  lv_obj_del(s_help);
  s_help = NULL;
  s_keypad_arrows = false;
  if (s_help_group) lv_group_remove_all_objs(s_help_group);
  if (s_keypad_indev) lv_indev_wait_release(s_keypad_indev);
  show_tab(TAB_SETTINGS);
}

// ============================================================================
// Reglages > Jeux ecartes. Un jeu qui plante au demarrage est range dans le
// dossier "quarantaine" (crash_guard) au lieu d'etre supprime. Cette fenetre
// les liste ; un jeu touche : le remettre dans la liste ou le supprimer.
// Fenetre sur lv_layer_top(), detruite a la fermeture.
// ============================================================================
#define QUAR_ROWS_MAX 12
static lv_obj_t *s_quar = NULL;
static lv_group_t *s_quar_group = NULL;
static lv_obj_t *s_quar_lbl = NULL;          // ligne de la carte des Reglages
static int s_quar_sel = -1;                  // jeu choisi (actions), -1 : liste
static volatile int s_req_quar_pick = -1;
static volatile bool s_req_quar_open = false, s_req_quar_close = false, s_req_quar_back = false;
static volatile bool s_req_quar_restore = false, s_req_quar_delete = false;

static void quar_row_event(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_PRESSED) { s_ignore_click = press_stops_scroll(); return; }
  if (code != LV_EVENT_CLICKED) return;
  if (s_ignore_click) { s_ignore_click = false; return; }
  s_req_quar_pick = (int)(intptr_t)lv_event_get_user_data(e);
}

static void quar_label_update() {
  if (!s_quar_lbl) return;
  const int n = osd_quarantine_count();
  if (n > 0) lv_label_set_text_fmt(s_quar_lbl, LV_SYMBOL_WARNING "  %d jeu%s \xC3\xA9" "cart\xC3\xA9%s (plantage)",
                                   n, n > 1 ? "x" : "", n > 1 ? "s" : "");
  else lv_label_set_text(s_quar_lbl, "Aucun jeu \xC3\xA9" "cart\xC3\xA9");
}

static void quar_build() {
  if (s_quar) lv_obj_del(s_quar);
  if (!s_quar_group) s_quar_group = lv_group_create();
  lv_group_remove_all_objs(s_quar_group);
  s_quar = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(s_quar);
  lv_obj_set_size(s_quar, DISPLAY_WIDTH, DISPLAY_HEIGHT);
  lv_obj_set_style_bg_color(s_quar, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(s_quar, LV_OPA_50, 0);
  lv_obj_clear_flag(s_quar, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_quar, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_quar, flag_event, LV_EVENT_CLICKED, (void *)&s_req_quar_close);

  lv_obj_t *card = lv_obj_create(s_quar);
  lv_obj_remove_style_all(card);
  lv_obj_add_style(card, &st_card, 0);
  lv_obj_set_style_radius(card, 16, 0);
  lv_obj_set_style_pad_all(card, 10, 0);
  lv_obj_set_style_pad_row(card, 6, 0);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_width(card, DISPLAY_WIDTH - 16);
  lv_obj_set_style_max_height(card, DISPLAY_HEIGHT - 16, 0);
  lv_obj_set_height(card, LV_SIZE_CONTENT);
  lv_obj_align(card, LV_ALIGN_BOTTOM_MID, 0, -8);

  const int n = osd_quarantine_count();
  if (s_quar_sel >= 0 && s_quar_sel < n) {
    // Actions sur un jeu
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    char nm[64], sub[64], sz[16];
    game_display_name(osd_quarantine_name(s_quar_sel), nm, sizeof(nm));
    lv_obj_t *t = make_label(card, nm, &ui_font_20, COL_TXT);
    lv_obj_set_width(t, LV_PCT(100));
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_height(t, lv_font_get_line_height(&ui_font_20));
    format_size(osd_quarantine_size(s_quar_sel), sz, sizeof(sz));
    snprintf(sub, sizeof(sub), "%s  \xC2\xB7  %s", osd_quarantine_where(s_quar_sel) == OSD_WHERE_FLASH ? "M\xC3\xA9moire interne" : "Carte SD", sz);
    make_label(card, sub, &ui_font_12, COL_MUT);
    lv_obj_t *w = make_label(card, "Ce jeu a fait red\xC3\xA9marrer la console au lancement. Remis dans la liste, "
                                   "il peut planter de nouveau (il serait alors remis de c\xC3\xB4t\xC3\xA9).", &ui_font_12, COL_MUT);
    lv_obj_set_width(w, LV_PCT(100));
    lv_label_set_long_mode(w, LV_LABEL_LONG_WRAP);
    lv_obj_t *b1 = make_button(card, LV_SYMBOL_UPLOAD "  Remettre dans la liste", COL_ACC, COL_ACC_INK);
    lv_obj_set_size(b1, LV_PCT(100), 38);
    lv_obj_add_event_cb(b1, flag_event, LV_EVENT_CLICKED, (void *)&s_req_quar_restore);
    group_add(s_quar_group, b1);
    lv_obj_t *row = make_plain(card);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_t *b2 = make_button(row, LV_SYMBOL_TRASH "  Supprimer", COL_SURF2, COL_RED);
    lv_obj_set_flex_grow(b2, 1);
    lv_obj_add_event_cb(b2, flag_event, LV_EVENT_CLICKED, (void *)&s_req_quar_delete);
    group_add(s_quar_group, b2);
    lv_obj_t *b3 = make_button(row, "Retour", COL_SURF2, COL_TXT);
    lv_obj_set_flex_grow(b3, 1);
    lv_obj_add_event_cb(b3, flag_event, LV_EVENT_CLICKED, (void *)&s_req_quar_back);
    group_add(s_quar_group, b3);
    s_keypad_lr_nav = true;
  } else {
    s_quar_sel = -1;
    lv_obj_t *head = make_plain(card);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    make_label(head, "Jeux \xC3\xA9" "cart\xC3\xA9s", &ui_font_20, COL_TXT);
    lv_obj_t *x = make_button(head, LV_SYMBOL_CLOSE, COL_SURF2, COL_TXT);
    lv_obj_set_size(x, 40, 30);
    lv_obj_align(x, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(x, flag_event, LV_EVENT_CLICKED, (void *)&s_req_quar_close);
    if (n == 0) {
      make_label(card, "Aucun jeu mis de c\xC3\xB4t\xC3\xA9.", &ui_font_14, COL_MUT);
    }
    for (int i = 0; i < n && i < QUAR_ROWS_MAX; i++) {
      char nm[64];
      game_display_name(osd_quarantine_name(i), nm, sizeof(nm));
      lv_obj_t *r = lv_btn_create(card);
      lv_obj_remove_style_all(r);
      lv_obj_add_style(r, &st_row, 0);
      lv_obj_add_style(r, &st_row_pr, LV_STATE_PRESSED);
      lv_obj_add_style(r, &st_row_focus, LV_STATE_FOCUS_KEY);
      lv_obj_set_height(r, 32);
      lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_add_flag(r, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
      lv_obj_add_event_cb(r, quar_row_event, LV_EVENT_ALL, (void *)(intptr_t)i);
      lv_obj_t *l = lv_label_create(r);
      lv_obj_add_style(l, &st_row_name, 0);
      lv_obj_set_y(l, 6);
      lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
      lv_label_set_text(l, nm);
      group_add(s_quar_group, r);
    }
    if (n > QUAR_ROWS_MAX) {
      lv_obj_t *more = make_label(card, "", &ui_font_12, COL_MUT);
      lv_label_set_text_fmt(more, "+ %d autre(s) : voir le portail web", n - QUAR_ROWS_MAX);
    }
    s_keypad_lr_nav = false;
  }
  if (s_touch_indev) lv_indev_wait_release(s_touch_indev);
  if (s_keypad_indev) {
    lv_indev_wait_release(s_keypad_indev);
    lv_indev_set_group(s_keypad_indev, s_quar_group);
    lv_obj_t *f = lv_group_get_focused(s_quar_group);
    if (f) lv_obj_add_state(f, LV_STATE_FOCUS_KEY);
  }
}

static void quar_close() {
  if (!s_quar) return;
  lv_obj_del(s_quar);
  s_quar = NULL;
  s_quar_sel = -1;
  s_keypad_lr_nav = false;
  if (s_quar_group) lv_group_remove_all_objs(s_quar_group);
  if (s_keypad_indev) lv_indev_wait_release(s_keypad_indev);
  show_tab(TAB_SETTINGS);
}

static void build_settings_panel() {
  lv_obj_t *panel = s_panels[TAB_SETTINGS];
  lv_group_t *g = s_groups[TAB_SETTINGS];
  lv_obj_t *col = make_scroll_column(panel);
  s_settings_col = col;

  make_section(col, "SON ET \xC3\x89" "CRAN");
  lv_obj_t *card = make_card(col);
  char buf[16];
  snprintf(buf, sizeof(buf), "%d %%", osd_get_volume());
  s_vol_val = make_kv_row(card, "Volume", buf, COL_MUT);
  make_slider(card, 0, 40, osd_get_volume() / 5, vol_slider_event, g);
  snprintf(buf, sizeof(buf), "%d %%", s_bl_pct);
  s_bl_val = make_kv_row(card, "Luminosit\xC3\xA9", buf, COL_MUT);
  make_slider(card, BL_MIN_PCT / 10, 10, s_bl_pct / 10, bl_slider_event, g);

  build_theme_card(col, g);

  make_section(col, "WI-FI");
  card = make_card(col);
#if ENABLE_WIFI_PORTAL
  lv_obj_t *row = make_plain(card);
  lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_t *wl = make_label(row, "Wi-Fi activ\xC3\xA9", &ui_font_14, COL_TXT);
  lv_obj_align(wl, LV_ALIGN_LEFT_MID, 0, 0);
  s_wifi_switch = lv_switch_create(row);
  lv_obj_set_size(s_wifi_switch, 44, 24);
  // Styles complets (fonctionne aussi avec LV_USE_THEME_DEFAULT 0)
  lv_obj_set_style_bg_color(s_wifi_switch, C(COL_LINE), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_wifi_switch, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(s_wifi_switch, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_wifi_switch, LV_OPA_TRANSP, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(s_wifi_switch, C(COL_ACC), (lv_style_selector_t)LV_PART_INDICATOR | (lv_style_selector_t)LV_STATE_CHECKED);
  lv_obj_set_style_bg_opa(s_wifi_switch, LV_OPA_COVER, (lv_style_selector_t)LV_PART_INDICATOR | (lv_style_selector_t)LV_STATE_CHECKED);
  lv_obj_set_style_radius(s_wifi_switch, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(s_wifi_switch, C(COL_KNOB), LV_PART_KNOB);
  lv_obj_set_style_bg_opa(s_wifi_switch, LV_OPA_COVER, LV_PART_KNOB);
  lv_obj_set_style_radius(s_wifi_switch, LV_RADIUS_CIRCLE, LV_PART_KNOB);
  lv_obj_set_style_pad_all(s_wifi_switch, -3, LV_PART_KNOB);
  lv_obj_add_style(s_wifi_switch, &st_focus_key, LV_STATE_FOCUS_KEY);
  lv_obj_align(s_wifi_switch, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_add_event_cb(s_wifi_switch, wifi_switch_event, LV_EVENT_VALUE_CHANGED, NULL);
  group_add(g, s_wifi_switch);

  lv_obj_t *row2 = make_plain(card);
  lv_obj_set_size(row2, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row2, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row2, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row2, 8, 0);
  s_wifi_pill = lv_label_create(row2);
  lv_obj_add_style(s_wifi_pill, &st_pill, 0);
  s_wifi_detail = make_label(row2, "", &ui_font_12, COL_MUT);
  lv_label_set_long_mode(s_wifi_detail, LV_LABEL_LONG_DOT);
  lv_obj_set_width(s_wifi_detail, 170);

  // Choix du mode (Auto / Box / Point d'acces)
  make_label(card, "Mode", &ui_font_14, COL_TXT);
  lv_obj_t *bmw = make_segmented(card, s_map_wifimode);
  lv_btnmatrix_set_btn_width(bmw, 2, 2);   // "Point d'acces" plus large
  lv_btnmatrix_set_btn_ctrl(bmw, (uint16_t)wifi_boot_get_mode(), LV_BTNMATRIX_CTRL_CHECKED);
  lv_obj_add_event_cb(bmw, wifi_mode_event, LV_EVENT_VALUE_CHANGED, NULL);
  group_add(g, bmw);
  s_wifi_mode_desc = make_label(card, "", &ui_font_12, COL_MUT);
  lv_obj_set_width(s_wifi_mode_desc, LV_PCT(100));
  lv_label_set_long_mode(s_wifi_mode_desc, LV_LABEL_LONG_WRAP);
  wifi_mode_desc_update((int)wifi_boot_get_mode());
  s_wifi_mode_bm = bmw;
  // Reseau de la box (modifiable depuis le portail, sans recompiler). Un seul
  // label ; "Oublier" n'existe que pour un reseau enregistre depuis le portail
  // (cree au besoin, wifi_net_update()).
  s_wifi_net_row = card;
  s_wifi_net = make_label(card, "", &ui_font_12, COL_MUT);
  lv_obj_set_width(s_wifi_net, LV_PCT(100));
  lv_label_set_long_mode(s_wifi_net, LV_LABEL_LONG_WRAP);
  s_wifi_group = g;
  wifi_net_update();
#if ENABLE_BLE_GAMEPAD
  // Bluetooth arrete pendant le Wi-Fi (bluetooth_controller.h).
  lv_obj_t *btn_note = make_label(card, "Quand le Wi-Fi est allum\xC3\xA9, la manette du t\xC3\xA9l\xC3\xA9phone est "
                                        "coup\xC3\xA9" "e (pas assez de m\xC3\xA9moire pour le Wi-Fi et le "
                                        "Bluetooth ensemble) ; elle revient quand le Wi-Fi est coup\xC3\xA9 "
                                        "ou au lancement d'un jeu.", &ui_font_12, COL_MUT);
  lv_obj_set_width(btn_note, LV_PCT(100));
  lv_label_set_long_mode(btn_note, LV_LABEL_LONG_WRAP);
#endif
#else
  lv_obj_t *nl = make_label(card, "Wi-Fi non compil\xC3\xA9 (ENABLE_WIFI_PORTAL = 0 dans wifi_boot.h)", &ui_font_12, COL_MUT);
  lv_obj_set_width(nl, LV_PCT(100));
  lv_label_set_long_mode(nl, LV_LABEL_LONG_WRAP);
#endif

  // Troisieme volet : etalonnage et test de l'ecran tactile
  make_section(col, "\xC3\x89" "CRAN TACTILE");
  card = make_card(col);
  if (g_touch_available) {
    lv_obj_t *th = make_label(card,
      "Si vos appuis tombent \xC3\xA0 c\xC3\xB4t\xC3\xA9 (curseurs, boutons), r\xC3\xA9\xC3\xA9talonnez : "
      "3 croix \xC3\xA0 toucher, puis une v\xC3\xA9rification.", &ui_font_12, COL_MUT);
    lv_obj_set_width(th, LV_PCT(100));
    lv_label_set_long_mode(th, LV_LABEL_LONG_WRAP);
    lv_obj_t *brow = make_plain(card);
    lv_obj_set_size(brow, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(brow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(brow, 8, 0);
    lv_obj_t *bcal = make_button(brow, LV_SYMBOL_REFRESH " R\xC3\xA9\xC3\xA9talonner", COL_ACC, COL_ACC_INK);
    lv_obj_set_flex_grow(bcal, 3);
    lv_obj_add_event_cb(bcal, flag_event, LV_EVENT_CLICKED, (void *)&s_req_calibrate);
    group_add(g, bcal);
    lv_obj_t *btest = make_button(brow, LV_SYMBOL_EYE_OPEN " Tester", COL_SURF2, COL_TXT);
    lv_obj_set_flex_grow(btest, 2);
    lv_obj_add_event_cb(btest, flag_event, LV_EVENT_CLICKED, (void *)&s_req_touch_test);
    group_add(g, btest);
  } else {
    lv_obj_t *nt = make_label(card, "Tactile non d\xC3\xA9tect\xC3\xA9 au d\xC3\xA9marrage.", &ui_font_12, COL_WARM);
    lv_obj_set_width(nt, LV_PCT(100));
  }

  make_section(col, "\xC3\x80 PROPOS");
  card = make_card(col);
  // Aide et jeux ecartes : un bouton chacun, fenetres construites a
  // l'ouverture (le tas LVGL sert d'abord a la liste des jeux)
  lv_obj_t *bh = make_button(card, LV_SYMBOL_LIST "  Commandes et sauvegardes", COL_SURF2, COL_TXT);
  lv_obj_set_width(bh, LV_PCT(100));
  lv_obj_add_event_cb(bh, flag_event, LV_EVENT_CLICKED, (void *)&s_req_help_open);
  group_add(g, bh);
  if (osd_quarantine_refresh() > 0) {   // seulement s'il y en a a l'ouverture des menus
    lv_obj_t *bq = make_button(card, "", COL_SURF2, COL_WARM);
    lv_obj_set_width(bq, LV_PCT(100));
    s_quar_lbl = lv_obj_get_child(bq, 0);
    quar_label_update();
    lv_obj_add_event_cb(bq, flag_event, LV_EVENT_CLICKED, (void *)&s_req_quar_open);
    group_add(g, bq);
  }
  make_kv_row(card, "Version", NES_EMULATOR_VERSION_SHORT, COL_MUT);
  make_kv_row(card, "Portail", NES_WIFI_VERSION_SHORT, COL_MUT);
  snprintf(buf, sizeof(buf), "%d", osd_games_total());
  s_about_games = make_kv_row(card, "Jeux", buf, COL_MUT);
  // Espace libre des deux emplacements de jeux - UNE ligne de deux lignes de
  // texte (3 objets au lieu de 6 : le tas LVGL sert d'abord a la liste des
  // jeux). Valeur de la memoire interne en violet (recoloriage).
  s_about_storage = make_kv_row(card, LV_SYMBOL_SD_CARD "  Carte SD\n" LV_SYMBOL_DRIVE "  M\xC3\xA9moire interne", "", COL_MUT);
  lv_obj_set_style_text_align(s_about_storage, LV_TEXT_ALIGN_RIGHT, 0);
  lv_label_set_recolor(s_about_storage, true);
  lv_obj_set_style_text_line_space(lv_obj_get_parent(s_about_storage), 6, 0);   // herite par les 2 textes
  update_storage_labels();
  // Si la memoire interne est indisponible, la raison exacte et les partitions
  // reellement presentes dans la flash (diagnostic).
  const char *diag = osd_flash_diag();
  if (diag && diag[0]) {
    lv_obj_t *dl = make_label(card, diag, &ui_font_12, COL_WARM);
    lv_obj_set_width(dl, LV_PCT(100));
    lv_label_set_long_mode(dl, LV_LABEL_LONG_WRAP);
  }
}

// ============================================================================
// Onglet Video
// ============================================================================
static const char *s_map_output[] = {"\xC3\x89" "cran LCD", "T\xC3\xA9l\xC3\xA9viseur", ""};
// PAL 60 Hz en plus (couleur PAL, 60 images/s comme les jeux NES).
static const char *s_map_norm[] = {"NTSC 60", "PAL 50", "PAL 60", ""};
static uint8_t s_tv_norm = TVLINK_NORM_PAL;
// Image entrelacee ou progressive (comme une vraie NES).
static const char *s_map_scan[] = {"Entrelac\xC3\xA9" "e", "Progressive", ""};
static bool s_tv_progressive = false;

// On relit l'etat COCHE des boutons (ce que l'ecran montre), pas le
// bouton "selectionne" : les deux restent ainsi toujours d'accord.
static void output_event(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
  lv_obj_t *bm = lv_event_get_target(e);
  g_display_tv_mode = lv_btnmatrix_has_btn_ctrl(bm, 1, LV_BTNMATRIX_CTRL_CHECKED) && video_link_is_ready();
  video_link_remember_tv_mode(g_display_tv_mode);   // Garde apres la fin de partie
  if (g_display_tv_mode) video_link_tick(false);    // la TV affiche aussitot "Console connectee"
  Serial.printf("[UI] Sortie : %s\n", g_display_tv_mode ? "TELEVISEUR" : "ECRAN LCD");
}

static const char *norm_name(uint8_t n) {
  return n == TVLINK_NORM_NTSC ? "NTSC 60 Hz" : n == TVLINK_NORM_PAL60 ? "PAL 60 Hz" : "PAL 50 Hz";
}

static lv_obj_t *s_norm_desc = NULL;
static void norm_desc_update() {
  if (!s_norm_desc) return;
  static const char *d[] = {
    "NTSC 60 Hz : couleurs NTSC, 60 images/s. Les 8 colonnes de chaque bord ne sont pas montr\xC3\xA9" "es "
    "(image nette, 3 points du signal par pixel).",
    "PAL 50 Hz : la norme des t\xC3\xA9l\xC3\xA9viseurs fran\xC3\xA7" "ais. Les jeux NES tournent \xC3\xA0 60 images/s : "
    "une sur six n'est pas montr\xC3\xA9" "e (petites saccades).",
    "PAL 60 Hz : couleurs PAL et 60 images/s, comme les jeux NES : le plus net et le plus fluide. "
    "Si l'\xC3\xA9" "cran reste noir, la t\xC3\xA9l\xC3\xA9 ou le convertisseur ne l'accepte pas : reviens sur PAL 50.",
  };
  lv_label_set_text(s_norm_desc, d[s_tv_norm <= TVLINK_NORM_MAX ? s_tv_norm : TVLINK_NORM_PAL]);
}

static void norm_event(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
  lv_obj_t *bm = lv_event_get_target(e);
  for (int i = 0; i <= TVLINK_NORM_MAX; i++)
    if (lv_btnmatrix_has_btn_ctrl(bm, i, LV_BTNMATRIX_CTRL_CHECKED)) s_tv_norm = (uint8_t)i;
  g_video_pal_mode = s_tv_norm != TVLINK_NORM_NTSC;
  video_link_set_video_norm(s_tv_norm);              // sans effet si liaison inactive
  Preferences p;                                     // Norme memorisee
  if (p.begin(UI_NVS_NAMESPACE, false)) {
    p.putUChar("norm", s_tv_norm);
    p.putBool("pal", g_video_pal_mode);
    p.end();
  }
  norm_desc_update();
  Serial.printf("[UI] Norme : %s\n", norm_name(s_tv_norm));
}

// Image progressive ou entrelacee (memorisee, envoyee a la carte video).
static void scan_event(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
  lv_obj_t *bm = lv_event_get_target(e);
  s_tv_progressive = lv_btnmatrix_has_btn_ctrl(bm, 1, LV_BTNMATRIX_CTRL_CHECKED);
  video_link_set_progressive(s_tv_progressive);
  Preferences p;
  if (p.begin(UI_NVS_NAMESPACE, false)) {
    p.putBool("prog", s_tv_progressive);
    p.end();
  }
  Serial.printf("[UI] Image TV : %s\n", s_tv_progressive ? "progressive" : "entrelacee");
}

// Norme video relue au demarrage (PAL par defaut, televiseurs francais).
// Et le type d'image (entrelacee par defaut).
static void video_norm_load() {
  Preferences p;
  if (p.begin(UI_NVS_NAMESPACE, true)) {
    g_video_pal_mode = p.getBool("pal", true);
    s_tv_norm = p.getUChar("norm", g_video_pal_mode ? TVLINK_NORM_PAL : TVLINK_NORM_NTSC);
    if (s_tv_norm > TVLINK_NORM_MAX) s_tv_norm = TVLINK_NORM_PAL;
    s_tv_progressive = p.getBool("prog", false);
    p.end();
  }
  g_video_pal_mode = s_tv_norm != TVLINK_NORM_NTSC;
  video_link_set_progressive(s_tv_progressive);
  video_link_set_video_norm(s_tv_norm);
  Serial.printf("[UI] Norme TV : %s, image %s\n", norm_name(s_tv_norm),
                s_tv_progressive ? "progressive" : "entrelacee");
}

static void build_video_panel() {
  lv_obj_t *panel = s_panels[TAB_VIDEO];
  lv_group_t *g = s_groups[TAB_VIDEO];
  lv_obj_t *col = make_scroll_column(panel);
  bool link_ok = video_link_is_ready();
  if (!link_ok) g_display_tv_mode = false;   // jamais d'ecran fige sans coprocesseur

  make_section(col, "IMAGE DU JEU");
  s_bm_output = make_segmented(col, s_map_output);
  lv_btnmatrix_set_btn_ctrl(s_bm_output, g_display_tv_mode ? 1 : 0, LV_BTNMATRIX_CTRL_CHECKED);
  if (!link_ok) lv_btnmatrix_set_btn_ctrl(s_bm_output, 1, LV_BTNMATRIX_CTRL_DISABLED);
  lv_obj_add_event_cb(s_bm_output, output_event, LV_EVENT_VALUE_CHANGED, NULL);
  group_add(g, s_bm_output);

  make_section(col, "NORME (T\xC3\x89L\xC3\x89VISEUR)");
  s_bm_norm = make_segmented(col, s_map_norm);
  lv_btnmatrix_set_btn_ctrl(s_bm_norm, s_tv_norm, LV_BTNMATRIX_CTRL_CHECKED);
  lv_obj_add_event_cb(s_bm_norm, norm_event, LV_EVENT_VALUE_CHANGED, NULL);
  group_add(g, s_bm_norm);
  s_norm_desc = make_label(col, "", &ui_font_12, COL_MUT);
  lv_obj_set_width(s_norm_desc, LV_PCT(100));
  lv_label_set_long_mode(s_norm_desc, LV_LABEL_LONG_WRAP);
  norm_desc_update();

  // Image entrelacee ou progressive
  make_section(col, "IMAGE (T\xC3\x89L\xC3\x89VISEUR)");
  lv_obj_t *bm_scan = make_segmented(col, s_map_scan);
  lv_btnmatrix_set_btn_ctrl(bm_scan, s_tv_progressive ? 1 : 0, LV_BTNMATRIX_CTRL_CHECKED);
  lv_obj_add_event_cb(bm_scan, scan_event, LV_EVENT_VALUE_CHANGED, NULL);
  group_add(g, bm_scan);
  lv_obj_t *scan_note = make_label(col, "Progressive : image nette et stable, comme une vraie NES. "
                                        "Si la t\xC3\xA9l\xC3\xA9 ou le convertisseur n'affiche plus rien, "
                                        "reviens sur Entrelac\xC3\xA9" "e.", &ui_font_12, COL_MUT);
  lv_obj_set_width(scan_note, LV_PCT(100));
  lv_label_set_long_mode(scan_note, LV_LABEL_LONG_WRAP);

  lv_obj_t *card = make_card(col);
  lv_obj_set_style_pad_all(card, 8, 0);
  lv_obj_t *info = make_label(card, "", &ui_font_12, link_ok ? COL_TXT : COL_WARM);
  lv_obj_set_width(info, LV_PCT(100));
  lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
  if (link_ok) {
    lv_label_set_text(info,
      "Liaison SPI pr\xC3\xAA" "te. La carte vid\xC3\xA9o affiche \xC2\xAB Console connect\xC3\xA9" "e \xC2\xBB "
      "d\xC3\xA8s que T\xC3\xA9l\xC3\xA9viseur est choisi. Si elle reste sur \xC2\xAB En attente \xC2\xBB, "
      "v\xC3\xA9rifiez les 3 fils et la masse commune.");
  } else {
#if ENABLE_VIDEO_LINK
    lv_label_set_text(info,
      "Liaison vid\xC3\xA9o non d\xC3\xA9marr\xC3\xA9" "e (voir le port s\xC3\xA9rie, lignes [VIDEO-LINK]) : "
      "la sortie T\xC3\xA9l\xC3\xA9viseur n'est pas disponible.");
#else
    lv_label_set_text(info,
      "Liaison vid\xC3\xA9o d\xC3\xA9sactiv\xC3\xA9" "e (ENABLE_VIDEO_LINK = 0 dans video_link.h) : "
      "la sortie T\xC3\xA9l\xC3\xA9viseur n'est pas disponible.");
#endif
  }
  lv_obj_t *note = make_label(card, "\xC3\x89" "cran LCD \xC3\xA0 chaque mise sous tension ; T\xC3\xA9l\xC3\xA9viseur conserv\xC3\xA9 "
                              "apr\xC3\xA8s une partie. Norme et image m\xC3\xA9moris\xC3\xA9" "es. "
                              "Carte vid\xC3\xA9o v3.0 n\xC3\xA9" "cessaire (couleurs exactes).", &ui_font_12, COL_MUT);
  lv_obj_set_width(note, LV_PCT(100));
  lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
}

// ============================================================================
// Onglet Portail
// ============================================================================
// Voir s_ap_info (etiquette detruite -> plus de mise a jour).
static void ap_info_deleted_cb(lv_event_t *e) {
  if (lv_event_get_target(e) == s_ap_info) {
    s_ap_info = NULL;
    s_ap_info_key = -1;
  }
}

static void portal_refresh() {
  lv_obj_t *panel = s_panels[TAB_PORTAL];
  lv_group_remove_all_objs(s_groups[TAB_PORTAL]);
  lv_obj_clean(panel);
  s_ap_info = NULL;        // Detruit avec le panneau
  s_ap_info_key = -1;

#if !ENABLE_WIFI_PORTAL
  lv_obj_t *l = make_label(panel, "Portail non compil\xC3\xA9 (ENABLE_WIFI_PORTAL = 0 dans wifi_boot.h).", &ui_font_14, COL_MUT);
  lv_obj_set_width(l, 280);
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  lv_obj_center(l);
#else
  bool station = (g_wifi_boot_status == WIFI_BOOT_STATION_OK);
  bool ap = (g_wifi_boot_status == WIFI_BOOT_AP_FALLBACK);

  if (!station && !ap) {
    lv_obj_t *box = make_plain(panel);
    lv_obj_set_size(box, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 8, 0);
    make_label(box, LV_SYMBOL_WIFI, &ui_font_20, COL_DIM);
    const char *msg = (g_wifi_boot_status == WIFI_BOOT_CONNECTING)
                        ? "Connexion Wi-Fi en cours\xE2\x80\xA6"
                        : "Wi-Fi coup\xC3\xA9 : le portail est indisponible.";
    lv_obj_t *l = make_label(box, msg, &ui_font_14, COL_TXT);
    lv_obj_set_width(l, 280);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *btn = lv_btn_create(box);
    lv_obj_set_style_bg_color(btn, C(COL_SURF2), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_pad_hor(btn, 14, 0);
    lv_obj_set_style_pad_ver(btn, 8, 0);
    lv_obj_add_style(btn, &st_focus_key, LV_STATE_FOCUS_KEY);
    lv_obj_add_event_cb(btn, goto_settings_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = make_label(btn, LV_SYMBOL_SETTINGS "  Ouvrir les R\xC3\xA9glages", &ui_font_14, COL_TXT);
    lv_obj_center(bl);
    group_add(s_groups[TAB_PORTAL], btn);
    return;
  }

  static char url[48];
  if (station) snprintf(url, sizeof(url), "http://%s/", g_wifi_boot_status_text);
  else snprintf(url, sizeof(url), "http://192.168.4.1/");

  lv_obj_t *wrap = make_plain(panel);
  lv_obj_set_size(wrap, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_pad_all(wrap, 10, 0);

  // QR code de l'adresse (fond blanc obligatoire pour la lecture). Le
  // toucher ouvre l'ecran QR plein format.
  lv_obj_t *qbg = lv_obj_create(wrap);
  lv_obj_remove_style_all(qbg);
  lv_obj_set_style_bg_color(qbg, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(qbg, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(qbg, 6, 0);
  lv_obj_set_size(qbg, 124, 124);
  lv_obj_align(qbg, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_clear_flag(qbg, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(qbg, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_style(qbg, &st_focus_key, LV_STATE_FOCUS_KEY);
  lv_obj_add_event_cb(qbg, flag_event, LV_EVENT_CLICKED, (void *)&s_req_qr_full);
  group_add(s_groups[TAB_PORTAL], qbg);
  lv_obj_t *qr = make_qr_image(qbg, url, 116);
  if (qr) lv_obj_center(qr);
  const int text_x = 136;

  lv_obj_t *txt = make_plain(wrap);
  lv_obj_set_size(txt, DISPLAY_WIDTH - 20 - text_x, LV_SIZE_CONTENT);
  lv_obj_align(txt, LV_ALIGN_LEFT_MID, text_x, 0);
  lv_obj_set_flex_flow(txt, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(txt, 4, 0);

  if (ap) {
    make_label(txt, "R\xC3\xA9seau Wi-Fi :", &ui_font_12, COL_MUT);
    lv_obj_t *ss = make_label(txt, g_wifi_boot_status_text, &ui_font_14, COL_CYAN);
    lv_obj_set_width(ss, LV_PCT(100));
    lv_label_set_long_mode(ss, LV_LABEL_LONG_WRAP);
    make_label(txt, "puis ouvrez :", &ui_font_12, COL_MUT);
  } else {
    lv_obj_t *sc = make_label(txt, "Scannez avec le t\xC3\xA9l\xC3\xA9phone", &ui_font_14, COL_TXT);
    lv_obj_set_width(sc, LV_PCT(100));
    lv_label_set_long_mode(sc, LV_LABEL_LONG_WRAP);
    make_label(txt, "ou ouvrez :", &ui_font_12, COL_MUT);
  }
  lv_obj_t *u = make_label(txt, url, &ui_font_14, COL_ACC_TXT);
  lv_obj_set_width(u, LV_PCT(100));
  lv_label_set_long_mode(u, LV_LABEL_LONG_WRAP);
  if (!ap) {
    lv_obj_t *h = make_label(txt, "Puis d\xC3\xA9posez vos jeux dans la page.", &ui_font_12, COL_MUT);
    lv_obj_set_width(h, LV_PCT(100));
    lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
  }
  lv_obj_t *z = make_label(txt, ap ? LV_SYMBOL_PLUS " Touchez le QR : il connecte aussi le t\xC3\xA9l\xC3\xA9phone au r\xC3\xA9seau."
                                   : LV_SYMBOL_PLUS " Touchez le QR pour l'agrandir.", &ui_font_12, COL_TXT);
  lv_obj_set_width(z, LV_PCT(100));
  lv_label_set_long_mode(z, LV_LABEL_LONG_WRAP);
  if (ap) {
    // Tenu a jour par portal_ap_info_update() (boucle des menus, deux fois par
    // seconde). Pointeur remis a NULL a la destruction de l'etiquette, quelle
    // qu'en soit la cause (onglet reconstruit, ecran entier detruit par un
    // changement de theme...).
    s_ap_info = make_label(txt, "", &ui_font_12, COL_MUT);
    lv_obj_set_width(s_ap_info, LV_PCT(100));
    lv_label_set_long_mode(s_ap_info, LV_LABEL_LONG_WRAP);
    lv_obj_add_event_cb(s_ap_info, ap_info_deleted_cb, LV_EVENT_DELETE, NULL);
    portal_ap_info_update();
  }
#endif
}

// Diagnostic du point d'acces sur une ligne : appareils relies au reseau
// NES-WiFi (un appareil absent du compte n'est pas sur ce reseau) et
// requetes recues par la page (relie mais 0 requete : ses demandes partent
// ailleurs, souvent par les donnees mobiles du telephone). Redessinee
// seulement quand un des deux nombres change.
static void portal_ap_info_update() {
#if ENABLE_WIFI_PORTAL
  if (!s_ap_info) return;
  int n = wifi_boot_ap_clients();
  if (n > 9) n = 9;
  uint32_t r = wifi_portal_request_count();
  if (r > 999) r = 1000;                 // affiche "999+" (une seule ligne)
  int key = (int)r * 10 + n;
  if (key == s_ap_info_key) return;
  s_ap_info_key = key;
  char buf[64];
  if (r > 999) snprintf(buf, sizeof(buf), "Reli\xC3\xA9s : %d \xC2\xB7 requ\xC3\xAAtes : 999+", n);
  else snprintf(buf, sizeof(buf), "Reli\xC3\xA9s : %d \xC2\xB7 requ\xC3\xAAtes : %lu", n, (unsigned long)r);
  lv_label_set_text(s_ap_info, buf);
#endif
}

// ============================================================================
// Barre du haut (titre + etat Wi-Fi) et etat Wi-Fi dans Reglages
// ============================================================================
static void update_wifi_ui() {
#if ENABLE_WIFI_PORTAL
  uint32_t icon_col = COL_DIM, pill_col = COL_DIM;
  const char *pill = "d\xC3\xA9sactiv\xC3\xA9";
  const char *top = "Wi-Fi coup\xC3\xA9";
  const char *detail = "";
  bool on = false;
  switch (g_wifi_boot_status) {
    case WIFI_BOOT_STATION_OK:
      icon_col = COL_ACC_TXT; pill_col = COL_ACC; pill = "connect\xC3\xA9";
      top = g_wifi_boot_status_text; detail = g_wifi_boot_status_text; on = true;
      break;
    case WIFI_BOOT_AP_FALLBACK:
      icon_col = COL_CYAN; pill_col = COL_CYAN; pill = "point d'acc\xC3\xA8s";
      top = g_wifi_boot_status_text; detail = g_wifi_boot_status_text; on = true;
      break;
    case WIFI_BOOT_CONNECTING:
      icon_col = COL_WARM; pill_col = COL_WARM; pill = "connexion\xE2\x80\xA6";
      top = "connexion\xE2\x80\xA6"; on = true;
      break;
    case WIFI_BOOT_FAILED:
      icon_col = COL_RED; pill_col = COL_RED; pill = "indisponible";
      top = "Wi-Fi indisponible";
      detail = g_wifi_boot_status_text;   // "box injoignable"...
      break;
    default:
      break;
  }
  if (s_wifi_icon) lv_obj_set_style_text_color(s_wifi_icon, C(icon_col), 0);
  if (s_wifi_text) lv_label_set_text(s_wifi_text, top);
  if (s_wifi_pill) {
    lv_label_set_text(s_wifi_pill, pill);
    lv_obj_set_style_bg_color(s_wifi_pill, C(pill_col), 0);
    lv_obj_set_style_text_color(s_wifi_pill, C(pill_col == COL_DIM ? COL_TXT : COL_ACC_INK), 0);
  }
  if (s_wifi_detail) lv_label_set_text(s_wifi_detail, detail);
  if (s_wifi_switch) {
    if (on) lv_obj_add_state(s_wifi_switch, LV_STATE_CHECKED);
    else lv_obj_clear_state(s_wifi_switch, LV_STATE_CHECKED);
  }
  s_last_wifi_status = (int)g_wifi_boot_status;
  if (s_tab == TAB_PORTAL) portal_refresh();
#else
  if (s_wifi_icon) lv_obj_add_flag(s_wifi_icon, LV_OBJ_FLAG_HIDDEN);
#endif
}

// Icone Bluetooth de la barre du haut (manette du telephone), redessinee
// seulement quand l'etat de la liaison change.
// Troisieme etat, Bluetooth en pause pendant le point d'acces Wi-Fi (icone
// couleur "attention", orange) : le telephone ne peut pas se connecter avant
// le debut de la partie (voir bluetooth_controller.h). Pas d'icone barree : a
// cette taille, le trait se confond avec le symbole.
static void update_bt_ui() {
  if (!s_bt_icon) return;
  int st = bluetooth_controller_is_connected() ? 2 : (bluetooth_controller_waiting_for_wifi() ? 1 : 0);
  if (st == s_last_bt) return;
  s_last_bt = st;
  lv_obj_set_style_text_color(s_bt_icon, C(st == 2 ? COL_ACC_TXT : st == 1 ? COL_WARM : COL_DIM), 0);
  Serial.printf("[UI] Telephone %s\n", st == 2 ? "connecte (icone Bluetooth allumee)"
                                   : st == 1 ? "en attente : Bluetooth en pause pendant le point d'acces (icone orange)"
                                             : "deconnecte (icone grisee)");
}

// ============================================================================
// Onglets
// ============================================================================
static void nav_event(lv_event_t *e) {
  if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
    show_tab((int)(intptr_t)lv_event_get_user_data(e));
  }
}

static void toast_hide();

static void show_tab(int tab) {
  if (tab < 0 || tab >= TAB_COUNT) tab = TAB_GAMES;
  if (tab != TAB_GAMES) toast_hide();   // Le message concerne la liste
  if (s_tab == TAB_SETTINGS && tab != TAB_SETTINGS) {
    osd_volume_commit();     // au cas ou le curseur a ete bouge a la manette
    bl_commit();
  }
  s_tab = tab;
  for (int i = 0; i < TAB_COUNT; i++) {
    if (i == tab) {
      lv_obj_clear_flag(s_panels[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_state(s_nav_btns[i], LV_STATE_CHECKED);
    } else {
      lv_obj_add_flag(s_panels[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_state(s_nav_btns[i], LV_STATE_CHECKED);
    }
  }
  if (tab == TAB_PORTAL) portal_refresh();
  if (s_keypad_indev) {
    lv_indev_set_group(s_keypad_indev, s_groups[tab]);
    // Les onglets sont construits caches : LVGL n'y donne le focus a
    // personne. Sans element focalise, toutes les touches seraient ignorees.
    if (lv_group_get_focused(s_groups[tab]) == NULL) lv_group_focus_next(s_groups[tab]);
  }
  update_title();
}

static void build_ui() {
  s_scr = lv_obj_create(NULL);
  lv_obj_remove_style_all(s_scr);
  lv_obj_set_style_bg_color(s_scr, C(COL_BG), 0);
  lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
  lv_obj_set_style_text_font(s_scr, &ui_font_14, 0);
  lv_obj_set_style_text_color(s_scr, C(COL_TXT), 0);
  lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

  // Barre du haut
  lv_obj_t *top = make_plain(s_scr);
  lv_obj_set_size(top, DISPLAY_WIDTH, TOPBAR_H);
  lv_obj_set_pos(top, 0, 0);
  lv_obj_set_style_bg_color(top, C(COL_SURF), 0);
  lv_obj_set_style_bg_opa(top, LV_OPA_COVER, 0);
  s_title = make_label(top, "", &ui_font_20, COL_TXT);   // Titre plus grand
  lv_obj_align(s_title, LV_ALIGN_LEFT_MID, 12, 1);
  s_wifi_icon = make_label(top, LV_SYMBOL_WIFI, &ui_font_14, COL_DIM);
  lv_obj_align(s_wifi_icon, LV_ALIGN_RIGHT_MID, -10, 0);
  s_wifi_text = make_label(top, "", &ui_font_12, COL_MUT);
  lv_label_set_long_mode(s_wifi_text, LV_LABEL_LONG_DOT);
  lv_obj_set_width(s_wifi_text, 118);
  lv_obj_set_style_text_align(s_wifi_text, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_align(s_wifi_text, LV_ALIGN_RIGHT_MID, -34, 0);
#if ENABLE_BLE_GAMEPAD
  // Icone Bluetooth entre le titre et l'etat du Wi-Fi - couleur d'accent quand
  // le telephone est connecte, grisee sinon (update_bt_ui()).
  s_bt_icon = make_label(top, LV_SYMBOL_BLUETOOTH, &ui_font_14, COL_DIM);
  lv_obj_align(s_bt_icon, LV_ALIGN_RIGHT_MID, -34 - 118 - 6, 0);
  s_last_bt = -1;
#endif

  // Onglets (un conteneur par onglet, un seul visible a la fois)
  for (int i = 0; i < TAB_COUNT; i++) {
    s_groups[i] = lv_group_create();
    s_panels[i] = make_plain(s_scr);
    lv_obj_set_size(s_panels[i], DISPLAY_WIDTH, CONTENT_H);
    lv_obj_set_pos(s_panels[i], 0, CONTENT_Y);
    lv_obj_add_flag(s_panels[i], LV_OBJ_FLAG_HIDDEN);
  }

  // Barre de navigation
  static const char *nav_txt[TAB_COUNT] = {
    LV_SYMBOL_LIST "\nJeux",
    LV_SYMBOL_SETTINGS "\nR\xC3\xA9glages",
    LV_SYMBOL_VIDEO "\nVid\xC3\xA9o",
    LV_SYMBOL_WIFI "\nPortail",
  };
  // Barre continue, onglet actif en "pastille" arrondie
  lv_obj_t *navbar = make_plain(s_scr);
  lv_obj_set_size(navbar, DISPLAY_WIDTH, NAV_H);
  lv_obj_set_pos(navbar, 0, DISPLAY_HEIGHT - NAV_H);
  lv_obj_set_style_bg_color(navbar, C(COL_SURF), 0);
  lv_obj_set_style_bg_opa(navbar, LV_OPA_COVER, 0);
  const int nav_w = DISPLAY_WIDTH / TAB_COUNT;
  for (int i = 0; i < TAB_COUNT; i++) {
    lv_obj_t *b = lv_btn_create(navbar);
    lv_obj_remove_style_all(b);
    lv_obj_add_style(b, &st_nav, 0);
    lv_obj_add_style(b, &st_nav_on, LV_STATE_CHECKED);
    lv_obj_add_style(b, &st_nav_pr, LV_STATE_PRESSED);
    lv_obj_set_size(b, nav_w - 8, NAV_H - 2);
    lv_obj_set_pos(b, i * nav_w + 4, 1);
    lv_obj_set_ext_click_area(b, 4);   // pas de trou entre deux onglets
    lv_obj_add_event_cb(b, nav_event, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text_static(l, nav_txt[i]);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(l, 0, 0);
    lv_obj_center(l);
    s_nav_btns[i] = b;
  }

  // Ordre voulu : les onglets de taille fixe d'abord, la liste des jeux en
  // dernier - elle s'arrete d'elle-meme si le tas LVGL vient a manquer
  // (voir build_games_panel()), sans priver les autres onglets.
  build_settings_panel();
  build_video_panel();
  build_games_panel();
  // Portail : construit a l'affichage de l'onglet (depend de l'etat Wi-Fi)

  update_wifi_ui();
  lv_scr_load(s_scr);
  show_tab(TAB_GAMES);
}

// Ecran d'attente simple (chargement de la liste, lancement d'un jeu)
static lv_obj_t *show_wait_screen(const char *line1, const char *line2) {
  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_remove_style_all(scr);
  lv_obj_set_style_bg_color(scr, C(COL_BG), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_t *box = make_plain(scr);
  lv_obj_set_size(box, LV_PCT(100), LV_PCT(100));
  lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(box, 10, 0);
#if LV_USE_SPINNER
  lv_obj_t *sp = lv_spinner_create(box, 1000, 60);
  lv_obj_set_size(sp, 40, 40);
  lv_obj_set_style_arc_color(sp, C(COL_ACC), LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(sp, C(COL_LINE), LV_PART_MAIN);
  lv_obj_set_style_arc_width(sp, 4, LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(sp, 4, LV_PART_MAIN);
#endif
  make_label(box, line1, &ui_font_20, COL_TXT);
  if (line2 && line2[0]) {
    lv_obj_t *l2 = make_label(box, line2, &ui_font_14, COL_MUT);
    lv_obj_set_width(l2, 290);
    lv_label_set_long_mode(l2, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(l2, LV_TEXT_ALIGN_CENTER, 0);
  }
  lv_scr_load(scr);
  lv_refr_now(NULL);
  return scr;
}

// ============================================================================
// Ecran QR plein format
//   - Wi-Fi de la box : une page, l'adresse du portail.
//   - Point d'acces : page 1 "rejoindre le reseau NES-WiFi" (QR Wi-Fi),
//     page 2 "ouvrir le portail" (QR de l'adresse).
// ============================================================================
static lv_obj_t *s_qr_scr = NULL;
static lv_group_t *s_qr_group = NULL;
static int s_qr_page = 0;

static void qr_screen_build() {
  if (!s_qr_scr) {
    s_qr_scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_qr_scr);
    lv_obj_set_style_bg_color(s_qr_scr, C(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_qr_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_qr_scr, LV_OBJ_FLAG_SCROLLABLE);
  }
  if (!s_qr_group) s_qr_group = lv_group_create();
  lv_group_remove_all_objs(s_qr_group);
  lv_obj_clean(s_qr_scr);

#if ENABLE_WIFI_PORTAL
  bool ap = (g_wifi_boot_status == WIFI_BOOT_AP_FALLBACK);
  bool station = (g_wifi_boot_status == WIFI_BOOT_STATION_OK);
#else
  bool ap = false, station = false;
#endif
  int pages = ap ? 2 : 1;
  if (s_qr_page >= pages) s_qr_page = 0;
  bool join_page = ap && s_qr_page == 0;

  static char text[160];
  if (join_page) wifi_join_qr_text(text, sizeof(text));
  else if (station) {
#if ENABLE_WIFI_PORTAL
    snprintf(text, sizeof(text), "http://%s/", g_wifi_boot_status_text);
#endif
  }
  else snprintf(text, sizeof(text), "http://192.168.4.1/");

  // QR a gauche, sur fond blanc (204 x 204)
  lv_obj_t *tile = lv_obj_create(s_qr_scr);
  lv_obj_remove_style_all(tile);
  lv_obj_set_style_bg_color(tile, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(tile, 8, 0);
  lv_obj_set_size(tile, 204, 204);
  lv_obj_set_pos(tile, 6, 18);
  lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *img = make_qr_image(tile, text, 198);
  if (img) lv_obj_center(img);
  else {
    lv_obj_t *e = make_label(tile, "QR indisponible", &ui_font_14, COL_RED);
    lv_obj_center(e);
  }

  // Colonne de droite (x 218..314) : textes courts en haut, boutons fixes
  // en bas (toujours visibles, quelle que soit la longueur des textes).
  const int cx = 218, cw = DISPLAY_WIDTH - cx - 6;
  lv_obj_t *col = make_plain(s_qr_scr);
  lv_obj_set_size(col, cw, 128);
  lv_obj_set_pos(col, cx, 18);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(col, 4, 0);

  if (pages > 1) make_label(col, s_qr_page == 0 ? "\xC3\x89tape 1 / 2" : "\xC3\x89tape 2 / 2", &ui_font_12, COL_MUT);
  make_label(col, join_page ? "Wi-Fi" : "Portail web", &ui_font_14, COL_TXT);
  lv_obj_t *d = make_label(col, join_page ? "Scannez pour rejoindre le r\xC3\xA9seau :" : "Scannez avec l'appareil photo :",
                           &ui_font_12, COL_MUT);
  lv_obj_set_width(d, LV_PCT(100));
  lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
  // Adresse sur deux lignes ("http://" puis l'adresse IP) : sinon la
  // colonne etroite la couperait au milieu des chiffres.
  static char shown[176];
  if (!join_page && strncmp(text, "http://", 7) == 0) snprintf(shown, sizeof(shown), "http://\n%s", text + 7);
  else snprintf(shown, sizeof(shown), "%s", join_page ? WIFI_AP_SSID : text);
  lv_obj_t *v = make_label(col, shown, &ui_font_12, join_page ? COL_CYAN : COL_ACC_TXT);
  lv_obj_set_width(v, LV_PCT(100));
  lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);

  if (pages > 1) {
    lv_obj_t *bn = make_button(s_qr_scr, s_qr_page == 0 ? "Suivant " LV_SYMBOL_RIGHT : LV_SYMBOL_LEFT " Retour", COL_ACC, COL_ACC_INK);
    lv_obj_set_size(bn, cw, 34);
    lv_obj_set_pos(bn, cx, 150);
    lv_obj_add_event_cb(bn, flag_event, LV_EVENT_CLICKED, (void *)&s_req_qr_next);
    lv_group_add_obj(s_qr_group, bn);
  }
  lv_obj_t *bc = make_button(s_qr_scr, "Fermer", COL_SURF2, COL_TXT);
  lv_obj_set_size(bc, cw, 34);
  lv_obj_set_pos(bc, cx, 188);
  lv_obj_add_event_cb(bc, flag_event, LV_EVENT_CLICKED, (void *)&s_req_qr_close);
  lv_group_add_obj(s_qr_group, bc);

  if (s_keypad_indev) {
    lv_indev_set_group(s_keypad_indev, s_qr_group);
    lv_group_focus_next(s_qr_group);
  }
}

static void qr_screen_open() {
  toast_hide();
  s_qr_page = 0;
  qr_screen_build();
  lv_scr_load(s_qr_scr);
}

static void qr_screen_close() {
  if (!s_qr_scr) return;
  lv_scr_load(s_scr);
  lv_obj_del(s_qr_scr);          // libere aussi l'image du QR (PSRAM)
  s_qr_scr = NULL;
  show_tab(s_tab);               // groupe manette + onglet Portail a jour
}

// ============================================================================
// Ecran de recherche : clavier AZERTY + chiffres, compteur et apercu des jeux
// trouves. OK (ou B/START a la manette) applique le filtre a la liste. Ecran
// cree a l'ouverture et detruit a la fermeture (~2 Ko).
// ============================================================================
static lv_obj_t *s_search_scr = NULL;
static lv_obj_t *s_kb_query = NULL;
static lv_obj_t *s_kb_count = NULL;
static lv_obj_t *s_kb_preview = NULL;
static lv_group_t *s_kb_group = NULL;
static char s_kb_text[sizeof(s_search)];

#define KB_ID_BACKSPACE 36
#define KB_ID_CLEAR     37
#define KB_ID_SPACE     38
#define KB_ID_OK        39
static const char *s_kb_map[] = {
  "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
  "A", "Z", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
  "Q", "S", "D", "F", "G", "H", "J", "K", "L", "M", "\n",
  "W", "X", "C", "V", "B", "N", LV_SYMBOL_BACKSPACE, "\n",
  "Effacer", "Espace", "OK", ""
};

static void kb_refresh() {
  if (!s_search_scr) return;
  if (s_kb_text[0]) {
    lv_label_set_text_fmt(s_kb_query, "%s_", s_kb_text);
    lv_obj_set_style_text_color(s_kb_query, C(COL_TXT), 0);
  } else {
    lv_label_set_text(s_kb_query, "Tapez un mot-cl\xC3\xA9_");
    lv_obj_set_style_text_color(s_kb_query, C(COL_DIM), 0);
  }
  // Compteur + les 3 premiers jeux trouves (un par ligne, raccourcis)
  static char prev[3 * 48];
  size_t k = 0;
  int count = 0;
  prev[0] = '\0';
  for (int i = 0; i < s_rows_built; i++) {
    if (!game_matches(i, s_kb_text)) continue;
    count++;
    if (count <= 3 && k < sizeof(prev)) {
      size_t len;
      const char *nm = game_search_name(i, &len);
      if (len > 40) k += snprintf(prev + k, sizeof(prev) - k, "%s%.38s\xE2\x80\xA6", k ? "\n" : "", nm);
      else k += snprintf(prev + k, sizeof(prev) - k, "%s%.*s", k ? "\n" : "", (int)len, nm);
    }
  }
  if (count == 0) lv_label_set_text(s_kb_count, "aucun jeu");
  else lv_label_set_text_fmt(s_kb_count, "%d jeu%s", count, count > 1 ? "x" : "");
  lv_obj_set_style_text_color(s_kb_count, C(count ? COL_ACC_TXT : COL_WARM), 0);
  lv_label_set_text(s_kb_preview, s_kb_text[0] ? prev : "");
}

static void kb_event(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
  lv_obj_t *bm = lv_event_get_target(e);
  uint16_t id = lv_btnmatrix_get_selected_btn(bm);
  if (id == LV_BTNMATRIX_BTN_NONE) return;
  size_t len = strlen(s_kb_text);
  if (id == KB_ID_OK) { s_req_search_close = true; return; }
  if (id == KB_ID_BACKSPACE) { if (len) s_kb_text[len - 1] = '\0'; }
  else if (id == KB_ID_CLEAR) { s_kb_text[0] = '\0'; }
  else if (id == KB_ID_SPACE) {
    if (len && s_kb_text[len - 1] != ' ' && len + 1 < sizeof(s_kb_text)) { s_kb_text[len] = ' '; s_kb_text[len + 1] = '\0'; }
  } else {
    const char *t = lv_btnmatrix_get_btn_text(bm, id);
    if (t && t[0] && !t[1] && len + 1 < sizeof(s_kb_text)) { s_kb_text[len] = t[0]; s_kb_text[len + 1] = '\0'; }
  }
  kb_refresh();
}

static void search_screen_open() {
  if (s_search_scr) return;
  toast_hide();   // Pas de message par-dessus le clavier
  snprintf(s_kb_text, sizeof(s_kb_text), "%s", s_search);

  s_search_scr = lv_obj_create(NULL);
  lv_obj_remove_style_all(s_search_scr);
  lv_obj_set_style_bg_color(s_search_scr, C(COL_BG), 0);
  lv_obj_set_style_bg_opa(s_search_scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(s_search_scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *icon = make_label(s_search_scr, UI_SYMBOL_SEARCH, &ui_font_14, COL_ACC_TXT);
  lv_obj_set_pos(icon, 10, 12);
  s_kb_query = make_label(s_search_scr, "", &ui_font_20, COL_TXT);
  lv_obj_set_width(s_kb_query, 196);
  lv_label_set_long_mode(s_kb_query, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(s_kb_query, 30, 8);
  s_kb_count = make_label(s_search_scr, "", &ui_font_12, COL_ACC_TXT);
  lv_obj_set_width(s_kb_count, 78);
  lv_obj_set_style_text_align(s_kb_count, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_pos(s_kb_count, DISPLAY_WIDTH - 88, 13);
  s_kb_preview = make_label(s_search_scr, "", &ui_font_12, COL_MUT);
  lv_obj_set_width(s_kb_preview, DISPLAY_WIDTH - 20);
  lv_label_set_long_mode(s_kb_preview, LV_LABEL_LONG_CLIP);
  lv_obj_set_height(s_kb_preview, 46);
  lv_obj_set_pos(s_kb_preview, 10, 38);

  lv_obj_t *kb = lv_btnmatrix_create(s_search_scr);
  lv_obj_remove_style_all(kb);
  lv_obj_add_style(kb, &st_plain, 0);
  lv_obj_set_style_pad_all(kb, 3, 0);
  lv_obj_set_style_pad_row(kb, 4, 0);
  lv_obj_set_style_pad_column(kb, 3, 0);
  lv_obj_add_style(kb, &st_kb_key, LV_PART_ITEMS);
  lv_obj_add_style(kb, &st_bm_pr, (lv_style_selector_t)LV_PART_ITEMS | (lv_style_selector_t)LV_STATE_PRESSED);
  lv_obj_add_style(kb, &st_bm_on, (lv_style_selector_t)LV_PART_ITEMS | (lv_style_selector_t)LV_STATE_CHECKED);
  lv_obj_add_style(kb, &st_focus_key, (lv_style_selector_t)LV_PART_ITEMS | (lv_style_selector_t)LV_STATE_FOCUS_KEY);
  lv_obj_set_size(kb, DISPLAY_WIDTH, DISPLAY_HEIGHT - 86);
  lv_obj_set_pos(kb, 0, 86);
  lv_btnmatrix_set_map(kb, s_kb_map);
  // Une touche = un caractere, au relachement (un doigt qui glisse peut
  // encore se corriger) ; pas de repetition en appui long.
  lv_btnmatrix_set_btn_ctrl_all(kb, (lv_btnmatrix_ctrl_t)(LV_BTNMATRIX_CTRL_CLICK_TRIG | LV_BTNMATRIX_CTRL_NO_REPEAT));
  lv_btnmatrix_set_btn_width(kb, KB_ID_BACKSPACE, 2);
  lv_btnmatrix_set_btn_width(kb, KB_ID_CLEAR, 3);
  lv_btnmatrix_set_btn_width(kb, KB_ID_SPACE, 4);
  lv_btnmatrix_set_btn_width(kb, KB_ID_OK, 3);
  lv_btnmatrix_set_btn_ctrl(kb, KB_ID_OK, LV_BTNMATRIX_CTRL_CHECKED);   // couleur d'accent
  lv_obj_add_event_cb(kb, kb_event, LV_EVENT_VALUE_CHANGED, NULL);

  if (!s_kb_group) s_kb_group = lv_group_create();
  lv_group_remove_all_objs(s_kb_group);
  lv_group_add_obj(s_kb_group, kb);
  if (s_keypad_indev) lv_indev_set_group(s_keypad_indev, s_kb_group);
  s_keypad_arrows = true;

  kb_refresh();
  lv_scr_load(s_search_scr);
}

// Ferme l'ecran ; apply = true : le texte tape devient le filtre.
static void search_screen_close(bool apply) {
  if (!s_search_scr) return;
  if (apply) {
    size_t len = strlen(s_kb_text);
    while (len && s_kb_text[len - 1] == ' ') s_kb_text[--len] = '\0';   // espaces en fin
    snprintf(s_search, sizeof(s_search), "%s", s_kb_text);
  }
  s_keypad_arrows = false;
  // A encore tenu (B ou START appuye pendant la saisie) : son relachement
  // ne doit pas "cliquer" le jeu qui va recevoir la selection.
  if (s_keypad_indev) lv_indev_wait_release(s_keypad_indev);
  lv_scr_load(s_scr);
  lv_obj_del(s_search_scr);
  s_search_scr = NULL;
  show_tab(TAB_GAMES);           // groupe manette de l'onglet Jeux
  if (apply) {
    // La recherche porte sur TOUS les jeux ; resultat en pages.
    osd_games_set_view(s_search, 0);
    build_games_panel();
  }
  Serial.printf("[UI] Recherche : \"%s\" -> %d jeu(x)\n", s_search, osd_games_matching());
}

// ============================================================================
// Message bref en bas de l'ecran : "Jeu supprime", erreur...
// ============================================================================
static lv_obj_t *s_toast = NULL;

static void toast_deleted_cb(lv_event_t *e) {
  if (lv_event_get_target(e) == s_toast) s_toast = NULL;
}

// Texte fonce sur le rouge des themes sombres, blanc sur celui du theme
// clair (contraste >= 4,5:1 dans les deux cas).
static uint32_t red_ink() { return s_th->dark ? 0x140404 : 0xFFFFFF; }

static void tv_note_toast(const char *msg, bool error);

// Ms et long (texte sur plusieurs lignes) pour les messages importants (jeu
// ecarte apres un plantage...).
static void toast_show_ex(const char *msg, bool error, uint32_t ms, bool multi) {
  tv_note_toast(msg, error);   // Le message apparait aussi sur la television
  if (s_toast) lv_obj_del(s_toast);
  lv_obj_t *t = lv_label_create(lv_layer_top());
  s_toast = t;
  lv_obj_add_event_cb(t, toast_deleted_cb, LV_EVENT_DELETE, NULL);
  lv_label_set_text(t, msg);
  lv_label_set_long_mode(t, multi ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_DOT);
  lv_obj_set_size(t, DISPLAY_WIDTH - 24, multi ? LV_SIZE_CONTENT : 32);
  if (multi) lv_obj_set_style_pad_bottom(t, 7, 0);
  lv_obj_set_style_text_font(t, &ui_font_14, 0);
  lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(t, C(error ? red_ink() : COL_TXT), 0);
  lv_obj_set_style_bg_color(t, C(error ? COL_RED : COL_SURF2), 0);
  lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(t, C(error ? COL_RED : COL_ACC), 0);
  lv_obj_set_style_border_width(t, 1, 0);
  lv_obj_set_style_radius(t, 16, 0);
  lv_obj_set_style_pad_hor(t, 12, 0);
  lv_obj_set_style_pad_top(t, 7, 0);
  lv_obj_align(t, LV_ALIGN_BOTTOM_MID, 0, -(NAV_H + 6));
  lv_obj_del_delayed(t, ms);
}

static void toast_show(const char *msg, bool error) { toast_show_ex(msg, error, 2200, false); }

static void toast_hide() {
  if (s_toast) lv_obj_del(s_toast);   // s_toast remis a NULL par toast_deleted_cb()
}

// ============================================================================
// Relecture de la liste des jeux (envoi par le portail, suppression)
// ============================================================================
// Manette : selection sur la ligne visible la plus proche de "index" (le
// jeu qui a pris la place d'un jeu supprime).
static void games_focus_near(int index) {
#if HAS_PHYSICAL_CONTROLLER
  if (!s_games_col || s_rows_built <= 0) return;
  if (index >= s_rows_built) index = s_rows_built - 1;
  if (index < 0) index = 0;
  for (int d = 0; d < s_rows_built; d++) {
    for (int k = 0; k < 2; k++) {
      int i = k ? index - d : index + d;
      if (i < 0 || i >= s_rows_built) continue;
      lv_obj_t *row = lv_obj_get_child(s_games_col, i + GAME_ROW_FIRST);
      if (row && !lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN)) {
        lv_group_focus_obj(row);
        if (s_keypad_indev) lv_obj_add_state(row, LV_STATE_FOCUS_KEY);
        return;
      }
    }
  }
#else
  (void)index;
#endif
}

// keep_index >= 0 : la liste garde sa position (suppression depuis l'ecran) ;
// -1 : retour en haut de la liste (envoi par le portail).
static void games_reload(int keep_index) {
  lv_coord_t sy = s_games_col ? lv_obj_get_scroll_y(s_games_col) : 0;
#if ENABLE_WIFI_PORTAL
  g_portal_games_changed = false;   // la relecture ci-dessous prend aussi en compte le portail
#endif
  osd_games_rescan_quiet();
  build_games_panel();       // met aussi a jour le titre
  if (s_about_games) lv_label_set_text_fmt(s_about_games, "%d", osd_games_total());
  update_storage_labels();
  if (keep_index >= 0 && s_games_col) {
    lv_obj_update_layout(s_games_col);
    lv_obj_scroll_to_y(s_games_col, sy, LV_ANIM_OFF);
    games_focus_near(keep_index);
  }
}

// ============================================================================
// Manette du telephone (Bluetooth) dans la liste des jeux
// ============================================================================
// (A monte, B descend, SELECT choisit, START lance.) Seulement sur l'onglet
// Jeux, sans fiche, recherche ni ecran QR ouverts. Une fois la partie lancee,
// les boutons du telephone redeviennent ceux de la manette NES
// (nes_get_gamepad_state(), controller.cpp).
#define TEL_REPEAT_DELAY_MS 450   // fleche maintenue : 1re repetition
#define TEL_REPEAT_MS       150   // puis une ligne toutes les 150 ms
#define TEL_PAGE            TVM_VISIBLE_ROWS   // GAUCHE/DROITE : une page de la liste sur la TV

// Ligne du jeu i, meme cachee par le filtre de recherche (NULL si absente)
static lv_obj_t *tel_row_any(int i) {
  if (!s_games_col || i < 0 || i >= s_rows_built) return NULL;
  return lv_obj_get_child(s_games_col, i + GAME_ROW_FIRST);
}

// Ligne du jeu i si elle est affichee, sinon NULL
static lv_obj_t *tel_row(int i) {
  lv_obj_t *row = tel_row_any(i);
  return (row && !lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN)) ? row : NULL;
}

// Premiere ligne affichee au moins en partie a l'ecran : le curseur
// apparait la ou l'on regarde, meme apres un defilement au doigt.
static int tel_first_on_screen() {
  lv_obj_update_layout(s_games_col);   // positions a jour (liste tout juste reconstruite)
  lv_area_t col;
  lv_obj_get_coords(s_games_col, &col);
  int first = -1;
  for (int i = 0; i < s_rows_built; i++) {
    lv_obj_t *row = tel_row(i);
    if (!row) continue;
    if (first < 0) first = i;
    if (row->coords.y2 > col.y1) return i;
  }
  return first;
}

static void tel_cursor_set(int i) {
  lv_obj_t *row = tel_row(i);
  if (!row) return;
  lv_obj_t *old = (s_tel_cursor != i) ? tel_row_any(s_tel_cursor) : NULL;
  if (old) lv_obj_clear_state(old, LV_STATE_FOCUS_KEY);
#if HAS_PHYSICAL_CONTROLLER
  lv_group_focus_obj(row);   // un seul curseur pour les deux manettes
#endif
  lv_obj_add_state(row, LV_STATE_FOCUS_KEY);
  lv_obj_scroll_to_view(row, LV_ANIM_ON);
  s_tel_cursor = i;
}

// Manette physique compilee : elle a pu deplacer le curseur entre-temps.
static void tel_sync_from_group() {
#if HAS_PHYSICAL_CONTROLLER
  lv_obj_t *f = lv_group_get_focused(s_groups[TAB_GAMES]);
  if (f && lv_obj_get_parent(f) == s_games_col) {
    int fi = (int)lv_obj_get_index(f) - GAME_ROW_FIRST;
    if (tel_row(fi)) s_tel_cursor = fi;
  }
#endif
}

// dir = -1 (monter) ou +1 (descendre), steps jeux affiches plus loin (s'arrete
// au premier ou au dernier). Sans curseur affiche, l'appui le fait seulement
// apparaitre.
static void tel_cursor_move(int dir, int steps) {
  if (!tel_row(s_tel_cursor)) {
    tel_cursor_set(tel_first_on_screen());
    return;
  }
  int target = s_tel_cursor;
  for (int i = s_tel_cursor + dir; i >= 0 && i < s_rows_built && steps > 0; i += dir) {
    if (tel_row(i)) {
      target = i;
      steps--;
    }
  }
  if (target != s_tel_cursor) {
    tel_cursor_set(target);
  } else {
    // Au bout de la page, la croix passe a la page voisine (le curseur va sur
    // son premier ou son dernier jeu).
    const int page = osd_games_page();
    if (dir > 0 && page < osd_games_page_count() - 1) {
      s_req_view_page = page + 1;
      s_req_view_cursor = 0;
      s_req_view = true;
    } else if (dir < 0 && page > 0) {
      s_req_view_page = page - 1;
      s_req_view_cursor = -1;
      s_req_view = true;
    }
  }
}

// A ou START : lance le jeu sous le curseur (sans curseur encore affiche :
// il apparait seulement).
static void tel_launch_cursor(int button) {
  if (!tel_row(s_tel_cursor)) {
    tel_cursor_set(tel_first_on_screen());
    return;
  }
  // Bouton encore tenu au debut de la partie : il serait lu par le jeu
  // (pause, saut, ecran titre passe) - on attend son relachement, 1,5 s au
  // plus.
  uint32_t t0 = millis();
  while ((bluetooth_controller_get_state() & button) && millis() - t0 < 1500) ui_pump();
  Serial.printf("[UI] Telephone : lancement -> %s\n", osd_game_filename(s_tel_cursor));
  s_launch_index = s_tel_cursor;   // lance au tour suivant de la boucle
}

// Appelee a chaque tour de la boucle des menus (ui_lvgl_run_menu()).
static bool tv_list_active();

static void tel_nav_poll() {
  int s = bluetooth_controller_get_state();
  int pressed_now = s & ~s_tel_prev;   // fronts montants
  s_tel_prev = s;

  // En sortie Televiseur, un appui sur la croix, A ou START ramene la console
  // sur la liste des jeux (fiche, recherche ou ecran QR fermes) ; cet appui-la
  // ne fait rien d'autre.
  const int nav_buttons = NES_UP | NES_DOWN | NES_LEFT | NES_RIGHT | NES_A | NES_START;
  if (tv_list_active() && (pressed_now & nav_buttons) &&
      (s_tab != TAB_GAMES || s_sheet || s_pages || s_quar || s_help || s_search_scr || s_qr_scr)) {
    if (s_qr_scr) s_req_qr_close = true;
    if (s_search_scr) s_req_search_close = true;
    if (s_sheet) s_req_sheet_close = true;
    if (s_pages) s_req_pages_close = true;
    if (s_quar) s_req_quar_close = true;
    if (s_help) s_req_help_close = true;
    if (s_tab != TAB_GAMES) show_tab(TAB_GAMES);
    s_tel_arrows_prev = s & (NES_UP | NES_DOWN | NES_LEFT | NES_RIGHT);
    s_tel_repeat_at = millis() + TEL_REPEAT_DELAY_MS;
    Serial.println("[UI] Sortie Televiseur : retour a la liste des jeux (manette du telephone).");
    return;
  }
  // SELECT du telephone : Tous -> Favoris -> Recents -> Tous
  if ((pressed_now & NES_SELECT) && s_tab == TAB_GAMES && !s_sheet && !s_pages && !s_search_scr && !s_qr_scr && s_games_col &&
      osd_games_total() > 0) {
    s_req_mode = (osd_games_mode() + 1) % 3;
    return;
  }
  if (s_tab != TAB_GAMES || s_sheet || s_pages || s_search_scr || s_qr_scr || !s_games_col || s_rows_built <= 0) return;

  // Telephone connecte : le curseur est affiche d'office (premier jeu a
  // l'ecran), BAS descend donc des le premier appui.
  tel_sync_from_group();
  if (!tel_row(s_tel_cursor) && bluetooth_controller_is_connected()) tel_cursor_set(tel_first_on_screen());

  // Croix glissante : une diagonale (BAS puis BAS-DROITE...) compte pour
  // HAUT/BAS seul - GAUCHE/DROITE ne changent de page que seules.
  int arrows_now = s & (NES_UP | NES_DOWN | NES_LEFT | NES_RIGHT);
  if (arrows_now & (NES_UP | NES_DOWN)) arrows_now &= ~(NES_LEFT | NES_RIGHT);
  const int arrows_new = arrows_now & ~s_tel_arrows_prev;
  s_tel_arrows_prev = arrows_now;
  uint32_t now = millis();
  int held = 0;
  if (arrows_new) {
    held = arrows_new;
    s_tel_repeat_at = now + TEL_REPEAT_DELAY_MS;
  } else if (arrows_now && (int32_t)(now - s_tel_repeat_at) >= 0) {
    held = arrows_now;
    s_tel_repeat_at = now + TEL_REPEAT_MS;
  }
  if (held & NES_UP) tel_cursor_move(-1, 1);
  else if (held & NES_DOWN) tel_cursor_move(+1, 1);
  else if (held & NES_LEFT) tel_cursor_move(-1, TEL_PAGE);
  else if (held & NES_RIGHT) tel_cursor_move(+1, TEL_PAGE);

  if ((pressed_now & (NES_A | NES_START)) && (int32_t)(now - s_tel_launch_guard) < 0) {
    toast_show("Liste mise \xC3\xA0 jour : v\xC3\xA9rifie le jeu choisi", false);
    return;
  }
  if (pressed_now & NES_A) tel_launch_cursor(NES_A);
  else if (pressed_now & NES_START) tel_launch_cursor(NES_START);
}

// ============================================================================
// La liste des jeux recopiee sur la television (Sortie : Televiseur)
// ============================================================================
// Meme liste (filtre de recherche compris) et meme curseur que l'ecran de la
// console, dessinee par tv_menu.cpp et envoyee a la carte video comme une
// image de jeu. Redessinee a chaque changement, renvoyee toutes les 150 ms
// (la carte video revient a son ecran d'attente sans image pendant 3 s, et
// jette une bande abimee en route). Le dernier message (refus d'un jeu...) y apparait aussi.
static char s_tv_msg[96] = "";
static bool s_tv_msg_err = false;
static uint32_t s_tv_msg_until = 0;
static int s_tv_rows[MAX_GAMES];   // jeux affiches (filtre applique), dans l'ordre
static int s_tv_count = 0;

static void tv_note_toast(const char *msg, bool error) {
  snprintf(s_tv_msg, sizeof(s_tv_msg), "%s", msg ? msg : "");
  s_tv_msg_err = error;
  s_tv_msg_until = millis() + 2500;
}

static const char *tv_row_name(int row, uint8_t *flags) {
  const int i = (row >= 0 && row < s_tv_count) ? s_tv_rows[row] : -1;
  if (i < 0 || !s_game_disp) {
    *flags = 0;
    return "";
  }
  *flags = ((s_game_flags[i] & GF_BAD) ? TVM_BAD : 0) | ((s_game_flags[i] & GF_FLASH) ? TVM_FLASH : 0) |
           ((s_game_flags[i] & GF_FAV) ? TVM_FAV : 0);
  // Le nom de l'ecran LCD commence par "! " pour une ROM en liste noire :
  // sur la television, l'etiquette "!" a droite suffit.
  const char *n = s_game_disp[i];
  if ((*flags & TVM_BAD) && n[0] == '!' && n[1] == ' ') n += 2;
  return n;
}

static bool tv_list_active() {
  return g_display_tv_mode && video_link_is_ready();
}

static void tv_mirror_poll(bool force) {
  if (!tv_list_active()) return;
  static uint32_t next_check = 0, last_sent = 0, sig_sent = 0;
  static bool pending = false;
  const uint32_t now = millis();
  if (!force && (int32_t)(now - next_check) < 0) return;
  next_check = now + 60;

  tel_sync_from_group();
  s_tv_count = 0;
  int cursor = -1;
  for (int i = 0; i < s_rows_built && s_tv_count < MAX_GAMES; i++) {
    if (!tel_row(i)) continue;
    if (i == s_tel_cursor) cursor = s_tv_count;
    s_tv_rows[s_tv_count++] = i;
  }
  const bool msg_on = s_tv_msg[0] && (int32_t)(now - s_tv_msg_until) < 0;
  const bool phone = bluetooth_controller_is_connected();
  // L'ecran de la console montre autre chose que la liste (onglet Video
  // apres le choix de la sortie...) : la television l'indique.
  const bool away = s_tab != TAB_GAMES || s_sheet || s_pages || s_quar || s_help || s_search_scr || s_qr_scr;

  // Empreinte de ce qui est affiche : on ne redessine que si elle change.
  uint32_t sig = 2166136261u;
  auto mixin = [&sig](uint32_t v) { sig = (sig ^ v) * 16777619u; };
  mixin((uint32_t)cursor); mixin((uint32_t)s_tv_count); mixin(phone); mixin(msg_on); mixin(away);
  mixin(s_list_builds); mixin(osd_games_generation());   // liste reconstruite (noms, liste noire)
  mixin((uint32_t)osd_games_mode()); mixin(osd_library_generation());   // Liste, favoris
  for (int k = 0; k < s_tv_count; k++) mixin((uint32_t)s_tv_rows[k]);
  for (const char *p = s_search; *p; p++) mixin((uint8_t)*p);
  if (msg_on) for (const char *p = s_tv_msg; *p; p++) mixin((uint8_t)*p);

  if (sig != sig_sent || force) {
    char badge[24], sub[64];
    const int total = osd_games_total();   // Tous les jeux
    if (osd_games_page_count() > 1)
      snprintf(badge, sizeof(badge), "%d-%d / %d", osd_games_page_first() + 1,
               osd_games_page_first() + osd_games_count(), osd_games_matching());
    else if (s_search[0]) snprintf(badge, sizeof(badge), "%d / %d", s_tv_count, total);
    else snprintf(badge, sizeof(badge), "%d jeu%s", total, total > 1 ? "x" : "");
    if (away && phone) snprintf(sub, sizeof(sub), "Appuie sur la croix pour choisir un jeu");
    else if (s_search[0]) snprintf(sub, sizeof(sub), "Recherche : %s", s_search);
    else snprintf(sub, sizeof(sub), "%s", phone ? "Choisis un jeu avec la manette"
                                                : "Choisis un jeu sur l'\xC3\xA9" "cran de la console");
    const int mode = osd_games_mode();
    if (mode != OSD_VIEW_ALL && !s_search[0] && osd_games_page_count() <= 1)
      snprintf(badge, sizeof(badge), "%d jeu%s", osd_games_matching(), osd_games_matching() > 1 ? "x" : "");
    if (mode != OSD_VIEW_ALL && s_tv_count == 0 && !s_search[0])
      snprintf(sub, sizeof(sub), "%s", mode == OSD_VIEW_FAVS ? "Aucun favori : SELECT pour revenir \xC3\xA0 tous les jeux"
                                                             : "Aucun jeu r\xC3\xA9" "cent : SELECT pour changer de liste");
    TvMenuView v = {};
    v.title = mode == OSD_VIEW_FAVS ? "Favoris" : mode == OSD_VIEW_RECENTS ? "R\xC3\xA9" "cents" : "Jeux";
    v.badge = badge;
    v.subtitle = sub;
    v.count = s_tv_count;
    v.row = tv_row_name;
    v.cursor = cursor;
    v.hint = phone ? "A : lancer   \xE2\x80\xA2   SELECT : changer de liste"
                   : "Croix : choisir   \xE2\x80\xA2   A / START : lancer";
    v.message = msg_on ? s_tv_msg : NULL;
    v.message_error = s_tv_msg_err;
    if (tv_menu_render(v, NULL, NULL)) {
      sig_sent = sig;
      pending = true;
    }
  }
  // Envoi : apres un changement, et toutes les 150 ms - l'image entiere repart
  // au moins 3 fois par seconde (tv_menu_submit()), une bande perdue en route
  // ne reste pas a l'ecran.
  if (pending || now - last_sent >= 150) {
    if (tv_menu_submit()) {
      pending = false;
      last_sent = now;
    }
  }
}

// Ecran "Lancement..." sur la television pendant le chargement du jeu.
static void tv_show_launch(const char *name) {
  if (!g_display_tv_mode || !video_link_is_ready()) return;
  if (!tv_menu_render_notice("Lancement\xE2\x80\xA6", name, NULL, NULL)) return;
  const uint32_t t0 = millis();
  while (!tv_menu_submit() && millis() - t0 < 300) {   // image precedente encore en route
    video_link_tick(false);
    delay(5);
  }
  video_link_flush(150);   // toute l'image part avant le chargement du jeu
}

// ============================================================================
// Fiche d'un jeu : appui long sur un jeu, ou A maintenu a la manette. "Jouer"
// / "Supprimer" ; la suppression (carte SD ou memoire interne) demande une
// confirmation, ou "Annuler" est le choix par defaut a la manette. Fenetre sur
// lv_layer_top(), creee a l'ouverture et detruite a la fermeture (~1,5 Ko de
// tas LVGL). Un appui hors de la fiche, B ou START la ferment.
// ============================================================================
static lv_group_t *s_sheet_group = NULL;
static char s_sheet_name[64];             // nom du fichier, verifie avant d'agir
static int s_sheet_where = OSD_WHERE_SD;
static int s_sheet_index = -1;
static bool s_sheet_confirm = false;
static lv_obj_t *s_sheet_star = NULL;     // Etoile des favoris

// Index actuel du jeu de la fiche (verifie par son nom et son emplacement :
// la liste n'est jamais relue pendant que la fiche est ouverte, mais on ne
// supprime rien sur la seule foi d'un numero). -1 s'il a disparu.
static int sheet_find_game() {
  int n = osd_games_count();
  if (s_sheet_index >= 0 && s_sheet_index < n && osd_game_where(s_sheet_index) == s_sheet_where &&
      strcmp(osd_game_filename(s_sheet_index), s_sheet_name) == 0) return s_sheet_index;
  for (int i = 0; i < n; i++)
    if (osd_game_where(i) == s_sheet_where && strcmp(osd_game_filename(i), s_sheet_name) == 0) return i;
  return -1;
}

static void sheet_display_name(char *out, size_t n) {
  snprintf(out, n, "%s", s_sheet_name);
  size_t l = strlen(out);
  if (l > 4 && strcasecmp(out + l - 4, ".nes") == 0) out[l - 4] = '\0';
}

static lv_obj_t *sheet_button(lv_obj_t *row, const char *txt, uint32_t bg, uint32_t fg, volatile bool *flag) {
  lv_obj_t *b = make_button(row, txt, bg, fg);
  lv_obj_set_flex_grow(b, 1);
  lv_obj_set_height(b, 38);
  lv_obj_add_event_cb(b, flag_event, LV_EVENT_CLICKED, (void *)flag);   // doigt leve hors du bouton = rien
  group_add(s_sheet_group, b);
  return b;
}

// (Re)construit la fiche : actions, ou confirmation si s_sheet_confirm.
static void sheet_build() {
  if (s_sheet) lv_obj_del(s_sheet);
  if (!s_sheet_group) s_sheet_group = lv_group_create();
  lv_group_remove_all_objs(s_sheet_group);

  s_sheet_star = NULL;
  s_sheet = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(s_sheet);
  lv_obj_set_size(s_sheet, DISPLAY_WIDTH, DISPLAY_HEIGHT);
  lv_obj_set_style_bg_color(s_sheet, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(s_sheet, LV_OPA_50, 0);
  lv_obj_clear_flag(s_sheet, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_sheet, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_sheet, flag_event, LV_EVENT_CLICKED, (void *)&s_req_sheet_close);

  lv_obj_t *card = lv_obj_create(s_sheet);
  lv_obj_remove_style_all(card);
  lv_obj_add_style(card, &st_card, 0);
  lv_obj_set_style_radius(card, 16, 0);
  lv_obj_set_style_pad_all(card, 12, 0);
  lv_obj_set_style_pad_row(card, 8, 0);
  lv_obj_set_size(card, DISPLAY_WIDTH - 16, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);   // un appui SUR la fiche ne la ferme pas
  lv_obj_align(card, LV_ALIGN_BOTTOM_MID, 0, -8);

  char disp[64];
  sheet_display_name(disp, sizeof(disp));
  const bool flash = (s_sheet_where == OSD_WHERE_FLASH);
  lv_obj_t *row;

  if (!s_sheet_confirm) {
    int i = sheet_find_game();
    const bool fav = i >= 0 && osd_game_is_fav(i);
    const bool resume = i >= 0 && osd_game_has_resume(i);   // Reprise automatique gardee
    // Titre, et l'etoile des favoris a sa droite
    lv_obj_t *head = make_plain(card);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    const lv_coord_t tw = DISPLAY_WIDTH - 16 - 24 - 48;   // carte - marges - etoile
    lv_obj_t *t = make_label(head, disp, &ui_font_20, COL_TXT);
    lv_obj_set_width(t, tw);
    // 2 lignes au plus (un nom de 63 caracteres en ferait 3 et la fiche
    // deborderait de l'ecran) : "..." au bout de la 2e ligne.
    lv_point_t tsz;
    const lv_coord_t lh = lv_font_get_line_height(&ui_font_20);
    lv_txt_get_size(&tsz, disp, &ui_font_20, 0, 0, tw - 2, LV_TEXT_FLAG_NONE);
    if (tsz.y > 2 * lh) {
      lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
      lv_obj_set_height(t, 2 * lh);
    } else {
      lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    }
    lv_obj_t *star = make_button(head, UI_SYMBOL_STAR, COL_SURF2, fav ? COL_WARM : COL_MUT);
    lv_obj_set_size(star, 42, 34);
    lv_obj_set_style_pad_all(star, 0, 0);
    lv_obj_align(star, LV_ALIGN_TOP_RIGHT, 0, 0);
    if (fav) {   // favori : etoile doree et filet de la meme couleur
      lv_obj_set_style_border_color(star, C(COL_WARM), 0);
      lv_obj_set_style_border_width(star, 1, 0);
    }
    lv_obj_add_event_cb(star, flag_event, LV_EVENT_CLICKED, (void *)&s_req_sheet_fav);
    s_sheet_star = star;

    char sz[16], sub[96];
    format_size(i >= 0 ? osd_game_size(i) : 0, sz, sizeof(sz));
    snprintf(sub, sizeof(sub), "%s%s%s%s", flash ? LV_SYMBOL_DRIVE "  M\xC3\xA9moire interne" : LV_SYMBOL_SD_CARD "  Carte SD",
             sz[0] ? "  \xC2\xB7  " : "", sz, fav ? "  \xC2\xB7  " UI_SYMBOL_STAR " Favori" : "");
    make_label(card, sub, &ui_font_12, flash ? COL_FLASH : COL_MUT);
    if (i >= 0 && osd_game_is_bad(i)) {
      lv_obj_t *w = make_label(card, LV_SYMBOL_WARNING "  Liste noire : ce jeu a d\xC3\xA9j\xC3\xA0 plant\xC3\xA9", &ui_font_12, COL_WARM);
      lv_obj_set_width(w, LV_PCT(100));
      lv_label_set_long_mode(w, LV_LABEL_LONG_WRAP);
    }
    lv_obj_t *bp = make_button(card, resume ? LV_SYMBOL_PLAY "  Reprendre la partie" : LV_SYMBOL_PLAY "  Jouer", COL_ACC, COL_ACC_INK);
    lv_obj_set_size(bp, LV_PCT(100), 40);
    lv_obj_add_event_cb(bp, flag_event, LV_EVENT_CLICKED, (void *)(resume ? &s_req_sheet_resume : &s_req_sheet_play));
    group_add(s_sheet_group, bp);
    row = make_plain(card);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);
    if (resume) {
      // la reprise est le choix principal ; "Recommencer" part du debut
      // (la sauvegarde de la cartouche, elle, est toujours chargee)
      sheet_button(row, LV_SYMBOL_REFRESH "  Recommencer", COL_SURF2, COL_TXT, &s_req_sheet_play);
      lv_obj_t *del = sheet_button(row, LV_SYMBOL_TRASH, COL_SURF2, COL_RED, &s_req_sheet_confirm);
      lv_obj_set_flex_grow(del, 0);
      lv_obj_set_width(del, 46);
    } else {
      sheet_button(row, LV_SYMBOL_TRASH "  Supprimer", COL_SURF2, COL_RED, &s_req_sheet_confirm);
    }
    sheet_button(row, "Fermer", COL_SURF2, COL_TXT, &s_req_sheet_close);
    group_add(s_sheet_group, star);   // en dernier a la manette (Jouer reste le choix par defaut)
  } else {
    make_label(card, LV_SYMBOL_TRASH "  Supprimer ce jeu ?", &ui_font_20, COL_TXT);
    char msg[160];
    snprintf(msg, sizeof(msg), "\xC2\xAB %s \xC2\xBB sera effac\xC3\xA9 de %s. C'est d\xC3\xA9" "finitif.",
             disp, flash ? "la m\xC3\xA9moire interne" : "la carte SD");
    lv_obj_t *m = make_label(card, msg, &ui_font_14, COL_MUT);
    lv_obj_set_width(m, LV_PCT(100));
    lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
    row = make_plain(card);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);
    sheet_button(row, "Annuler", COL_SURF2, COL_TXT, &s_req_sheet_close);   // choix par defaut
    sheet_button(row, LV_SYMBOL_TRASH "  Supprimer", COL_RED, red_ink(), &s_req_sheet_delete);
  }

  if (s_keypad_indev) {
    // Le groupe a selectionne son 1er bouton (Jouer, ou Annuler pour la
    // confirmation) ; contour visible tout de suite.
    lv_indev_set_group(s_keypad_indev, s_sheet_group);
    lv_obj_t *f = lv_group_get_focused(s_sheet_group);
    if (f) lv_obj_add_state(f, LV_STATE_FOCUS_KEY);
  }
}

static void sheet_open(int index) {
  if (s_sheet || index < 0 || index >= osd_games_count()) return;
  snprintf(s_sheet_name, sizeof(s_sheet_name), "%s", osd_game_filename(index));
  s_sheet_where = osd_game_where(index);
  s_sheet_index = index;
  s_sheet_confirm = false;
  // Le doigt (ou A) qui a ouvert la fiche est encore appuye : son
  // relachement ne doit rien declencher dessous.
  if (s_touch_indev) lv_indev_wait_release(s_touch_indev);
  if (s_keypad_indev) lv_indev_wait_release(s_keypad_indev);
  s_keypad_lr_nav = true;
  sheet_build();
  Serial.printf("[UI] Fiche du jeu : %s\n", s_sheet_name);
}

static void sheet_close() {
  if (!s_sheet) return;
  lv_obj_del(s_sheet);
  s_sheet = NULL;
  s_sheet_star = NULL;
  s_sheet_confirm = false;
  s_keypad_lr_nav = false;
  if (s_sheet_group) lv_group_remove_all_objs(s_sheet_group);
  if (s_keypad_indev) lv_indev_wait_release(s_keypad_indev);
  show_tab(TAB_GAMES);           // groupe manette de l'onglet Jeux
  if (s_list_dirty) {            // Favori retire dans la liste des favoris
    s_list_dirty = false;
    osd_games_set_view(s_search, osd_games_page());
    build_games_panel();
  }
}

static void sheet_delete_now() {
  char disp[64], msg[96];
  sheet_display_name(disp, sizeof(disp));
  int idx = sheet_find_game();
  sheet_close();
  if (idx < 0) {
    toast_show("Jeu introuvable (d\xC3\xA9j\xC3\xA0 supprim\xC3\xA9 ?)", true);
    return;
  }
  if (!osd_game_delete(idx)) {
    toast_show(LV_SYMBOL_WARNING "  Suppression impossible", true);
    return;
  }
  games_reload(idx);
  snprintf(msg, sizeof(msg), LV_SYMBOL_OK "  %s supprim\xC3\xA9", disp);
  toast_show(msg, false);
}

// ============================================================================
// Etalonnage du tactile - 3 croix + 1 verification
// ============================================================================
// Precision :
//  - chaque point est la MEDIANE des mesures prises pendant tout l'appui
//    (et non la derniere lecture avant de relever le doigt, souvent
//    deplacee par le mouvement du doigt) ;
// Les coordonnees BRUTES sont lues directement (ft6336_touch_get_raw) ;
// le pointeur LVGL est desactive pendant l'etalonnage.
#define CAL_TIMEOUT_MS  20000
#define CAL_MAX_ERR_PX  12
#define CAL_MAX_SAMPLES 64
static const int CAL_X[4] = {30, 290, 30, 160};
static const int CAL_Y[4] = {30, 30, 210, 120};

struct CalUi {
  lv_obj_t *scr, *cross, *hbar, *vbar, *ring, *title, *step, *hint, *dot;
};

static void cal_cross_color(CalUi &u, uint32_t col) {
  lv_obj_set_style_bg_color(u.hbar, C(col), 0);
  lv_obj_set_style_bg_color(u.vbar, C(col), 0);
  lv_obj_set_style_border_color(u.ring, C(col), 0);
}

static int median_of(int *v, int n) {
  for (int i = 1; i < n; i++) {           // tri par insertion (n <= 64)
    int k = v[i], j = i - 1;
    while (j >= 0 && v[j] > k) { v[j + 1] = v[j]; j--; }
    v[j + 1] = k;
  }
  return v[n / 2];
}

// Resultat de cal_capture()
#define CAL_OK        1    // point mesure
#define CAL_NO_TOUCH  0    // aucun appui dans le delai (20 s)
#define CAL_TOO_SHORT (-1) // appui trop bref pour etre fiable
#define CAL_ABORTED   (-2) // B ou START appuye sur la manette
#define CAL_STUCK     (-3) // contact permanent : le doigt (ou la dalle) ne se releve pas
#define CAL_INCOHERENT 2   // (essai complet) points incoherents entre eux

// Manette : un NOUVEL appui sur B ou START abandonne l'etalonnage (possible
// a tout moment, meme au premier demarrage). Il faut que le bouton ait ete
// vu relache, puis appuye sur 4 lectures de suite (~25 ms, anti-rebond) :
// une manette debranchee qui lirait "tout appuye" en permanence ne
// declenche rien, et une entree flottante qui oscille a peu de chances de
// le faire. Verifie a chaque tour de boucle de l'etalonnage (toutes les
// ~5 ms), donc un appui bref n'est pas manque.
#if HAS_PHYSICAL_CONTROLLER
static bool s_cal_pad_armed = false;
static int s_cal_pad_hold = 0;
#endif
static bool s_cal_abort = false;

static void cal_abort_reset() {
  s_cal_abort = false;
#if HAS_PHYSICAL_CONTROLLER
  s_cal_pad_armed = (controller_read_physical_buttons() & (NES_B | NES_START)) == 0;
  s_cal_pad_hold = 0;
#endif
}

static void cal_pump() {
  ui_pump();
#if HAS_PHYSICAL_CONTROLLER
  if (controller_read_physical_buttons() & (NES_B | NES_START)) {
    if (s_cal_pad_armed && ++s_cal_pad_hold >= 4) s_cal_abort = true;
  } else {
    s_cal_pad_armed = true;
    s_cal_pad_hold = 0;
  }
#endif
}

// Attente de ms millisecondes, interrompue par B/START.
static void cal_pump_for(uint32_t ms) {
  uint32_t t0 = millis();
  while (millis() - t0 < ms && !s_cal_abort) cal_pump();
}

static int cal_capture(CalUi &u, int *rx, int *ry) {
  int x, y;
  // 1) Attend que le doigt soit releve depuis au moins 60 ms d'affilee
  //    (au plus 5 s) : un rebond de la dalle ne compte pas comme relache.
  //    Au-dela de 5 s de contact ininterrompu : CAL_STUCK.
  uint32_t t0 = millis(), t_free = 0;
  bool is_free = false, warned = false, lifted = false;
  static char saved_hint[192];
  while (millis() - t0 < 5000) {
    cal_pump();
    if (s_cal_abort) return CAL_ABORTED;
    if (ft6336_touch_get_raw(&x, &y)) {
      is_free = false;
      if (!warned && millis() - t0 > 400) {
        warned = true;
        snprintf(saved_hint, sizeof(saved_hint), "%s", lv_label_get_text(u.hint));
        lv_label_set_text(u.hint, "Rel\xC3\xA9vez le doigt, puis touchez la croix.");
      }
      continue;
    }
    if (!is_free) { is_free = true; t_free = millis(); }
    else if (millis() - t_free >= 60) { lifted = true; break; }
  }
  if (!lifted && !is_free) return CAL_STUCK;
  if (warned) lv_label_set_text(u.hint, saved_hint);
  // 2) Attend un nouvel appui (au plus CAL_TIMEOUT_MS).
  t0 = millis();
  while (!ft6336_touch_get_raw(&x, &y)) {
    if (millis() - t0 > CAL_TIMEOUT_MS) return CAL_NO_TOUCH;
    cal_pump();
    if (s_cal_abort) return CAL_ABORTED;
  }
  // 3) Mesures pendant tout l'appui, jusqu'au relache (60 ms sans contact)
  //    ou au plus 3 s : un doigt qui reste pose (ou une dalle qui signale un
  //    contact permanent) ne bloque jamais l'ecran.
  cal_cross_color(u, COL_ACC);
  static int xs[CAL_MAX_SAMPLES], ys[CAL_MAX_SAMPLES];
  int n = 0;
  xs[n] = x; ys[n] = y; n++;
  uint32_t t_first = millis(), last_seen = t_first;
  while (millis() - t_first < 3000) {
    cal_pump();
    if (s_cal_abort) { cal_cross_color(u, COL_TXT); return CAL_ABORTED; }
    if (ft6336_touch_get_raw(&x, &y)) {
      if (n < CAL_MAX_SAMPLES) { xs[n] = x; ys[n] = y; n++; }
      last_seen = millis();
    } else if (millis() - last_seen > 60) {
      break;                              // doigt releve
    }
  }
  cal_cross_color(u, COL_TXT);
  if (n < 4) return CAL_TOO_SHORT;
  int s = (n >= 8) ? 2 : 0;               // on ignore le debut et la fin de l'appui
  int e = (n >= 8) ? n - 2 : n;
  *rx = median_of(xs + s, e - s);
  *ry = median_of(ys + s, e - s);
  return CAL_OK;
}

static void set_inputs_enabled(bool en) {
  if (s_touch_indev) {
    lv_indev_enable(s_touch_indev, en);
    if (en) {
      lv_indev_reset(s_touch_indev, NULL);
      s_touch_wait_release = true;
    }
  }
  if (s_keypad_indev) {
    lv_indev_enable(s_keypad_indev, en);
    if (en) {
      lv_indev_reset(s_keypad_indev, NULL);
#if HAS_PHYSICAL_CONTROLLER
      // Un bouton encore enfonce au retour (B/START de l'etalonnage) ne
      // doit pas etre pris pour un nouvel appui par les menus.
      s_kp_prev = controller_read_physical_buttons();
#endif
    }
  }
}

// Renvoie true si un nouvel etalonnage a ete enregistre (NVS "touchcal").
static bool run_touch_calibration_ui(bool first_boot, lv_obj_t *return_scr) {
  float old[6];
  ft6336_touch_get_coeffs(old);
  set_inputs_enabled(false);

  CalUi u;
  u.scr = lv_obj_create(NULL);
  lv_obj_remove_style_all(u.scr);
  lv_obj_set_style_bg_color(u.scr, C(COL_BG), 0);
  lv_obj_set_style_bg_opa(u.scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(u.scr, LV_OBJ_FLAG_SCROLLABLE);

  u.title = make_label(u.scr, "\xC3\x89talonnage de l'\xC3\xA9" "cran tactile", &ui_font_14, COL_TXT);
  lv_obj_align(u.title, LV_ALIGN_TOP_MID, 0, 48);
  u.step = make_label(u.scr, "", &ui_font_20, COL_ACC_TXT);
  lv_obj_align(u.step, LV_ALIGN_TOP_MID, 0, 68);
  u.hint = make_label(u.scr, "", &ui_font_12, COL_MUT);
  lv_obj_set_width(u.hint, 250);
  lv_label_set_long_mode(u.hint, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(u.hint, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(u.hint, LV_ALIGN_TOP_MID, 0, 148);

  u.cross = make_plain(u.scr);
  lv_obj_set_size(u.cross, 40, 40);
  u.hbar = make_plain(u.cross);
  lv_obj_set_size(u.hbar, 36, 2);
  lv_obj_set_style_bg_opa(u.hbar, LV_OPA_COVER, 0);
  lv_obj_center(u.hbar);
  u.vbar = make_plain(u.cross);
  lv_obj_set_size(u.vbar, 2, 36);
  lv_obj_set_style_bg_opa(u.vbar, LV_OPA_COVER, 0);
  lv_obj_center(u.vbar);
  u.ring = make_plain(u.cross);
  lv_obj_set_size(u.ring, 16, 16);
  lv_obj_set_style_radius(u.ring, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(u.ring, 2, 0);
  lv_obj_center(u.ring);
  cal_cross_color(u, COL_TXT);

  u.dot = make_plain(u.scr);
  lv_obj_set_size(u.dot, 8, 8);
  lv_obj_set_style_radius(u.dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(u.dot, C(COL_WARM), 0);
  lv_obj_set_style_bg_opa(u.dot, LV_OPA_COVER, 0);
  lv_obj_add_flag(u.dot, LV_OBJ_FLAG_HIDDEN);

#if HAS_PHYSICAL_CONTROLLER
  lv_obj_t *pad = make_label(u.scr, first_boot ? "Manette : B ou START pour passer"
                                               : "Manette : B ou START pour annuler",
                             &ui_font_12, COL_MUT);
  lv_obj_align(pad, LV_ALIGN_BOTTOM_MID, 0, -6);
#endif

  lv_scr_load(u.scr);
  Serial.println("[TOUCH] Ecran d'etalonnage ouvert");

  // Nombre d'essais : 3 depuis Reglages. Au premier demarrage, 5 si une
  // manette est cablee (elle permet de naviguer meme mal etalonne) ; sans
  // manette, le tactile est la seule commande : on insiste jusqu'a obtenir
  // un etalonnage correct (le portail web reste joignable pendant ce
  // temps), sauf si la dalle signale un contact permanent 2 fois : on
  // n'insiste plus, pour ne pas tourner en rond.
#if HAS_PHYSICAL_CONTROLLER
  bool endless = false;
#else
  bool endless = first_boot;
#endif
  const int max_attempts = first_boot ? 5 : 3;
  int stuck_attempts = 0;
  bool saved = false, aborted = false;
  cal_abort_reset();

  // B ou START (manette) : on garde l'etalonnage precedent et on sort.
  auto do_abort = [&]() {
    ft6336_touch_set_coeffs(old);
    aborted = true;
    lv_obj_add_flag(u.cross, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(u.dot, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(u.step, first_boot ? "\xC3\x89talonnage pass\xC3\xA9" : "\xC3\x89talonnage annul\xC3\xA9");
    lv_label_set_text(u.hint, first_boot ? "Il sera redemand\xC3\xA9 au prochain d\xC3\xA9marrage."
                                         : "L'ancien \xC3\xA9talonnage est conserv\xC3\xA9.");
    Serial.println("[TOUCH] Etalonnage abandonne (manette)");
#if HAS_PHYSICAL_CONTROLLER
    // Attend que B/START soit relache (au plus 2 s).
    uint32_t t0 = millis();
    while ((controller_read_physical_buttons() & (NES_B | NES_START)) && millis() - t0 < 2000) ui_pump();
#endif
    ui_pump_for(1500);
  };

  for (int attempt = 1; (endless || attempt <= max_attempts) && !saved && !aborted; attempt++) {
    int rx[4] = {0}, ry[4] = {0};
    int res = CAL_OK;
    bool flaky = false;
    lv_obj_add_flag(u.dot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(u.cross, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < 4 && res == CAL_OK; i++) {
      lv_obj_set_pos(u.cross, CAL_X[i] - 20, CAL_Y[i] - 20);
      if (i < 3) lv_label_set_text_fmt(u.step, "Point %d sur 3", i + 1);
      else lv_label_set_text(u.step, "V\xC3\xA9rification");
      lv_label_set_text(u.hint, i < 3
        ? "Touchez le centre de la croix et laissez le doigt pos\xC3\xA9 un instant."
        : "Touchez encore le centre de la croix pour v\xC3\xA9rifier le r\xC3\xA9sultat.");
      int too_short = 0;
      do {
        res = cal_capture(u, &rx[i], &ry[i]);
        if (res == CAL_TOO_SHORT) {
          lv_label_set_text(u.hint, "Appui trop bref : laissez le doigt un instant sur la croix.");
          if (++too_short >= 5) { res = CAL_NO_TOUCH; flaky = true; }   // dalle instable : on arrete cet essai
        }
      } while (res == CAL_TOO_SHORT);

      if (i == 2 && res == CAL_OK) {
        bool ok = ft6336_touch_set_calibration(rx[0], ry[0], CAL_X[0], CAL_Y[0],
                                               rx[1], ry[1], CAL_X[1], CAL_Y[1],
                                               rx[2], ry[2], CAL_X[2], CAL_Y[2]);
        float c[6];
        ft6336_touch_get_coeffs(c);
        float det = c[0] * c[4] - c[1] * c[3];
        // Echelle attendue ~1 (1 point brut ~ 1 pixel) : un determinant tres
        // eloigne signale un appui a cote d'une croix.
        if (!ok || fabsf(det) < 0.2f || fabsf(det) > 5.0f) res = CAL_INCOHERENT;
      }
    }

    // 2 essais de suite interrompus par un appui permanent : on n'insiste plus.
    if (res != CAL_STUCK) stuck_attempts = 0;
    else if (++stuck_attempts >= 2) endless = false;
    const bool more = endless || attempt < max_attempts;

    if (res == CAL_ABORTED) {
      do_abort();
      break;
    } else if (res == CAL_OK) {
      int vx, vy;
      ft6336_touch_map_raw(rx[3], ry[3], &vx, &vy);
      int dx = vx - CAL_X[3], dy = vy - CAL_Y[3];
      int err = (int)lroundf(sqrtf((float)(dx * dx + dy * dy)));
      lv_obj_set_pos(u.dot, vx - 4, vy - 4);
      lv_obj_clear_flag(u.dot, LV_OBJ_FLAG_HIDDEN);
      if (err <= CAL_MAX_ERR_PX) {
        ft6336_touch_save_calibration();
        saved = true;
        g_touch_calibration_needed = false;
        lv_label_set_text(u.step, LV_SYMBOL_OK " Enregistr\xC3\xA9");
        lv_label_set_text_fmt(u.hint, "\xC3\x89" "cart mesur\xC3\xA9 au centre : %d px (point orange).", err);
        Serial.printf("[TOUCH] Nouvel etalonnage enregistre (ecart de verification : %d px)\n", err);
      } else {
        ft6336_touch_set_coeffs(old);
        lv_label_set_text(u.step, "Trop impr\xC3\xA9" "cis");
        lv_label_set_text_fmt(u.hint, "\xC3\x89" "cart de %d px au centre : rien n'est enregistr\xC3\xA9.%s",
                              err, more ? " Nouvel essai\xE2\x80\xA6" : "");
        Serial.printf("[TOUCH] Etalonnage refuse (ecart de verification : %d px)\n", err);
      }
    } else if (res == CAL_INCOHERENT) {
      ft6336_touch_set_coeffs(old);
      lv_label_set_text(u.step, "Points incoh\xC3\xA9rents");
      lv_label_set_text(u.hint, more ? "Un appui est tomb\xC3\xA9 loin d'une croix. Nouvel essai\xE2\x80\xA6" : "");
      Serial.println("[TOUCH] Etalonnage refuse (points incoherents)");
    } else if (res == CAL_STUCK) {
      ft6336_touch_set_coeffs(old);
      lv_label_set_text(u.step, "Appui permanent");
      lv_label_set_text(u.hint, more ? "Rel\xC3\xA9vez le doigt. Nouvel essai\xE2\x80\xA6"
                                     : "L'\xC3\xA9" "cran signale un appui qui ne cesse pas.");
      Serial.println("[TOUCH] Etalonnage interrompu (contact permanent)");
    } else {
      ft6336_touch_set_coeffs(old);
      lv_label_set_text(u.step, flaky ? "Appuis trop brefs" : "Aucun appui");
      Serial.println(flaky ? "[TOUCH] Etalonnage interrompu (appuis trop brefs)"
                           : "[TOUCH] Etalonnage interrompu (aucun appui)");
      if (!first_boot) {
        lv_label_set_text(u.hint, "\xC3\x89talonnage annul\xC3\xA9 : l'ancien est conserv\xC3\xA9.");
        lv_obj_add_flag(u.cross, LV_OBJ_FLAG_HIDDEN);
        cal_pump_for(2000);
        break;
      }
      lv_label_set_text(u.hint, more ? "Nouvel essai\xE2\x80\xA6" : "");
    }
    lv_obj_add_flag(u.cross, LV_OBJ_FLAG_HIDDEN);
    cal_pump_for(saved ? 1800 : 2200);
    if (!saved && s_cal_abort) { do_abort(); break; }
  }

  if (!saved && !aborted && first_boot) {
    lv_label_set_text(u.step, "Non \xC3\xA9talonn\xC3\xA9");
    lv_label_set_text(u.hint, "L'\xC3\xA9talonnage sera redemand\xC3\xA9 au prochain d\xC3\xA9marrage.");
    cal_pump_for(3000);
  }

  lv_scr_load(return_scr);
  lv_obj_del(u.scr);
  // Dessine tout de suite l'ecran de retour : au premier demarrage, la
  // lecture de la carte SD qui suit bloque LVGL un moment, et l'ecran
  // d'etalonnage resterait sinon affiche pendant ce temps.
  lv_refr_now(NULL);
  set_inputs_enabled(true);
  return saved;
}

// ============================================================================
// Test du tactile : une croix suit le doigt, 5 cercles de repere
// ============================================================================
static volatile bool s_test_done = false;
static const int TEST_RX[5] = {30, 290, 30, 290, 160};
static const int TEST_RY[5] = {30, 30, 210, 210, 120};

static void test_bg_draw_cb(lv_event_t *e) {
  lv_obj_t *obj = lv_event_get_target(e);
  lv_draw_ctx_t *dc = lv_event_get_draw_ctx(e);
  lv_area_t a;
  lv_obj_get_coords(obj, &a);

  lv_draw_rect_dsc_t line;
  lv_draw_rect_dsc_init(&line);
  line.bg_color = C(COL_SURF2);
  line.bg_opa = LV_OPA_COVER;
  lv_area_t r;
  for (int x = 40; x < DISPLAY_WIDTH; x += 40) {
    r.x1 = (lv_coord_t)(a.x1 + x); r.x2 = r.x1;
    r.y1 = a.y1; r.y2 = a.y2;
    lv_draw_rect(dc, &line, &r);
  }
  for (int y = 40; y < DISPLAY_HEIGHT; y += 40) {
    r.x1 = a.x1; r.x2 = a.x2;
    r.y1 = (lv_coord_t)(a.y1 + y); r.y2 = r.y1;
    lv_draw_rect(dc, &line, &r);
  }

  lv_draw_rect_dsc_t ring;
  lv_draw_rect_dsc_init(&ring);
  ring.bg_opa = LV_OPA_TRANSP;
  ring.radius = LV_RADIUS_CIRCLE;
  ring.border_color = C(COL_MUT);
  ring.border_width = 2;
  ring.border_opa = LV_OPA_COVER;
  for (int i = 0; i < 5; i++) {
    r.x1 = (lv_coord_t)(a.x1 + TEST_RX[i] - 11); r.x2 = (lv_coord_t)(r.x1 + 21);
    r.y1 = (lv_coord_t)(a.y1 + TEST_RY[i] - 11); r.y2 = (lv_coord_t)(r.y1 + 21);
    lv_draw_rect(dc, &ring, &r);
  }
}

static void run_touch_test_ui() {

  if (s_keypad_indev) lv_indev_enable(s_keypad_indev, false);
  s_test_done = false;

  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_remove_style_all(scr);
  lv_obj_set_style_bg_color(scr, C(COL_BG), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // Quadrillage de 40 px et 5 cercles de repere : dessines directement par
  // l'ecran (un seul objet LVGL au lieu de 17), pour menager le tas LVGL
  // quand la liste contient beaucoup de jeux.
  lv_obj_add_event_cb(scr, test_bg_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

  lv_obj_t *t = make_label(scr, "Test du tactile", &ui_font_14, COL_TXT);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 50);
  lv_obj_t *h = make_label(scr, "Touchez les cercles : le point vert doit tomber au centre.", &ui_font_12, COL_MUT);
  lv_obj_set_width(h, 220);
  lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 70);
#if HAS_PHYSICAL_CONTROLLER
  lv_obj_t *info = make_label(scr, "Manette : B ou START pour quitter", &ui_font_12, COL_MUT);
#else
  lv_obj_t *info = make_label(scr, "", &ui_font_12, COL_ACC_TXT);
#endif
  lv_obj_set_width(info, 240);
  lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(info, LV_ALIGN_TOP_MID, 0, 150);

  lv_obj_t *vline = make_plain(scr);
  lv_obj_set_size(vline, 1, DISPLAY_HEIGHT);
  lv_obj_set_style_bg_color(vline, C(COL_ACC), 0);
  lv_obj_set_style_bg_opa(vline, LV_OPA_60, 0);
  lv_obj_t *hline = make_plain(scr);
  lv_obj_set_size(hline, DISPLAY_WIDTH, 1);
  lv_obj_set_style_bg_color(hline, C(COL_ACC), 0);
  lv_obj_set_style_bg_opa(hline, LV_OPA_60, 0);
  lv_obj_t *dot = make_plain(scr);
  lv_obj_set_size(dot, 10, 10);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(dot, C(COL_ACC), 0);
  lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
  lv_obj_add_flag(vline, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(hline, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t *done = make_button(scr, "Termin\xC3\xA9", COL_SURF2, COL_TXT);
  lv_obj_set_size(done, 96, 32);
  lv_obj_align(done, LV_ALIGN_BOTTOM_MID, 0, -8);
  lv_obj_add_event_cb(done, flag_event, LV_EVENT_CLICKED, (void *)&s_test_done);

  lv_obj_t *prev = lv_scr_act();
  lv_scr_load(scr);
  if (s_touch_indev) lv_indev_reset(s_touch_indev, NULL);

  // Sortie : "Termine", B/START sur la manette, 20 s sans activite
  // (nouvel appui ou deplacement de plus de 3 px), ou 2 minutes au plus.
  // Un contact immobile ne compte pas : une dalle qui signale un appui
  // permanent ne bloque pas cet ecran.
  cal_abort_reset();
  const uint32_t t_open = millis();
  uint32_t last_activity = t_open;
  int lx = -1, ly = -1, ax = -100, ay = -100;
  bool was_down = false;
  while (!s_test_done && !s_cal_abort) {
    cal_pump();
    int x, y;
    bool down = ft6336_touch_read(&x, &y);
    if (down) {
      if (!was_down || abs(x - ax) > 3 || abs(y - ay) > 3) {
        last_activity = millis();
        ax = x; ay = y;
      }
      if (x != lx || y != ly) {
        lx = x; ly = y;
        lv_obj_set_pos(vline, x, 0);
        lv_obj_set_pos(hline, 0, y);
        lv_obj_set_pos(dot, x - 5, y - 5);
        lv_obj_clear_flag(vline, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(hline, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_HIDDEN);
        int best = 9999;
        for (int i = 0; i < 5; i++) {
          int dx = x - TEST_RX[i], dy = y - TEST_RY[i];
          int d = (int)lroundf(sqrtf((float)(dx * dx + dy * dy)));
          if (d < best) best = d;
        }
        lv_obj_set_style_text_color(info, C(COL_ACC_TXT), 0);
        if (best <= 30) lv_label_set_text_fmt(info, "x = %d   y = %d   \xC3\xA9" "cart au cercle : %d px", x, y, best);
        else lv_label_set_text_fmt(info, "x = %d   y = %d", x, y);
      }
    }
    was_down = down;
    if (millis() - last_activity > 20000) break;   // 20 s sans activite : on sort
    if (millis() - t_open > 120000) break;         // garde-fou : 2 minutes au plus
  }

  lv_scr_load(prev);
  lv_obj_del(scr);
  if (s_keypad_indev) {
    lv_indev_enable(s_keypad_indev, true);
    lv_indev_reset(s_keypad_indev, NULL);
#if HAS_PHYSICAL_CONTROLLER
    s_kp_prev = controller_read_physical_buttons();   // B/START deja consommes ici
#endif
  }
  if (s_touch_indev) {
    lv_indev_reset(s_touch_indev, NULL);
    s_touch_wait_release = true;
  }
}

static void print_lvgl_memory(const char *when) {
#if LV_MEM_CUSTOM == 0
  lv_mem_monitor_t mon;
  lv_mem_monitor(&mon);
  Serial.printf("[UI] Memoire LVGL (%s) : %lu octets libres sur %lu (%d %% utilises)\n",
                when, (unsigned long)mon.free_size, (unsigned long)mon.total_size, (int)mon.used_pct);
#else
  (void)when;
#endif
}

// ============================================================================
// Point d'entree
// ============================================================================
// Applique un autre theme tout de suite. Appele depuis la boucle principale
// (jamais depuis un rappel LVGL) : l'interface est detruite puis reconstruite
// avec les nouvelles couleurs, et l'on revient sur Reglages a la meme hauteur.
// Les reglages eux-memes (volume, Wi-Fi, filtre de recherche...) ne sont pas
// touches.
static void ui_apply_theme(int id) {
  if (id < 0 || id >= THEME_COUNT || id == s_theme_id || !s_scr) return;
  lv_coord_t sy = s_settings_col ? lv_obj_get_scroll_y(s_settings_col) : 0;
  theme_select(id);
  theme_save();

  // 1. Ecran provisoire sans style partage, aux couleurs du nouveau theme
  lv_obj_t *tmp = lv_obj_create(NULL);
  lv_obj_remove_style_all(tmp);
  lv_obj_set_style_bg_color(tmp, C(COL_BG), 0);
  lv_obj_set_style_bg_opa(tmp, LV_OPA_COVER, 0);
  lv_scr_load(tmp);

  // 2. Tout ce qui utilise les styles partages disparait (fiche de jeu et
  // message bref compris : ils sont sur lv_layer_top()).
  lv_obj_clean(lv_layer_top());
  s_sheet = NULL;
  s_sheet_confirm = false;
  s_keypad_lr_nav = false;
  lv_obj_del(s_scr);
  s_scr = NULL;
  s_settings_col = NULL;
  s_theme_bm = NULL;
  s_ap_info = NULL;        // Detruite avec l'ecran (deja remis a NULL par ap_info_deleted_cb)
  s_ap_info_key = -1;
  for (int i = 0; i < TAB_COUNT; i++) {
    if (s_groups[i]) lv_group_del(s_groups[i]);
    s_groups[i] = NULL;
  }

  // 3. Styles et theme LVGL aux nouvelles couleurs, puis reconstruction
  text_styles_reset();
  styles_init();
  theme_lvgl_apply();
  build_ui();
  lv_obj_del(tmp);
  show_tab(TAB_SETTINGS);
  if (s_settings_col) {
    lv_obj_update_layout(s_settings_col);
    lv_obj_scroll_to_y(s_settings_col, sy, LV_ANIM_OFF);
  }
  if (s_theme_req_keypad && s_theme_bm) {
    lv_group_focus_obj(s_theme_bm);
    lv_obj_add_state(s_theme_bm, LV_STATE_FOCUS_KEY);
  }
  Serial.printf("[UI] Theme applique : %s\n", s_th->name);
  print_lvgl_memory("theme applique");
}

int ui_lvgl_run_menu() {
  lvgl_init_once();

  lv_obj_t *wait = show_wait_screen("Chargement des jeux\xE2\x80\xA6", "");

  // Premier demarrage (ou NVS effacee) : etalonnage obligatoire, avant tout
  // le reste - sans lui, les appuis tomberaient n'importe ou.
  if (g_touch_available && g_touch_calibration_needed) {
    run_touch_calibration_ui(true, wait);
  }

  osd_games_rescan_quiet();
  Serial.printf("[MENU] %d jeu(x) trouve(s)\n", osd_games_count());
  video_norm_load();

  build_ui();
  lv_obj_del(wait);
  print_lvgl_memory("menus construits");
  {   // Jeu ecarte apres un plantage, reprise effacee...
    const char *note = osd_take_notice();
    if (note) toast_show_ex(note, true, 8000, true);
  }

  s_launch_index = -1;

  while (true) {
    ui_tick();
    lv_timer_handler();
    if (g_display_tv_mode) video_link_tick(false);   // Presence de la console (1 fois/s)

    // Un jeu touche ? Traite AVANT toute relecture de la carte SD par le
    // portail, sinon l'index pourrait designer un autre jeu.
    if (s_launch_index >= 0) {
      int idx = s_launch_index;
      s_launch_index = -1;
      const bool from_phone = s_launch_from_phone;
      s_launch_from_phone = false;
      // Controle du jeu avant de quitter les menus.
      int mapper = -1;
      const int pc = (idx < osd_games_count()) ? osd_game_precheck(idx, &mapper) : OSD_PRECHECK_OK;
      const bool resume = s_launch_resume;   // Reprise automatique
      s_launch_resume = false;
      if (pc != OSD_PRECHECK_OK) {
        Serial.printf("[UI] Lancement refuse (%s) : %s\n", osd_precheck_text(pc, mapper), osd_game_filename(idx));
        toast_show(osd_precheck_text(pc, mapper), true);
        if (from_phone) {
          games_link_report_refused(pc == OSD_PRECHECK_MAPPER ? GAMES_LINK_REFUSE_MAPPER
                                    : pc == OSD_PRECHECK_NOFILE ? GAMES_LINK_REFUSE_NOFILE
                                    : GAMES_LINK_REFUSE_NOT_NES, mapper);
        }
        idx = osd_games_count();   // rien a lancer
      } else if (idx < osd_games_count()) {
        games_link_report_loading(idx);   // perdu si la page n'est pas la
      }
      if (idx < osd_games_count()) {
        osd_volume_commit();
        bl_commit();
        char name[64];
        snprintf(name, sizeof(name), "%s", osd_game_filename(idx));
        size_t nl = strlen(name);
        if (nl > 4 && strcasecmp(name + nl - 4, ".nes") == 0) name[nl - 4] = '\0';
        show_wait_screen("Lancement\xE2\x80\xA6", name);
        tv_show_launch(name);
        Serial.printf("[UI] Jeu choisi : %s%s\n", osd_game_filename(idx), resume ? " (reprise)" : "");
        osd_request_resume(resume);
        // LVGL ne sera plus appele jusqu'au prochain redemarrage : on rend
        // le tampon de dessin (l'image "Lancement..." reste a l'ecran).
        heap_caps_free(s_buf1);
        s_buf1 = NULL;
        return idx;
      }
    }

#if ENABLE_WIFI_PORTAL
    if (s_wifi_enable_at && (int32_t)(millis() - s_wifi_enable_at) >= 0) {
      s_wifi_enable_at = 0;
#if ENABLE_BLE_GAMEPAD
      // Le telephone va etre deconnecte (Bluetooth arrete tant que le Wi-Fi
      // est allume).
      if (bluetooth_controller_is_connected())
        toast_show("Manette du t\xC3\xA9l\xC3\xA9phone coup\xC3\xA9" "e", false);
#endif
      lv_refr_now(NULL);         // interrupteur dessine en position ON
      wifi_boot_enable();        // bloquant jusqu'a ~10 s
      update_wifi_ui();
    }
    wifi_portal_loop();
    // Reseau de la box enregistre depuis le portail (connexion tout de suite,
    // 1 s apres la reponse) ou oublie depuis les Reglages
    {
      static uint32_t reconnect_at = 0;
      if (g_portal_wifi_reconnect) {
        g_portal_wifi_reconnect = false;
        reconnect_at = (millis() + 1000) | 1;
      }
      if (reconnect_at && (int32_t)(millis() - reconnect_at) >= 0) {
        reconnect_at = 0;
        wifi_net_update();
        toast_show(LV_SYMBOL_WIFI "  Connexion au r\xC3\xA9seau enregistr\xC3\xA9\xE2\x80\xA6", false);
        if (g_wifi_boot_status == WIFI_BOOT_DISABLED) {
          wifi_ui_show_connecting();
          s_wifi_enable_at = millis() + 300;
          if (s_wifi_enable_at == 0) s_wifi_enable_at = 1;
        } else {
          wifi_restart_if_on();
        }
      }
      if (s_req_wifi_forget) {
        s_req_wifi_forget = false;
        wifi_boot_forget_credentials();
        wifi_net_update();
        toast_show("R\xC3\xA9seau oubli\xC3\xA9 : retour \xC3\xA0 celui de wifi_config.h", false);
        wifi_restart_if_on();
      }
    }
    // Pas pendant la saisie d'une recherche - la liste est reconstruite a la
    // fermeture du clavier (compteur et apercu restent coherents, et le tas
    // LVGL ne porte pas les deux a la fois).
    // Une fiche de jeu ouverte reste ouverte : ses actions retrouvent le jeu
    // par son NOM et son emplacement dans la liste relue (sheet_find_game) -
    // "Jeu introuvable" s'il a disparu entre-temps.
    if (g_portal_games_changed && !s_search_scr) {
      // Envoi ou suppression reussi depuis le portail web : relecture de la
      // carte SD et reconstruction de la liste.
      g_portal_games_changed = false;
      games_reload(-1);
      print_lvgl_memory("liste reconstruite");
    }
    if ((int)g_wifi_boot_status != s_last_wifi_status) update_wifi_ui();
#endif

    // Bouton "MENU JEUX" du telephone -> ecran d'accueil de la selection des
    // jeux, tel qu'apres un retour de partie : ecran QR, recherche (texte tape
    // abandonne) et fiche de jeu fermes, filtre de recherche efface, onglet
    // Jeux en haut de la liste complete. Un ecran bloquant (etalonnage, test
    // du tactile) se termine avant ce retour.
    if (bluetooth_controller_take_menu_request()) {
      Serial.println("[MENU] bouton MENU du telephone -> liste des jeux");
      if (s_qr_scr) qr_screen_close();
      if (s_search_scr) search_screen_close(false);
      if (s_sheet) sheet_close();
      if (s_pages) pages_close();
      if (s_quar) quar_close();
      if (s_help) help_close();
      s_search[0] = '\0';
      show_tab(TAB_GAMES);
      if (osd_games_query()[0] || osd_games_page() != 0) {
        osd_games_set_view("", 0);   // Liste complete, premiere page
        build_games_panel();
      } else {
        apply_search_filter();       // deja la liste complete : rien a reconstruire
      }
    }

    // Liste des jeux et lancement depuis le panneau du telephone. PAUSE /
    // REPRISE n'ont pas de sens dans les menus : ignorees.
    {
      int pidx = -1;
      const uint8_t gen_before = osd_games_generation();
      const int ga = games_link_poll(GAMES_LINK_STATE_MENU, -1, &pidx);
      // Jeu choisi dans la liste COMPLETE du telephone, sur une autre page :
      // la console affiche cette page-la (recherche effacee si le jeu n'y
      // correspondait pas).
      const bool view_changed = osd_games_generation() != gen_before;
      if (view_changed) {
        if (s_qr_scr) qr_screen_close();
        if (s_search_scr) search_screen_close(false);
        if (s_sheet) sheet_close();
        if (s_pages) pages_close();
        if (s_quar) quar_close();
        if (s_help) help_close();
        snprintf(s_search, sizeof(s_search), "%s", osd_games_query());
        if (s_tab != TAB_GAMES) show_tab(TAB_GAMES);
        build_games_panel();
        Serial.printf("[UI] Liste : page %d/%d (jeu choisi sur le telephone)\n", osd_games_page() + 1,
                      osd_games_page_count());
      }
      if (ga == GAMES_LINK_TVLIST) {
        // MENU JEUX du telephone en sortie Televiseur : la console revient sur
        // sa liste des jeux (recopiee sur la television).
        if (s_qr_scr) qr_screen_close();
        if (s_search_scr) search_screen_close(false);
        if (s_sheet) sheet_close();
        if (s_pages) pages_close();
        if (s_quar) quar_close();
        if (s_help) help_close();
        if (s_tab != TAB_GAMES) show_tab(TAB_GAMES);
        Serial.println("[MENU] MENU JEUX du telephone : liste des jeux (television).");
      }
      if (ga == GAMES_LINK_LAUNCH && s_launch_index < 0) {
        Serial.printf("[MENU] Lancement demande par le telephone : %s\n", osd_game_filename(pidx));
        if (s_qr_scr) qr_screen_close();
        if (s_search_scr) search_screen_close(false);
        if (s_sheet) sheet_close();
        if (s_pages) pages_close();
        if (s_quar) quar_close();
        if (s_help) help_close();
        s_launch_from_phone = true;
        s_launch_index = pidx;   // lance au tour suivant de la boucle (controle compris)
        s_launch_resume = games_link_launch_resume();   // "Reprendre" du telephone
        // Le jeu demande est montre dans la liste (utile s'il est refuse)
        if (view_changed && pidx >= 0 && pidx < s_rows_built) tel_cursor_set(pidx);
      }
    }

    // Favoris changes depuis le telephone : etoiles des lignes a jour (liste
    // des favoris refaite si c'est elle qui est affichee)
    {
      static uint32_t lib_gen = osd_library_generation();
      if (osd_library_generation() != lib_gen) {
        lib_gen = osd_library_generation();
        bool changed = false;
        for (int i = 0; i < s_rows_built; i++) {
          const uint8_t f = (uint8_t)((s_game_flags[i] & ~GF_FAV) | (osd_game_is_fav(i) ? GF_FAV : 0));
          if (f != s_game_flags[i]) { s_game_flags[i] = f; changed = true; }
        }
        if (changed && s_games_col) lv_obj_invalidate(s_games_col);
        if (osd_games_mode() == OSD_VIEW_FAVS) {
          if (s_sheet) s_list_dirty = true;
          else if (!s_search_scr && !s_pages && !s_qr_scr) {
            osd_games_set_view(s_search, osd_games_page());
            build_games_panel();
          }
        }
      }
    }

    // Croix, A et START du telephone dans la liste des jeux
    tel_nav_poll();
    tv_mirror_poll(false);   // La liste aussi sur la television
    update_bt_ui();   // Icone Bluetooth de la barre du haut

    // Partage de la radio entre le point d'acces et le Bluetooth (demarrage
    // differe, pause ou reprise) et ligne d'information de l'onglet Portail,
    // deux fois par seconde.
    {
      static uint32_t next_radio_check = 0;
      if ((int32_t)(millis() - next_radio_check) >= 0) {
        next_radio_check = millis() + 500;
        bluetooth_controller_radio_check();
        portal_ap_info_update();
      }
    }

    // Ecran QR plein format ouvert : B (manette) le ferme, SELECT/START
    // sont ignores.
    if (s_qr_scr) {
      if (s_req_tab_games) s_req_qr_close = true;
      s_req_tab_games = false;
      s_req_next_tab = false;
      s_req_tab_video = false;
    }
    // Ecran de recherche ouvert - B ou START (manette) valident la recherche
    // comme OK ; SELECT est ignore.
    if (s_search_scr) {
      if (s_req_tab_games || s_req_tab_video) s_req_search_close = true;
      s_req_tab_games = false;
      s_req_next_tab = false;
      s_req_tab_video = false;
    }
    if (s_req_search_close) { s_req_search_close = false; search_screen_close(true); }
    // Page suivante/precedente ou filtre efface (bouton de la liste, croix du
    // telephone au bout de la page)
    if (s_req_view) {
      s_req_view = false;
      const int cur = s_req_view_cursor;
      s_req_view_cursor = -2;
      if (!s_search_scr && !s_qr_scr && !s_sheet && s_tab == TAB_GAMES) {
        osd_games_set_view(s_search, s_req_view_page);
        build_games_panel();
        Serial.printf("[UI] Liste : page %d/%d (%d jeu(x))\n", osd_games_page() + 1, osd_games_page_count(), osd_games_matching());
        if (cur != -2 && s_rows_built > 0) tel_cursor_set(cur < 0 ? s_rows_built - 1 : 0);
      }
    }
    if (s_req_search_open) { s_req_search_open = false; if (!s_search_scr && !s_qr_scr && !s_sheet && !s_pages) search_screen_open(); }

    // Tous / Favoris / Recents
    if (s_req_mode >= 0) {
      const int m = s_req_mode;
      s_req_mode = -1;
      if (!s_search_scr && !s_qr_scr && !s_sheet && !s_pages && s_tab == TAB_GAMES && m != osd_games_mode()) {
        osd_games_set_mode(m);
        build_games_panel();
        Serial.printf("[UI] Liste : %s (%d jeu(x))\n", m == OSD_VIEW_FAVS ? "favoris" : m == OSD_VIEW_RECENTS ? "recents" : "tous",
                      osd_games_matching());
        if (s_keypad_indev && s_mode_bm) {   // la manette reste sur les onglets de liste
          lv_group_focus_obj(s_mode_bm);
          lv_obj_add_state(s_mode_bm, LV_STATE_FOCUS_KEY);
        }
      }
    }
    // Carte "Reprendre"
    if (s_req_resume) {
      s_req_resume = false;
      if (!s_search_scr && !s_qr_scr && !s_sheet && !s_pages && s_tab == TAB_GAMES && s_launch_index < 0) {
        bool has_state = false;
        const int ci = osd_resume_catalog(&has_state);
        if (ci >= 0) {
          const uint8_t g0 = osd_games_generation();
          const int i = osd_games_show_catalog(ci);
          if (osd_games_generation() != g0) {
            snprintf(s_search, sizeof(s_search), "%s", osd_games_query());
            build_games_panel();
          }
          if (i >= 0) {
            s_launch_index = i;
            s_launch_resume = has_state;
          }
        }
      }
    }
    // "Aller a la page"
    if (s_req_pages_open) {
      s_req_pages_open = false;
      if (!s_sheet && !s_search_scr && !s_qr_scr && !s_pages && s_tab == TAB_GAMES) pages_open();
    }
    if (s_pages) {   // B ou START ferment la grille ; SELECT est ignore
      if (s_req_tab_games || s_req_tab_video) s_req_pages_close = true;
      s_req_tab_games = false;
      s_req_next_tab = false;
      s_req_tab_video = false;
      pages_info_update();
    }
    if (s_req_pages_go >= 0) {
      const int p = s_req_pages_go;
      s_req_pages_go = -1;
      if (s_pages) {
        pages_close();
        if (p != osd_games_page()) {
          s_req_view_page = p;
          s_req_view_cursor = 0;   // curseur du telephone sur le 1er jeu de la page
          s_req_view = true;
        }
      }
    }
    if (s_req_pages_close) { s_req_pages_close = false; pages_close(); }

    // Reglages > Aide
    if (s_req_help_open) { s_req_help_open = false; if (!s_help && !s_quar && !s_sheet && !s_pages && !s_qr_scr && !s_search_scr) help_open(); }
    if (s_help) {   // B ou START ferment l'aide
      if (s_req_tab_games || s_req_tab_video) s_req_help_close = true;
      s_req_tab_games = false;
      s_req_next_tab = false;
      s_req_tab_video = false;
    }
    if (s_req_help_close) { s_req_help_close = false; help_close(); }

    // Reglages > Jeux ecartes
    if (s_req_quar_open) {
      s_req_quar_open = false;
      if (!s_quar && !s_sheet && !s_pages && !s_qr_scr && !s_search_scr) {
        osd_quarantine_refresh();
        quar_label_update();
        s_quar_sel = -1;
        quar_build();
      }
    }
    if (s_quar) {   // B ou START : retour a la liste, puis fermeture
      if (s_req_tab_games || s_req_tab_video) {
        if (s_quar_sel >= 0) s_req_quar_back = true;
        else s_req_quar_close = true;
      }
      s_req_tab_games = false;
      s_req_next_tab = false;
      s_req_tab_video = false;
    }
    if (s_req_quar_pick >= 0) {
      const int i = s_req_quar_pick;
      s_req_quar_pick = -1;
      if (s_quar) { s_quar_sel = i; quar_build(); }
    }
    if (s_req_quar_back) { s_req_quar_back = false; if (s_quar) { s_quar_sel = -1; quar_build(); } }
    if ((s_req_quar_restore || s_req_quar_delete) && s_quar && s_quar_sel >= 0) {
      const bool restore = s_req_quar_restore;
      char nm[64], msg[120];
      game_display_name(osd_quarantine_name(s_quar_sel), nm, sizeof(nm));
      const char *err = NULL;
      if (restore) err = osd_quarantine_restore(s_quar_sel);
      else if (!osd_quarantine_delete(s_quar_sel)) err = "Suppression impossible";
      if (err) {
        toast_show_ex(err, true, 4000, true);
      } else {
        snprintf(msg, sizeof(msg), restore ? LV_SYMBOL_OK "  Remis dans la liste des jeux" : LV_SYMBOL_OK "  Jeu supprim\xC3\xA9");
        Serial.printf("[UI] Jeu ecarte %s : %s\n", restore ? "remis" : "supprime", nm);
        osd_quarantine_refresh();
        quar_label_update();
        s_quar_sel = -1;
        quar_build();
        if (restore) games_reload(-1);
        toast_show(msg, false);
      }
    }
    s_req_quar_restore = false;
    s_req_quar_delete = false;
    if (s_req_quar_close) { s_req_quar_close = false; quar_close(); }

    // Fiche d'un jeu ouverte - B ou START la ferment ; SELECT est ignore.
    if (s_sheet) {
      if (s_req_tab_games || s_req_tab_video) s_req_sheet_close = true;
      s_req_tab_games = false;
      s_req_next_tab = false;
      s_req_tab_video = false;
    }
    if (s_req_sheet_open >= 0) {
      int i = s_req_sheet_open;
      s_req_sheet_open = -1;
      if (!s_sheet && !s_pages && !s_search_scr && !s_qr_scr && s_tab == TAB_GAMES) sheet_open(i);
    }
    if (s_req_sheet_play || s_req_sheet_resume) {
      const bool resume = s_req_sheet_resume;
      s_req_sheet_play = false;
      s_req_sheet_resume = false;
      int i = sheet_find_game();
      s_list_dirty = false;   // la liste sera refaite au retour du jeu
      sheet_close();
      if (i >= 0) {
        s_launch_index = i;   // lance au tour suivant de la boucle
        s_launch_resume = resume;
      }
    }
    if (s_req_sheet_fav) {   // Etoile de la fiche
      s_req_sheet_fav = false;
      const int i = s_sheet && !s_sheet_confirm ? sheet_find_game() : -2;
      if (i == -1) {
        toast_show("Jeu introuvable (d\xC3\xA9j\xC3\xA0 supprim\xC3\xA9 ?)", true);
      } else if (i >= 0 && !osd_game_is_fav(i) && osd_fav_full()) {
        toast_show(LV_SYMBOL_WARNING "  Favoris pleins (60 au plus)", true);
      } else if (i >= 0) {
        const bool now = osd_game_toggle_fav(i);
        if (i < s_rows_built) {
          if (now) s_game_flags[i] |= GF_FAV;
          else s_game_flags[i] &= (uint8_t)~GF_FAV;
          if (s_games_col) lv_obj_invalidate(s_games_col);
        }
        if (osd_games_mode() == OSD_VIEW_FAVS) s_list_dirty = true;
        sheet_build();
        if (s_keypad_indev && s_sheet_star) {
          lv_group_focus_obj(s_sheet_star);
          lv_obj_add_state(s_sheet_star, LV_STATE_FOCUS_KEY);
        }
        // pas de message : l'etoile et "Favori" de la fiche changent sous le doigt
        Serial.printf("[UI] Favori %s : %s\n", now ? "ajoute" : "retire", s_sheet_name);
      }
    }
    if (s_req_sheet_delete) { s_req_sheet_delete = false; if (s_sheet) sheet_delete_now(); }
    if (s_req_sheet_confirm) { s_req_sheet_confirm = false; if (s_sheet) { s_sheet_confirm = true; sheet_build(); } }
    if (s_req_sheet_close) { s_req_sheet_close = false; sheet_close(); }

    if (s_req_qr_close) { s_req_qr_close = false; s_req_qr_next = false; qr_screen_close(); }
    if (s_req_qr_next) { s_req_qr_next = false; if (s_qr_scr) { s_qr_page++; qr_screen_build(); } }
    if (s_req_qr_full) { s_req_qr_full = false; if (!s_qr_scr) qr_screen_open(); }

    if (s_req_calibrate) {
      s_req_calibrate = false;
      osd_volume_commit();
      bl_commit();
      run_touch_calibration_ui(false, s_scr);
    }
    if (s_req_touch_test) { s_req_touch_test = false; run_touch_test_ui(); }

    if (s_req_theme >= 0) { int t = s_req_theme; s_req_theme = -1; ui_apply_theme(t); }

    if (s_req_next_tab) { s_req_next_tab = false; show_tab((s_tab + 1) % TAB_COUNT); }
    if (s_req_tab_games) { s_req_tab_games = false; show_tab(TAB_GAMES); }
    if (s_req_tab_video) { s_req_tab_video = false; show_tab(TAB_VIDEO); }

    delay(5);
  }
}

#endif  // USE_LVGL_UI
