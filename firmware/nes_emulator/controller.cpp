// controller.cpp - lecture des manettes : nes_get_gamepad_state().
//
// Le depot d'origine cablait les 8 boutons NES sur 8 GPIO. Cette carte n'a
// que 6 GPIO libres, partages avec la liaison video (voir hw_config.h) : la
// manette filaire est donc lue en serie sur 3 broches (LATCH, CLOCK, DATA) :
//   - PAD_PROTOCOL_NES (defaut) : manette NES (registre CD4021) de la carte
//     porteuse KiCad, ou manette NES d'origine alimentee en 3,3 V - voir
//     read_nes_pad() ;
//   - PAD_PROTOCOL_HC165 : 8 boutons-poussoirs sur un registre 74HC165.
//
// nes_get_gamepad_state() garde la signature et le masque de bits du depot
// d'origine (NES_A = 0x01, NES_B = 0x02...) : c'est la seule fonction
// appelee par le reste de l'emulateur. Elle combine (OU) la manette
// filaire, les zones tactiles (touch_controls.cpp) et la manette du
// telephone (bluetooth_controller.cpp).
#include <Arduino.h>
#include "hw_config.h"
#include "bluetooth_controller.h"   // Manette du telephone

#if USE_TOUCH_CONTROLS
#include "touch_controls.h"
#endif

#define NES_A 0x01
#define NES_B 0x02
#define NES_SELECT 0x04
#define NES_START 0x08
#define NES_UP 0x10
#define NES_DOWN 0x20
#define NES_LEFT 0x40
#define NES_RIGHT 0x80

void setup_controller() {
  // Les deux blocs ci-dessous ne s'excluent pas : chacun depend de SON PROPRE
  // define (HAS_PHYSICAL_CONTROLLER / USE_TOUCH_CONTROLS), les deux pouvant
  // etre a 1 en meme temps.
#if HAS_PHYSICAL_CONTROLLER && (PAD_PROTOCOL == PAD_PROTOCOL_NES)
  // Manette NES (CD4021) : LATCH et CLOCK au repos a l'etat bas, DATA en
  // entree avec rappel interne (en plus des 47 kOhm de la carte porteuse) pour
  // qu'une prise vide se lise "aucun bouton appuye".
  pinMode(HC165_PL, OUTPUT);
  pinMode(HC165_CLK, OUTPUT);
  pinMode(HC165_Q7, INPUT_PULLUP);
  digitalWrite(HC165_PL, LOW);
  digitalWrite(HC165_CLK, LOW);
#elif HAS_PHYSICAL_CONTROLLER
  pinMode(HC165_PL, OUTPUT);
  pinMode(HC165_CLK, OUTPUT);
  pinMode(HC165_Q7, INPUT);

  // PL au repos a l'etat haut (mode "decalage"), CLK au repos a l'etat
  // bas - conditions de depart classiques pour un 74HC165 dont l'entree
  // CE (clock inhibit) est cablee a la masse (autorisation permanente de
  // decalage, comme dans la quasi-totalite des montages avec un seul
  // 74HC165).
  digitalWrite(HC165_PL, HIGH);
  digitalWrite(HC165_CLK, LOW);
#endif
  // Rien a faire ici pour le tactile : il est initialise/etalonne
  // separement (touch_controls_init_and_calibrate(), appelee depuis
  // setup() APRES l'ecran ET le bus I2C - donc plus tard que ce point).
}

#if HAS_PHYSICAL_CONTROLLER && (PAD_PROTOCOL == PAD_PROTOCOL_NES)
// Lecture d'une manette NES (registre CD4021 : manette_nes du projet KiCad, ou
// manette d'origine alimentee en 3,3 V). Protocole de la console :
//   1. LATCH a 1 pendant 12 us : le 4021 recopie les 8 boutons (chargement
//      parallele asynchrone) ; DATA donne deja le bouton A.
//   2. LATCH a 0, puis pour chaque bouton : lire DATA, puis front montant
//      de CLOCK pour passer au suivant.
// Ordre fixe : A, B, Select, Start, Haut, Bas, Gauche, Droite - le meme que
// les bits NES_A (0x01) ... NES_RIGHT (0x80) : le bit i lu va directement
// dans le bit i du masque. DATA a 0 = bouton appuye.
static int read_nes_pad() {
  digitalWrite(HC165_PL, HIGH);
  delayMicroseconds(NES_PAD_LATCH_US);
  digitalWrite(HC165_PL, LOW);
  delayMicroseconds(NES_PAD_HALF_US);
  int state = 0;
  for (int i = 0; i < 8; i++) {
    if (digitalRead(HC165_Q7) == LOW) state |= (1 << i);
    digitalWrite(HC165_CLK, HIGH);
    delayMicroseconds(NES_PAD_HALF_US);
    digitalWrite(HC165_CLK, LOW);
    delayMicroseconds(NES_PAD_HALF_US);
  }
  return state;
}
#elif HAS_PHYSICAL_CONTROLLER
// Lit les 8 entrees D0..D7 du 74HC165 en une seule fois et renvoie un
// octet dans lequel le bit k correspond directement a l'entree Dk (voir
// le mapping HC165_BIT_* dans hw_config.h). Boutons cables actifs a
// l'etat bas (comme les INPUT_PULLUP + logique "actif bas" du projet
// d'origine) : bouton relache => bit a 1, bouton appuye => bit a 0.
static uint8_t read_hc165() {
  // Impulsion de chargement parallele (PL) : un bref passage a l'etat
  // bas puis un retour a l'etat haut capture l'etat des 8 entrees dans
  // le registre interne du circuit.
  digitalWrite(HC165_PL, LOW);
  delayMicroseconds(5);
  digitalWrite(HC165_PL, HIGH);
  delayMicroseconds(5);

  uint8_t raw = 0;
  for (int i = 0; i < 8; i++) {
    // QH (sortie serie) porte deja le bit courant (D7 en premier, avant
    // toute impulsion d'horloge) - on le lit AVANT de faire avancer le
    // registre, comme le veut le fonctionnement de ce circuit.
    int bit = digitalRead(HC165_Q7);
    raw = (uint8_t)((raw << 1) | (bit ? 1 : 0));

    digitalWrite(HC165_CLK, HIGH);
    delayMicroseconds(2);
    digitalWrite(HC165_CLK, LOW);
    delayMicroseconds(2);
  }
  // Apres la boucle : raw contient D7 D6 D5 D4 D3 D2 D1 D0, du bit 7 au
  // bit 0 - donc raw bit k == entree Dk. Correspond directement aux
  // constantes HC165_BIT_* de hw_config.h.
  return raw;
}
#endif  // HAS_PHYSICAL_CONTROLLER

// Lecture de la SEULE manette physique (74HC165), sans le tactile - utilisee
// par ui_lvgl.cpp comme peripherique "clavier" de LVGL.
// nes_get_gamepad_state() ci-dessous ne convient pas pour les menus LVGL :
// elle ajoute les zones tactiles des bandes laterales, alors que LVGL lit deja
// le doigt lui-meme (partout sur l'ecran). Renvoie le meme masque NES_* que
// nes_get_gamepad_state() ; toujours 0 tant que HAS_PHYSICAL_CONTROLLER vaut
// 0.
extern "C" int controller_read_physical_buttons() {
  int state = 0;
#if HAS_PHYSICAL_CONTROLLER && (PAD_PROTOCOL == PAD_PROTOCOL_NES)
  state = read_nes_pad();
#elif HAS_PHYSICAL_CONTROLLER
  uint8_t raw = read_hc165();
  if (!(raw & (1 << HC165_BIT_A)))      state |= NES_A;
  if (!(raw & (1 << HC165_BIT_B)))      state |= NES_B;
  if (!(raw & (1 << HC165_BIT_SELECT))) state |= NES_SELECT;
  if (!(raw & (1 << HC165_BIT_START)))  state |= NES_START;
  if (!(raw & (1 << HC165_BIT_UP)))     state |= NES_UP;
  if (!(raw & (1 << HC165_BIT_DOWN)))   state |= NES_DOWN;
  if (!(raw & (1 << HC165_BIT_LEFT)))   state |= NES_LEFT;
  if (!(raw & (1 << HC165_BIT_RIGHT)))  state |= NES_RIGHT;
#endif
  return state;
}

// Etat de la manette NES : OU bit a bit de la manette filaire, des zones
// tactiles et de la manette du telephone (voir hw_config.h :
// HAS_PHYSICAL_CONTROLLER / USE_TOUCH_CONTROLS / g_display_tv_mode).
extern "C" int IRAM_ATTR nes_get_gamepad_state() {
  int state = 0;

#if HAS_PHYSICAL_CONTROLLER && (PAD_PROTOCOL == PAD_PROTOCOL_NES)
  state = read_nes_pad();
#elif HAS_PHYSICAL_CONTROLLER
  uint8_t raw = read_hc165();

  if (!(raw & (1 << HC165_BIT_A)))      state |= NES_A;
  if (!(raw & (1 << HC165_BIT_B)))      state |= NES_B;
  if (!(raw & (1 << HC165_BIT_SELECT))) state |= NES_SELECT;
  if (!(raw & (1 << HC165_BIT_START)))  state |= NES_START;
  if (!(raw & (1 << HC165_BIT_UP)))     state |= NES_UP;
  if (!(raw & (1 << HC165_BIT_DOWN)))   state |= NES_DOWN;
  if (!(raw & (1 << HC165_BIT_LEFT)))   state |= NES_LEFT;
  if (!(raw & (1 << HC165_BIT_RIGHT)))  state |= NES_RIGHT;
#endif

  // La manette virtuelle du telephone envoie EXACTEMENT le meme masque NES_*
  // que ci-dessus ; on l'ajoute par OU aux autres sources (manette physique,
  // puis tactile plus bas). Le delai de securite de bluetooth_controller.cpp
  // (500 ms sans trame) relache tous les boutons si le telephone se tait. La
  // restriction du mode Televiseur (plus bas) ne touche que le tactile : le
  // telephone reste une manette complete pendant une partie sur la TV.
  state |= bluetooth_controller_get_state();

#if USE_TOUCH_CONTROLS
  // En mode Television, pendant une PARTIE (g_in_menu_screen == false), l'ecran
  // LCD est eteint : le tactile est ignore, sauf la zone SELECT (coin bas-droit),
  // gardee comme sortie de secours (maintien 3 s = retour au menu) pour ne
  // jamais rester bloque sans manette physique. Les ecrans de menu restent
  // toujours utilisables au doigt.
  //
  // Meme restriction quand la partie est affichee en plein ecran sur le LCD
  // (g_lcd_fullscreen_active, voir LCD_FULLSCREEN_MODE dans hw_config.h) : les
  // bandes tactiles ont disparu sous l'image du jeu, on joue avec la manette
  // du telephone ; seule la zone SELECT (coin bas-droit) reste lue, pour
  // pouvoir quitter (maintien 3 s).
  bool touch_restricted_to_quit_only = (g_display_tv_mode || g_lcd_fullscreen_active) && !g_in_menu_screen;
  int touch_state = touch_controls_get_gamepad_state();

  {
    static unsigned long last_diag = 0;
    if (millis() - last_diag > 1000) {
      Serial.printf("[DIAG-TOUCH] tv_mode=%d menu=%d restrict_select=%d touch_raw=0x%02X\n",
                    (int)g_display_tv_mode, (int)g_in_menu_screen,
                    (int)touch_restricted_to_quit_only, (unsigned)touch_state);
      last_diag = millis();
    }
  }

  if (touch_restricted_to_quit_only) {
    touch_state &= NES_SELECT;
  }
  state |= touch_state;
#endif

  return state;
}
