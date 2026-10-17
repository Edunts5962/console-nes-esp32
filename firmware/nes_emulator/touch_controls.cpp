// touch_controls.cpp - zones tactiles des bandes laterales (manette a
// l'ecran pendant une partie, ancienne interface des menus) et etalonnage
// du tactile au premier demarrage. Voir touch_controls.h.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <string.h>
#include "driver/gpio.h"
#include "hw_config.h"
#include "tft_driver.h"
#include "ft6336_touch.h"
#include "audio_codec.h"
#include "touch_controls.h"
#include "video_link.h"  // ENABLE_VIDEO_LINK, pour l'etiquette "Sortie" ci-dessous
#include "wifi_boot.h"   // ENABLE_WIFI_PORTAL, pour l'etiquette "WIFI" ci-dessous

#define NES_A 0x01
#define NES_B 0x02
#define NES_SELECT 0x04
#define NES_START 0x08
#define NES_UP 0x10
#define NES_DOWN 0x20
#define NES_LEFT 0x40
#define NES_RIGHT 0x80

extern TFT_eSPI my_lcd;
extern TFTDriver tft;

// ==================== Etalonnage ====================

static bool wait_for_tap(int screen_x, int screen_y, int *raw_x, int *raw_y) {
  my_lcd.fillScreen(0xFFFF);
  my_lcd.setTextColor(0x0000, 0xFFFF);
  my_lcd.setTextSize(2);
  my_lcd.setCursor(20, 20);
  my_lcd.print("Etalonnage tactile");
  my_lcd.setTextSize(1);
  my_lcd.setCursor(20, 50);
  my_lcd.print("Touche precisement la croix");

  my_lcd.drawLine(screen_x - 10, screen_y, screen_x + 10, screen_y, 0xF800);
  my_lcd.drawLine(screen_x, screen_y - 10, screen_x, screen_y + 10, 0xF800);
  my_lcd.drawCircle(screen_x, screen_y, 6, 0xF800);

  unsigned long start = millis();
  bool was_touched = false;
  while (millis() - start < 20000) {  // 20 s max par point, sinon abandon
    int x, y;
    bool touched = ft6336_touch_get_raw(&x, &y);
    if (touched) {
      was_touched = true;
      *raw_x = x;
      *raw_y = y;
    } else if (was_touched) {
      // Doigt relache juste apres un contact : capture au relachement,
      // plus stable qu'au premier contact (qui peut etre bruite).
      return true;
    }
    delay(20);
  }
  return false;  // timeout : rien touche a temps
}

bool g_touch_available = false;
bool g_touch_calibration_needed = false;

// Renvoie true seulement si les 3 points ont ete touches ET que l'etalonnage a
// ete accepte (points non alignes).
[[maybe_unused]] static bool run_touch_calibration() {   // inutilisee si USE_LVGL_UI = 1
  const int margin = 30;
  struct {
    int sx, sy, rx, ry;
  } pts[3] = {
    { margin, margin, 0, 0 },                          // haut-gauche
    { DISPLAY_WIDTH - margin, margin, 0, 0 },           // haut-droite
    { margin, DISPLAY_HEIGHT - margin, 0, 0 },          // bas-gauche
  };

  for (int i = 0; i < 3; i++) {
    if (!wait_for_tap(pts[i].sx, pts[i].sy, &pts[i].rx, &pts[i].ry)) {
      Serial.println("[TOUCH] Etalonnage : pas de contact detecte a temps, rien n'est enregistre");
      my_lcd.fillScreen(0xFFFF);
      return false;
    }
    delay(300);  // evite de capturer le rebond du meme appui sur le point suivant
  }

  bool ok = ft6336_touch_set_calibration(
    pts[0].rx, pts[0].ry, pts[0].sx, pts[0].sy,
    pts[1].rx, pts[1].ry, pts[1].sx, pts[1].sy,
    pts[2].rx, pts[2].ry, pts[2].sx, pts[2].sy);
  if (!ok) {
    Serial.println("[TOUCH] Etalonnage : points incoherents, rien n'est enregistre");
    my_lcd.fillScreen(0xFFFF);
    return false;
  }

  my_lcd.fillScreen(0xFFFF);
  my_lcd.setTextColor(0x0000, 0xFFFF);
  my_lcd.setTextSize(2);
  my_lcd.setCursor(20, 100);
  my_lcd.print("Etalonnage termine");
  delay(600);
  return true;
}

void touch_controls_init_and_calibrate() {
  Serial.println("[TOUCH] Initialisation du tactile FT6336...");

  if (!ft6336_touch_init(audio_i2c_bus_handle(), (gpio_num_t)TOUCH_RST, DISPLAY_WIDTH, DISPLAY_HEIGHT)) {
    Serial.println("[TOUCH] Init FAILED - le tactile ne sera pas utilisable (74HC165 physique requis)");
    g_touch_available = false;
    return;
  }
  g_touch_available = true;

  // On essaie d'abord de recharger un etalonnage deja enregistre (NVS, espace
  // "touchcal") ; sinon on demande les 3 points.
  if (!ft6336_touch_load_calibration()) {
    Serial.println("[TOUCH] Aucun etalonnage enregistre - etalonnage initial requis");
#if USE_LVGL_UI
    // Fait par l'ecran d'etalonnage LVGL au lancement des menus (ui_lvgl.cpp),
    // qui moyenne les mesures et verifie le resultat.
    g_touch_calibration_needed = true;
#else
    // Rien n'est enregistre si l'etalonnage echoue (croix non touchee dans les
    // 20 s, ou points incoherents) : il sera redemande au prochain demarrage.
    if (run_touch_calibration()) {
      ft6336_touch_save_calibration();
    } else {
      g_touch_calibration_needed = true;
    }
#endif
  } else {
    Serial.println("[TOUCH] Etalonnage precedent recharge depuis la NVS");
  }
}

// ==================== Zones de boutons (bandes gauche/droite) ====================
//
// Bande gauche (x: 0..31), 4 zones de 60 px de haut : UP / LEFT / RIGHT / DOWN
// Bande droite (x: 288..319), 4 zones de 60 px de haut : A / B / START / SELECT
// Repartition arbitraire (juste labellee clairement a l'ecran par
// touch_controls_draw_gameplay_overlay()) - AUCUN lien avec l'ordre du
// 74HC165 (D0..D7), ce sont deux chemins d'entree independants qui
// aboutissent tous les deux au meme bitmask NES_* en sortie.
#define ZONE_LEFT_X0  0
#define ZONE_LEFT_X1  32
#define ZONE_RIGHT_X0 (DISPLAY_WIDTH - 32)
#define ZONE_RIGHT_X1 DISPLAY_WIDTH
#define ZONE_H (DISPLAY_HEIGHT / 4)

// ==================== Dessin generique des 2x4 zones ====================
//
// Facture commune aux trois ecrans (jeu / menu / reglages), qui ne
// different que par les etiquettes affichees dans chaque zone - la
// geometrie (2 bandes de 32 px, 4 hauteurs de ZONE_H) est toujours la
// meme, ce sont les memes zones que lit touch_controls_get_gamepad_state().
// Un libelle vide ("") laisse la zone en noir uni (zone tactile toujours
// active, juste sans fonction utile a cet ecran - ex. LEFT/RIGHT dans le
// menu).
struct TouchZoneLabel {
  const char *label;
  uint16_t color;
};

// Etiquettes en "badge" (fond arrondi colore) dessinees directement dans les
// bandes tactiles, la ou l'appui est lu par touch_controls_get_gamepad_state()
// : l'etiquette et le bouton ne font qu'un.
static void draw_zone_badge(int zone_x0, int zone_y, int zone_w, const char *label, uint16_t color) {
  if (!label || !label[0]) return;
  int text_w = (int)strlen(label) * 6 - 1;
  int badge_w = text_w + 8;
  if (badge_w > zone_w - 4) badge_w = zone_w - 4;  // ne deborde jamais de la bande de 32 px
  const int badge_h = 16;
  int bx = zone_x0 + (zone_w - badge_w) / 2;
  int by = zone_y + (ZONE_H - badge_h) / 2;
  tft.drawFilledRoundRect(bx, by, badge_w, badge_h, badge_h / 2, color);
  int text_x = bx + (badge_w - text_w) / 2;
  if (text_x < bx + 2) text_x = bx + 2;
  tft.drawString(text_x, by + 4, label, 0x0000, color, 1);
}

static void draw_zone_overlay(const TouchZoneLabel left[4], const TouchZoneLabel right[4]) {
  for (int i = 0; i < 4; i++) {
    int y = i * ZONE_H;
    tft.drawFilledRect(ZONE_LEFT_X0, y, ZONE_LEFT_X1 - ZONE_LEFT_X0, ZONE_H, 0x0000);
    draw_zone_badge(ZONE_LEFT_X0, y, ZONE_LEFT_X1 - ZONE_LEFT_X0, left[i].label, left[i].color);

    tft.drawFilledRect(ZONE_RIGHT_X0, y, ZONE_RIGHT_X1 - ZONE_RIGHT_X0, ZONE_H, 0x0000);
    draw_zone_badge(ZONE_RIGHT_X0, y, ZONE_RIGHT_X1 - ZONE_RIGHT_X0, right[i].label, right[i].color);
  }
}

void touch_controls_draw_gameplay_overlay() {
  const TouchZoneLabel left[4] = {
    { "UP", 0x07E0 }, { "LT", 0x07E0 }, { "RT", 0x07E0 }, { "DN", 0x07E0 }
  };
  const TouchZoneLabel right[4] = {
    { "A", 0xF800 }, { "B", 0xF800 }, { "ST", 0x7BEF }, { "SE", 0x7BEF }
  };
  draw_zone_overlay(left, right);
}

// UP/DOWN (bande gauche, zones 0 et 3) font defiler la liste de jeux ; A
// (bande droite, zone 0) lance le jeu selectionne ; SELECT (bande droite, zone
// 3) ouvre les reglages de volume - exactement le mapping deja lu par
// show_menu() dans osd.cpp (HW_MASK_UP/DOWN/A/SELECT), on ne fait ici
// qu'afficher ce qui etait deja actif. LEFT/RIGHT (gauche) et B/START (droite)
// ne servent a rien dans ce menu : zones laissees actives mais sans etiquette
// plutot que retirees, pour garder une seule fonction draw_zone_overlay()
// commune aux trois ecrans.
void touch_controls_draw_menu_overlay() {
  const TouchZoneLabel left[4] = {
    { "UP", 0x07E0 }, { "", 0 }, { "", 0 }, { "DN", 0x07E0 }
  };
  const TouchZoneLabel right[4] = {
    // START ouvre l'ecran "SORTIE VIDEO" (run_video_output_screen(), osd.cpp)
    // - etiquette "VIDEO", meme teinte grise-bleue que "OPTS" (SELECT) puisque
    // ce sont toutes les deux des zones qui ouvrent un ecran secondaire, pas
    // une action directe sur le jeu (contrairement a "PLAY", rouge).
    { "PLAY", 0xF800 }, { "", 0 }, { "VIDEO", 0x7BEF }, { "OPTS", 0x7BEF }
  };
  draw_zone_overlay(left, right);
}

// Ecran "SETTINGS" (sous-menu ouvert par SELECT depuis le menu principal, voir
// show_menu() dans osd.cpp) : UP/DOWN regle le volume, A ferme le sous-menu
// ("OK").
// LEFT/RIGHT bascule la sortie ECRAN LCD / TELEVISEUR - etiquetees "LCD"/"TV"
// uniquement si ENABLE_VIDEO_LINK vaut 1 (sinon ces zones restent actives mais
// muettes, puisque LEFT/RIGHT ne font toujours rien dans ce cas).
// B ne ferme plus le sous-menu quand ENABLE_VIDEO_LINK vaut 1 (voir osd.cpp) -
// il bascule la norme video NTSC/PAL, zone 1 de la bande droite, etiquetee
// "NORME". Sinon (ENABLE_VIDEO_LINK a 0), B ferme toujours le sous-menu ("OK"
// via A suffit a l'expliquer, comportement inchange dans ce cas).
//
// La zone START porte l'etiquette "WIFI" (dans les deux variantes, avec ou
// sans ENABLE_VIDEO_LINK) seulement si ENABLE_WIFI_PORTAL vaut 1 ; sinon elle
// reste muette (aucune fonction).
void touch_controls_draw_settings_overlay() {
#if ENABLE_VIDEO_LINK
  const TouchZoneLabel left[4] = {
    { "UP", 0x07E0 }, { "LCD", 0x7BEF }, { "TV", 0x7BEF }, { "DN", 0x07E0 }
  };
  const TouchZoneLabel right[4] = {
    { "OK", 0xF800 }, { "NORME", 0x7BEF },
#if ENABLE_WIFI_PORTAL
    // Etiquette "INFO" : le serveur du portail demarre tout seul des que le Wi-Fi
    // se connecte (wifi_boot.cpp) ; cette zone ouvre seulement l'ecran qui
    // affiche l'adresse a saisir dans un navigateur.
    { "INFO", 0x07FF },
#else
    { "", 0 },
#endif
    { "", 0 }
  };
#else
  const TouchZoneLabel left[4] = {
    { "UP", 0x07E0 },
#if ENABLE_WIFI_PORTAL
    { "WIFI", 0x07FF }, { "WIFI", 0x07FF },
#else
    { "", 0 }, { "", 0 },
#endif
    { "DN", 0x07E0 }
  };
  const TouchZoneLabel right[4] = {
    { "OK", 0xF800 }, { "B", 0xF800 },
#if ENABLE_WIFI_PORTAL
    // Meme renommage "WIFI" -> "INFO" que dans la branche ENABLE_VIDEO_LINK
    // ci-dessus, meme raison.
    { "INFO", 0x07FF },
#else
    { "", 0 },
#endif
    { "", 0 }
  };
#endif
  draw_zone_overlay(left, right);
}

// Ecran du portail : seule la zone B (bande droite, zone 1, comme sur les
// autres ecrans) est etiquetee - sans elle, "B: Quitter le portail" serait
// inaccessible sans manette physique. Les 7 autres zones restent muettes.
void touch_controls_draw_portal_overlay() {
  const TouchZoneLabel left[4] = {
    { "", 0 }, { "", 0 }, { "", 0 }, { "", 0 }
  };
  const TouchZoneLabel right[4] = {
    { "", 0 }, { "B", 0xF800 }, { "", 0 }, { "", 0 }
  };
  draw_zone_overlay(left, right);
}

// Ecran "SORTIE VIDEO" (run_video_output_screen(), osd.cpp) - voir
// touch_controls.h pour le contexte complet de cette fonction. LEFT et RIGHT
// (bande gauche, zones 1 et 2) portent la meme etiquette "SORTIE" (comme la
// bascule WiFi de l'ecran Reglages) puisque les deux zones font la meme action
// (basculer Sortie ECRAN LCD/TELEVISEUR) ; B (bande droite, zone 1) bascule la
// Norme NTSC/PAL ; A (bande droite, zone 0) ferme l'ecran ("FERMER" plutot que
// "OK", pour ne pas laisser croire qu'il valide un choix particulier - les
// deux bascules s'appliquent immediatement, il n'y a rien a "valider").
void touch_controls_draw_video_output_overlay() {
  const TouchZoneLabel left[4] = {
    { "", 0 }, { "SORTIE", 0x07FF }, { "SORTIE", 0x07FF }, { "", 0 }
  };
  const TouchZoneLabel right[4] = {
    { "FERMER", 0xF800 }, { "NORME", 0x7BEF }, { "", 0 }, { "", 0 }
  };
  draw_zone_overlay(left, right);
}

// Journal serie : une ligne a CHAQUE CHANGEMENT d'etat (pas a chaque image -
// ca inonderait la console a 60 Hz) : contact pris, relache, ou changement de
// zone touchee, avec les coordonnees deja etalonnees (x,y) renvoyees par
// ft6336_touch_read() - utile pour verifier que l'etalonnage retombe bien
// dans les bonnes zones (0-31 / 288-319 en x).
// Seuil de glissement (en pixels) pour le defilement tactile de la liste de
// jeux (voir touch_controls_get_gamepad_state()). 18 px : assez pour ignorer
// le tremblement d'un doigt pose, assez peu pour rester reactif sur un ecran
// de 240 px de haut (~13 pas pour traverser tout l'ecran).
#define SWIPE_THRESHOLD_PX 18

// Dernier point lu (menu pause : doigt pose sur l'image, choix d'une ligne au
// doigt).
static bool s_pt_valid = false;
static int s_pt_x = 0, s_pt_y = 0;
bool touch_controls_point(int *x, int *y) {
  if (s_pt_valid) { *x = s_pt_x; *y = s_pt_y; }
  return s_pt_valid;
}

int touch_controls_get_gamepad_state() {
  int x, y;
  static int last_logged_state = -1;  // -1 : jamais log - force le tout premier log
  // Point de reference du glissement en cours (contact au centre de l'ecran) -
  // voir plus bas. -1 : aucun glissement en cours.
  static int swipe_ref_y = -1;

  if (!ft6336_touch_read(&x, &y)) {
    s_pt_valid = false;
    if (last_logged_state != 0) {
      Serial.println("[TOUCH] relache (aucun contact)");
      last_logged_state = 0;
    }
    swipe_ref_y = -1;  // Doigt releve : le prochain contact au
                        // centre repart d'une reference neuve.
    return 0;
  }

  s_pt_valid = true;
  s_pt_x = x;
  s_pt_y = y;
  int state = 0;
  const char *name = "centre (ignore)";

  if (x >= ZONE_LEFT_X0 && x < ZONE_LEFT_X1) {
    int zone = y / ZONE_H;
    if (zone < 0) zone = 0;
    if (zone > 3) zone = 3;
    switch (zone) {
      case 0: state = NES_UP;    name = "UP";    break;
      case 1: state = NES_LEFT;  name = "LEFT";  break;
      case 2: state = NES_RIGHT; name = "RIGHT"; break;
      case 3: state = NES_DOWN;  name = "DOWN";  break;
    }
  } else if (x >= ZONE_RIGHT_X0 && x < ZONE_RIGHT_X1) {
    int zone = y / ZONE_H;
    if (zone < 0) zone = 0;
    if (zone > 3) zone = 3;
    switch (zone) {
      case 0: state = NES_A;      name = "A";      break;
      case 1: state = NES_B;      name = "B";      break;
      case 2: state = NES_START;  name = "START";  break;
      case 3: state = NES_SELECT; name = "SELECT"; break;
    }
  }

  if (state != last_logged_state) {
    Serial.printf("[TOUCH] contact (%d,%d) -> %s (masque 0x%02X)\n", x, y, name, state);
    last_logged_state = state;
  }

  if (state != 0) {
    swipe_ref_y = -1;  // Voir plus bas : un doigt qui quitte la
                        // zone centrale pour une bande laterale ne doit
                        // pas laisser une reference de glissement perimee.
    return state;
  }

  // Au-dela de SWIPE_THRESHOLD_PX, la fonction renvoie HAUT/BAS EN CONTINU
  // (comme un bouton reellement maintenu enfonce), et non une seule fois lors
  // du franchissement. Necessaire a cause du polling de show_menu() :
  // nes_get_gamepad_state() y est appele en boucle mais n'est REELEMENT pris
  // en compte que toutes les 150 ms (anti-rebond, voir "if ((millis() -
  // last_input) > 150)" dans show_menu()) - un signal ponctuel (un seul appel
  // a renvoyer HAUT/BAS juste apres le franchissement) risquerait fortement de
  // tomber entre deux verifications et d'etre perdu. En le renvoyant tant que le doigt
  // reste au-dela du seuil, N'IMPORTE LEQUEL des appels effectues dans la
  // fenetre de 150 ms le capte - et tant que le doigt reste pose la, le
  // defilement continue automatiquement au meme rythme qu'un appui maintenu
  // sur la zone UP/DN laterale (meme anti-rebond, meme comportement, aucun
  // changement cote show_menu()/osd.cpp). Sens du glissement : convention
  // habituelle d'une liste tactile (le contenu "suit" le doigt) - glisser vers
  // le BAS (y croissant par rapport a l'origine) fait remonter des elements
  // plus HAUT dans la liste (NES_UP), glisser vers le HAUT (y decroissant)
  // fait apparaitre les elements SUIVANTS (NES_DOWN) - l'inverse d'un mapping
  // "y augmente -> DOWN" trop naif. swipe_ref_y est remis a -1 (origine
  // perimee, prochain contact au centre en capture une nouvelle) au
  // relachement du doigt (ci-dessus, `if (!ft6336_touch_read(...))`) et au
  // passage sur une bande laterale (juste au-dessus) - PAS a chaque appel ni a
  // chaque pas de defilement.
  if (swipe_ref_y < 0) {
    swipe_ref_y = y;
  } else if (y - swipe_ref_y > SWIPE_THRESHOLD_PX) {
    return NES_UP;
  } else if (swipe_ref_y - y > SWIPE_THRESHOLD_PX) {
    return NES_DOWN;
  }

  // Contact au milieu de l'ecran (sur l'image du jeu ou sur le menu),
  // deplacement encore sous le seuil de glissement : pas une zone de
  // bouton, ignore.
  return 0;
}
