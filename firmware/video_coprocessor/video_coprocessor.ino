// video_coprocessor.ino - carte video TV de la console NES.
//
// Second ESP32 dedie a la television (par exemple une carte Freenove
// ESP32-WROOM, module ESP32-WROOM-32E). Il recoit l'image et le son du jeu depuis la carte
// principale (ESP32-S3) par une liaison SPI a sens unique (3 fils + masse)
// et fabrique :
//   - un signal video COMPOSITE EN COULEUR (PAL 50 Hz, NTSC 60 Hz ou PAL
//     60 Hz) sur GPIO25, grace au DAC integre de l'ESP32 "classique" ;
//   - le son du jeu sur GPIO26 (modulation PWM + petit filtre RC, methode
//     du projet esp_8_bit), avec correction de derive entre les deux quartz.
// Un convertisseur "AV vers HDMI" (ou l'entree RCA / Peritel du televiseur)
// termine la chaine. Voir docs/MATERIEL.md (carte video, carte porteuse
// KiCad, manette).
//
// CARTE A CHOISIR DANS L'IDE ARDUINO : "ESP32 Dev Module" (coeur esp32 par
// Espressif, version 3.x). PAS "ESP32S3 Dev Module" : ce croquis ne tourne
// que sur un ESP32 classique (ESP32-WROOM-32 / 32E, puce ESP32-D0WD), le seul
// a avoir a la fois le DAC et la generation video necessaires.
//
// BIBLIOTHEQUE A INSTALLER : "LovyanGFX" (Gestionnaire de bibliotheques,
// auteur lovyan03, version 1.2.x). Rien d'autre : les polices avec accents
// sont fournies dans tv_fonts.h, la sortie video dans nes_cvbs.*.
//
// FICHIERS DE CE DOSSIER :
//   video_coprocessor.ino  - ce fichier (materiel : video, son, SPI, bouton)
//   nes_cvbs.hpp / .cpp    - sortie composite (copie adaptee de LovyanGFX
//                            Panel_CVBS : palette exacte, image progressive,
//                            PAL 60)
//   tv_link_protocol.h     - format des paquets (COPIE IDENTIQUE cote console)
//   tv_link_codec.h        - compression des bandes (COPIE IDENTIQUE cote console)
//   tv_link_receiver.h     - decodage des paquets + file audio (sans materiel)
//   tv_fonts.h             - polices Helvetica avec accents (U8g2, licence X11)
//   LICENCE_LovyanGFX.txt  - licence FreeBSD de LovyanGFX (nes_cvbs.*)
//
// FONCTIONNEMENT :
//   - chaque bande de 15 lignes arrive avec son numero et une somme de
//     controle, compressee ou non ; une bande abimee est jetee (la console
//     la renvoie) ; jusqu'a 16 tampons de reception si la memoire le permet ;
//   - l'image complete est affichee d'un coup pendant le retour de trame (pas
//     de dechirure), sans bloquer la reception ;
//   - image progressive au choix (Reglages > Video de la console) : 288
//     lignes en PAL, 240 en NTSC, sans entrelacement, comme une vraie NES ;
//   - PAL 60 : couleur PAL a 60 images/s, les jeux NES sans image sautee. Si
//     la television ou le convertisseur ne l'accepte pas, revenir en PAL 50 ;
//   - ecrans d'attente avec mire de couleurs, statistiques de la liaison
//     (taux de compression et d'erreurs, mode d'image), norme memorisee ;
//   - appui long sur BOOT (console debranchee) : PAL 50 -> NTSC -> PAL 60 ;
//   - statistiques de liaison sur le port serie toutes les 5 s.

#include <Arduino.h>
#include <Preferences.h>
#include <LovyanGFX.hpp>
#include "driver/spi_slave.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nes_cvbs.hpp"
#include "tv_link_protocol.h"
#include "tv_link_receiver.h"
#include "tv_fonts.h"

#if !defined(CONFIG_IDF_TARGET_ESP32)
#error "Carte video TV : choisir 'ESP32 Dev Module' (ESP32 classique, WROOM-32/32E). Les ESP32-S2/S3/C3/C6 n'ont pas le DAC video necessaire."
#endif
#if !defined(ESP_ARDUINO_VERSION_MAJOR) || (ESP_ARDUINO_VERSION_MAJOR < 3)
#error "Carte video TV : coeur Arduino-ESP32 3.x requis (Outils > Type de carte > Gestionnaire de cartes > esp32 par Espressif)."
#endif

#define COPRO_VERSION "v3.1 (2026-10-10)"

// ============================ Brochage ============================
// Liaison avec la carte principale : broches natives (IO_MUX) du bus SPI2,
// indispensables pour la vitesse. GPIO12 (MISO) volontairement NON utilise :
// c'est une broche de demarrage (tension de la memoire flash).
#define PIN_LINK_MOSI 13   // <- GPIO43 (TXD0) de la carte principale
#define PIN_LINK_SCLK 14   // <- GPIO2  (IO2)  de la carte principale
#define PIN_LINK_CS   15   // <- GPIO3  (IO3)  de la carte principale
#define PIN_VIDEO     25   // DAC 1 -> fiche RCA jaune (en direct, sans composant)
#define PIN_AUDIO     26   // PWM -> 1 kOhm + 10 nF -> 10 uF -> RCA blanche/rouge (carte porteuse)
#define PIN_BOOT       0   // bouton BOOT de la carte
#define PIN_LED        2   // LED bleue de beaucoup de cartes DevKit (-1 : aucune)

// ============================ Reglages ============================
#define VIDEO_OUTPUT_LEVEL 128   // amplitude : 128 par defaut ; 160-200 si l'image est sombre ou decroche
#define VIDEO_CHROMA_LEVEL 128   // saturation des couleurs (0 = noir et blanc)
#define DEFAULT_NORM       TVLINK_NORM_PAL   // norme au tout premier demarrage (ensuite : memorisee)
#define RX_BUFFERS_MIN     10    // paquets SPI pouvant attendre
#define RX_BUFFERS_MAX     16    // Davantage si la memoire le permet
#define RX_HEAP_RESERVE    32768 // Memoire laissee libre apres les tampons
#define LINK_IDLE_MS       3000  // sans nouvelles de la console depuis 3 s -> ecran d'attente
#define STARTUP_BEEP       1     // deux bips au demarrage (verifie le cablage du son)

// Geometrie de l'image selon la norme. NETTETE : chaque pixel du jeu dure un
// nombre ENTIER d'echantillons du signal (3), sinon les colonnes n'ont pas
// toutes la meme largeur et l'image parait floue.
//  - PAL 50 Hz : 288 colonnes de 3 echantillons (864), les 256 du jeu au
//    centre ;
//  - PAL 60 Hz : meme chose (meme horloge que PAL), 240 lignes, 60 Hz ;
//  - NTSC : 720 echantillons utiles = 240 colonnes de 3. Les 8 colonnes de
//    chaque bord du jeu ne sont pas montrees (les televisions les cachent de
//    toute facon ; beaucoup de jeux NES y laissent des parasites).
using CvbsPanel = lgfx::nestv::Panel_NesCVBS;   // nes_cvbs.hpp
using SignalType = CvbsPanel::config_detail_t::signal_type_t;

struct NormGeom {
  SignalType type;
  int memory_width, memory_height;   // taille "memoire" du signal
  int panel_width;                   // colonnes du jeu montrees
  int offset_x, offset_y;            // place de l'image dans la memoire
  int crop_x;                        // premiere colonne du jeu montree
  const char *name;
};

static const NormGeom NORMS[3] = {
  {SignalType::NTSC,   240, 240, 240,  0,  0, 8, "NTSC 60 Hz"},
  {SignalType::PAL,    288, 288, 256, 16, 24, 0, "PAL 50 Hz"},
  {SignalType::PAL_60, 288, 240, 256, 16,  0, 0, "PAL 60 Hz"},
};
// (macro et non fonction : l'IDE Arduino insere ses declarations de
// fonctions avant la premiere fonction du fichier, ici trop tot.)
#define norm_geom(n) (NORMS[(uint8_t)(n) <= TVLINK_NORM_MAX ? (uint8_t)(n) : TVLINK_NORM_PAL])

// ======================= Sortie video (LovyanGFX) =======================

class TvPanel : public CvbsPanel {
 public:
  // Acces direct aux lignes de l'image affichee (1 octet RGB332 par pixel).
  uint8_t *line(int y) {
    return (_lines_buffer && y >= 0 && y < TVLINK_HEIGHT) ? _lines_buffer[y] : nullptr;
  }
};

class TvOut : public lgfx::LGFX_Device {
 public:
  TvPanel panel;
  TvOut() {
    auto cfg = panel.config();
    cfg.memory_width = 288;   // ajuste par setResolution() au demarrage (norme)
    cfg.memory_height = TVLINK_HEIGHT;
    cfg.panel_width = TVLINK_WIDTH;
    cfg.panel_height = TVLINK_HEIGHT;
    cfg.offset_x = 16;
    cfg.offset_y = 0;
    panel.config(cfg);
    auto det = panel.config_detail();
    det.signal_type = SignalType::PAL;
    det.pin_dac = PIN_VIDEO;
    det.use_psram = 0;
    det.output_level = VIDEO_OUTPUT_LEVEL;
    det.chroma_level = VIDEO_CHROMA_LEVEL;
    panel.config_detail(det);
    setPanel(&panel);
  }
};

static TvOut tv;
static const lgfx::U8g2font FONT_TEXT(tvfont_helvR10_tf);
static const lgfx::U8g2font FONT_BOLD(tvfont_helvB12_tf);
static const lgfx::U8g2font FONT_TITLE(tvfont_helvB18_tf);

// ============================ Etat partage ============================
static SemaphoreHandle_t s_panel_mutex = nullptr;   // image affichee (2 coeurs)
static uint8_t *s_bands[TVLINK_NUM_BANDS + 1];      // image en reconstruction (+1 : decompression)
static TvLinkReceiver s_rx;
static TvAudioPlayer s_player;
static Preferences s_prefs;

enum Screen { SCREEN_NONE, SCREEN_WAITING, SCREEN_MENU, SCREEN_MISMATCH, SCREEN_GAME };
static volatile Screen s_screen = SCREEN_NONE;

static volatile uint8_t s_current_norm = DEFAULT_NORM;     // TVLINK_NORM_*
static volatile uint8_t s_requested_norm = DEFAULT_NORM;
static volatile int s_crop_x = 0;                           // Voir NormGeom
static volatile int s_panel_w = TVLINK_WIDTH;
static volatile bool s_standard_change = false;
static volatile uint32_t s_last_packet_ms = 0;
static volatile uint32_t s_last_frame_ms = 0;
static volatile uint32_t s_last_config_ms = 0;
static volatile bool s_ever_packet = false;
static volatile bool s_ever_frame = false;
static volatile bool s_cfg_in_game = false;
static volatile uint8_t s_cfg_version = 0;
static volatile uint32_t s_frames_shown = 0;
static volatile uint32_t s_present_skipped = 0;
static volatile bool s_link_ok = false;
static volatile int s_rx_buffers = 0;
static bool s_audio_ok = false;

// Palette utilisee par la sortie video (protegee par s_panel_mutex) :
//   PALETTE_332     : couleurs RGB332 (ecrans d'attente dessines ici) ;
//   PALETTE_PENDING : palette de la console calculee, appliquee avec la
//                     prochaine image (pendant le retour de trame) ;
//   PALETTE_CONSOLE : palette de la console affichee.
enum PaletteState { PALETTE_332, PALETTE_PENDING, PALETTE_CONSOLE };
static PaletteState s_palette_state = PALETTE_332;
static volatile bool s_progressive = false;
static volatile uint32_t s_presents_forced = 0;   // images affichees hors retour de trame

// ============================ Son ============================
// PWM 8 bits a 312,5 kHz (80 MHz / 256) sur GPIO26, mis a jour 16 000 fois
// par seconde par un temporisateur materiel. Le filtre RC externe (1 kOhm +
// 10 nF) ne garde que le son.
static hw_timer_t *s_audio_timer = nullptr;

static void ARDUINO_ISR_ATTR audio_isr() {
  int32_t s = s_player.next();
  ledcWrite(PIN_AUDIO, (uint32_t)(s + 32768) >> 8);
}

static bool audio_begin() {
  if (!ledcAttach(PIN_AUDIO, 312500, 8) && !ledcAttach(PIN_AUDIO, 156250, 8)) {
    Serial.println("[SON] ledcAttach a echoue : pas de son sur la TV.");
    return false;
  }
  ledcWrite(PIN_AUDIO, 128);                 // silence = rapport cyclique 50 %
  s_player.reset();
  s_audio_timer = timerBegin(16000000);      // 16 MHz / 1000 = 16 kHz exactement
  if (!s_audio_timer) {
    Serial.println("[SON] timerBegin a echoue : pas de son sur la TV.");
    return false;
  }
  timerAttachInterrupt(s_audio_timer, &audio_isr);
  timerAlarm(s_audio_timer, 16000000 / TVLINK_AUDIO_RATE, true, 0);
  return true;
}

static void audio_beep() {
#if STARTUP_BEEP
  static int16_t tone[TVLINK_AUDIO_SAMPLES];
  const int freqs[2] = {880, 1320};
  for (int f = 0; f < 2; f++) {
    for (int chunk = 0; chunk < 6; chunk++) {        // 6 x 256 = ~100 ms par note
      for (int i = 0; i < TVLINK_AUDIO_SAMPLES; i++) {
        int n = chunk * TVLINK_AUDIO_SAMPLES + i;
        tone[i] = ((n * freqs[f] * 2 / TVLINK_AUDIO_RATE) & 1) ? 5000 : -5000;
      }
      s_player.ring.push(tone, TVLINK_AUDIO_SAMPLES);
    }
  }
#endif
}

// ============================ Affichage ============================
// L'image recue est recopiee dans l'image affichee pendant que le faisceau est
// hors de l'image (retour de trame et bordures noires) : pas de dechirure. La
// copie (60 Ko, moins d'une demi-milliseconde) tient largement dans cette
// fenetre (PAL : environ 70 lignes de 64 us).
static void copy_bands_to_screen() {
  for (int y = 0; y < TVLINK_HEIGHT; y++) {
    uint8_t *dst = tv.panel.line(y);
    if (!dst) return;
    memcpy(dst, s_rx.band(y / TVLINK_BAND_ROWS) + (y % TVLINK_BAND_ROWS) * TVLINK_WIDTH + s_crop_x, s_panel_w);
  }
}

// Palette de la console a afficher avec l'image (appele sous s_panel_mutex).
static void apply_console_palette_now() {
  if (s_palette_state == PALETTE_PENDING) {
    tv.panel.applyPendingPalette();
    s_palette_state = PALETTE_CONSOLE;
  } else if (s_palette_state == PALETTE_332 && s_rx.has_palette()) {
    tv.panel.setPaletteRGB888(s_rx.palette(), true);   // retour d'un ecran d'attente : ~1 ms
    s_palette_state = PALETTE_CONSOLE;
  }
}

// Appelee par la tache de reception (coeur 0) quand une image est complete
// et que le faisceau est hors de l'image (ou qu'on ne peut plus attendre).
// Faux si l'image affichee est occupee (ecran d'attente en cours de dessin).
static bool present_frame(bool forced) {
  if (xSemaphoreTake(s_panel_mutex, 0) != pdTRUE) return false;
  copy_bands_to_screen();
  apply_console_palette_now();
  s_screen = SCREEN_GAME;
  xSemaphoreGive(s_panel_mutex);
  s_last_frame_ms = millis();
  s_ever_frame = true;
  s_frames_shown = s_frames_shown + 1;
  if (forced) s_presents_forced = s_presents_forced + 1;
  return true;
}

// Nouvelle palette recue : calculee tout de suite dans la table de reserve,
// affichee avec la prochaine image.
static void prepare_console_palette() {
  xSemaphoreTake(s_panel_mutex, portMAX_DELAY);
  tv.panel.setPaletteRGB888(s_rx.palette(), false);
  s_palette_state = PALETTE_PENDING;
  xSemaphoreGive(s_panel_mutex);
}

static void set_norm_resolution(uint8_t norm) {
  const NormGeom &g = norm_geom(norm);
  tv.panel.setResolution(g.panel_width, TVLINK_HEIGHT, g.type, g.memory_width, g.memory_height, g.offset_x, g.offset_y);
  s_crop_x = g.crop_x;
  s_panel_w = g.panel_width;
}

static void apply_standard(uint8_t norm) {
  xSemaphoreTake(s_panel_mutex, portMAX_DELAY);
  set_norm_resolution(norm);
  s_current_norm = norm;
  // L'image vient d'etre reallouee (noire) : on remet la derniere image du jeu
  // si une partie est en cours, sinon l'ecran d'attente sera redessine.
  // La palette choisie (console ou RGB332) est recalculee par la
  // reinitialisation, une palette en attente est donc deja appliquee.
  if (s_palette_state == PALETTE_PENDING) s_palette_state = PALETTE_CONSOLE;
  if (s_screen == SCREEN_GAME && s_ever_frame) {
    copy_bands_to_screen();
  } else {
    s_screen = SCREEN_NONE;
  }
  xSemaphoreGive(s_panel_mutex);
  if (s_prefs.getUChar("norm", 0xFF) != norm) s_prefs.putUChar("norm", norm);
  Serial.printf("[TV] norme : %s\n", norm_geom(norm).name);
}

// Mire : 8 barres de couleur a 75 % (blanc, jaune, cyan, vert, magenta,
// rouge, bleu, noir) - sert a verifier couleurs, cadrage et norme.
static void draw_color_bars(int y, int h) {
  static const uint32_t bars[8] = {0xC0C0C0, 0xC0C000, 0x00C0C0, 0x00C000,
                                   0xC000C0, 0xC00000, 0x0000C0, 0x000000};
  int x0 = 12, w = (tv.width() - 2 * x0) / 8;   // 240 colonnes en NTSC
  for (int i = 0; i < 8; i++) tv.fillRect(x0 + i * w, y, w, h, bars[i]);
  tv.drawRect(x0 - 1, y - 1, 8 * w + 2, h + 2, (uint32_t)0x606060);
}

static void draw_status_screen(Screen which) {
  const uint32_t BG = 0x101830, INK = 0xF0F0F0, MUTED = 0xA0A8C0;
  uint32_t accent = which == SCREEN_MENU ? (uint32_t)0x40E080 : which == SCREEN_MISMATCH ? (uint32_t)0xF06040 : (uint32_t)0xF0C040;
  const TvLinkStats &st = s_rx.stats();
  char line[64];

  xSemaphoreTake(s_panel_mutex, portMAX_DELAY);
  if (s_palette_state != PALETTE_332) {   // Dessins LovyanGFX en RGB332
    tv.panel.setPaletteRGB888(nullptr, true);
    s_palette_state = PALETTE_332;
  }
  tv.startWrite();
  tv.fillScreen(BG);
  tv.setTextDatum(lgfx::top_left);
  tv.setTextColor(INK);
  tv.setFont(&FONT_TITLE);
  tv.drawString("Console NES", 14, 14);
  tv.setFont(&FONT_TEXT);
  tv.setTextColor(MUTED);
  tv.drawString("Sortie TV \xC2\xB7 " COPRO_VERSION, 14, 42);

  tv.fillRect(14, 64, 4, 44, accent);
  tv.setFont(&FONT_BOLD);
  tv.setTextColor(accent);
  const char *title = which == SCREEN_MENU ? "Console connect\xC3\xA9" "e"
                    : which == SCREEN_MISMATCH ? "Versions diff\xC3\xA9rentes"
                    : "En attente de la console";
  tv.drawString(title, 26, 64);
  tv.setFont(&FONT_TEXT);
  tv.setTextColor(INK);
  if (which == SCREEN_MENU) {
    tv.drawString("Lance un jeu sur l'\xC3\xA9" "cran de la console.", 26, 86);
  } else if (which == SCREEN_MISMATCH) {
    snprintf(line, sizeof(line), "Console : protocole v%u, TV : v%u.", (unsigned)s_cfg_version,
             (unsigned)TVLINK_PROTOCOL_VERSION);
    tv.drawString(line, 26, 86);
    tv.drawString("Reflashe les deux cartes.", 26, 100);
  } else {
    tv.drawString("C\xC3\xA2" "ble SCLK, CS, MOSI et masse ?", 26, 86);
    tv.drawString("R\xC3\xA9glages > Sortie : T\xC3\xA9l\xC3\xA9viseur", 26, 100);
  }

  tv.setTextColor(MUTED);
  snprintf(line, sizeof(line), "Norme : %s \xC2\xB7 image %s", norm_geom(s_current_norm).name,
           s_progressive ? "progressive" : "entrelac\xC3\xA9" "e");
  tv.drawString(line, 14, 128);
  // Erreurs en pourcentage des paquets (plus parlant qu'un compteur qui
  // grandit sans cesse).
  const uint32_t errs = st.bad_checksum + st.bad_header + st.bad_length;
  const uint32_t all = st.packets + errs;
  snprintf(line, sizeof(line), "Paquets : %lu \xC2\xB7 erreurs : %lu (%.1f %%)", (unsigned long)st.packets,
           (unsigned long)errs, all ? 100.0f * errs / all : 0.0f);
  tv.drawString(line, 14, 144);
  tv.drawString(s_link_ok ? (s_audio_ok ? "Liaison SPI et son pr\xC3\xAAts" : "Liaison SPI pr\xC3\xAA" "te, son absent")
                          : "Liaison SPI en panne (port s\xC3\xA9rie)", 14, 160);

  draw_color_bars(186, 34);
  tv.endWrite();
  s_screen = which;
  xSemaphoreGive(s_panel_mutex);
}

// ======================= Reception (coeur 0) =======================
static void handle_config() {
  s_last_config_ms = millis();
  s_cfg_version = s_rx.config_version();
  s_cfg_in_game = s_rx.config_in_game();
  if (s_cfg_version != TVLINK_PROTOCOL_VERSION) return;
  if (s_rx.config_norm() != s_current_norm && !s_standard_change) {
    s_requested_norm = s_rx.config_norm();
    s_standard_change = true;
  }
  // Image progressive ou entrelacee, au choix de la console (pas memorise
  // ici : au demarrage, entrelacee, qui marche partout).
  if (s_rx.config_progressive() != s_progressive) {
    s_progressive = s_rx.config_progressive();
    tv.panel.setProgressive(s_progressive);
    Serial.printf("[TV] image %s\n", s_progressive ? "progressive (288p / 240p)" : "entrelacee");
  }
}

// Paquets d'image (bandes, palette) mis de cote pendant qu'une image complete
// attend le retour de trame pour etre affichee : ils seront traites juste
// apres, dans l'ordre (les traiter avant melangerait deux images). Le son et
// la configuration passent tout de suite.
static spi_slave_transaction_t *s_held[RX_BUFFERS_MAX];
static int s_held_n = 0;
static bool s_present_pending = false;
static int64_t s_present_since_us = 0;

static void requeue(spi_slave_transaction_t *t) {
  t->length = TVLINK_MAX_PACKET_BYTES * 8;
  t->trans_len = 0;
  spi_slave_queue_trans(SPI2_HOST, t, portMAX_DELAY);
}

// Traite un tampon recu ; une image complete devient "a afficher".
static void process_packet(spi_slave_transaction_t *t) {
  const size_t len = t->trans_len / 8;
  const int n = s_rx.handle_all((const uint8_t *)t->rx_buffer, len, [](int ev) {
    if (ev & TVLINK_EV_AUDIO) s_player.ring.push(s_rx.audio_samples(), (uint32_t)s_rx.audio_count());
    if (ev & TVLINK_EV_CONFIG) handle_config();
    if (ev & TVLINK_EV_PALETTE) prepare_console_palette();
    if (ev & TVLINK_EV_FRAME) {
      s_present_pending = true;
      s_present_since_us = esp_timer_get_time();
    }
  });
  if (n > 0) {
    s_last_packet_ms = millis();
    s_ever_packet = true;
  }
}

static void link_task(void *) {
  static uint8_t *rx_buf[RX_BUFFERS_MAX];
  static spi_slave_transaction_t trans[RX_BUFFERS_MAX];

  // Le son est demarre ici pour que son interruption tourne sur le coeur 0
  // (la video occupe le coeur 1).
  s_audio_ok = audio_begin();
  if (s_audio_ok) audio_beep();

  // 10 tampons au moins, 16 si la memoire le permet (une image compressee
  // entiere peut alors attendre le retour de trame).
  int n = 0;
  for (; n < RX_BUFFERS_MAX; n++) {
    if (n >= RX_BUFFERS_MIN && heap_caps_get_free_size(MALLOC_CAP_DMA) < RX_HEAP_RESERVE + TVLINK_MAX_PACKET_BYTES) break;
    rx_buf[n] = (uint8_t *)heap_caps_malloc(TVLINK_MAX_PACKET_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!rx_buf[n]) break;
  }
  s_rx_buffers = n;
  if (n < 3) {
    Serial.printf("[SPI] memoire insuffisante (%d tampons) : liaison arretee.\n", n);
    vTaskDelete(nullptr);
    return;
  }
  const int hold_max = n - 3;   // garde 3 tampons libres pour le son et la configuration

  spi_bus_config_t bus = {};
  bus.mosi_io_num = PIN_LINK_MOSI;
  bus.miso_io_num = -1;
  bus.sclk_io_num = PIN_LINK_SCLK;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = TVLINK_MAX_PACKET_BYTES;

  spi_slave_interface_config_t slv = {};
  slv.mode = 0;
  slv.spics_io_num = PIN_LINK_CS;
  slv.queue_size = n;
  slv.flags = 0;

  esp_err_t err = spi_slave_initialize(SPI2_HOST, &bus, &slv, SPI_DMA_CH_AUTO);
  if (err != ESP_OK) {
    Serial.printf("[SPI] spi_slave_initialize a echoue (%d) : liaison arretee.\n", (int)err);
    vTaskDelete(nullptr);
    return;
  }
  // Console eteinte ou en demarrage : CS reste au repos (haut), horloge basse.
  gpio_set_pull_mode((gpio_num_t)PIN_LINK_CS, GPIO_PULLUP_ONLY);
  gpio_set_pull_mode((gpio_num_t)PIN_LINK_SCLK, GPIO_PULLDOWN_ONLY);
  gpio_set_pull_mode((gpio_num_t)PIN_LINK_MOSI, GPIO_PULLDOWN_ONLY);

  for (int i = 0; i < n; i++) {
    trans[i] = {};
    trans[i].length = TVLINK_MAX_PACKET_BYTES * 8;
    trans[i].rx_buffer = rx_buf[i];
    spi_slave_queue_trans(SPI2_HOST, &trans[i], portMAX_DELAY);
  }
  s_link_ok = true;
  Serial.printf("[SPI] pret : MOSI=GPIO%d SCLK=GPIO%d CS=GPIO%d, %d tampons de %d octets, tas libre %u o.\n",
                PIN_LINK_MOSI, PIN_LINK_SCLK, PIN_LINK_CS, n, TVLINK_MAX_PACKET_BYTES,
                (unsigned)esp_get_free_heap_size());

  for (;;) {
    spi_slave_transaction_t *done = nullptr;
    // Image en attente du retour de trame : on revient voir le faisceau
    // toutes les millisecondes environ.
    const TickType_t wait = s_present_pending ? 1 : pdMS_TO_TICKS(200);
    if (spi_slave_get_trans_result(SPI2_HOST, &done, wait) == ESP_OK && done) {
      const char type = TvLinkReceiver::packet_type((const uint8_t *)done->rx_buffer, done->trans_len / 8);
      if (s_present_pending && (type == TVLINK_TYPE_BAND || type == TVLINK_TYPE_PALETTE)) {
        s_held[s_held_n++] = done;   // traite apres l'affichage de l'image en attente
      } else {
        process_packet(done);
        requeue(done);
      }
    }
    if (!s_present_pending) continue;

    // Affichage : faisceau hors de l'image (3 lignes de marge) ; sinon,
    // tout de suite si les tampons de reception s'epuisent ou apres 45 ms
    // (rien ne doit bloquer la reception).
    const bool blank = tv.panel.inBlankingWindow(3);
    const bool must = s_held_n >= hold_max || (esp_timer_get_time() - s_present_since_us) > 45000;
    if (!blank && !must) continue;
    if (!present_frame(!blank)) {
      if (!must) continue;   // ecran d'attente en cours de dessin : on reessaie
      // Image affichee occupee trop longtemps : celle-ci est abandonnee.
      s_present_skipped = s_present_skipped + 1;
    }
    s_present_pending = false;
    // Paquets mis de cote, dans l'ordre ; s'arrete a la prochaine image
    // complete (elle attendra a son tour le retour de trame).
    int k = 0;
    while (k < s_held_n && !s_present_pending) {
      process_packet(s_held[k]);
      requeue(s_held[k]);
      k++;
    }
    for (int j = k; j < s_held_n; j++) s_held[j - k] = s_held[j];
    s_held_n -= k;
  }
}

// ======================= Bouton BOOT, LED, statistiques =======================
static void poll_boot_button(uint32_t now) {
  static uint32_t pressed_since = 0;
  static bool handled = false;
  bool down = digitalRead(PIN_BOOT) == LOW;
  if (!down) {
    pressed_since = 0;
    handled = false;
    return;
  }
  if (!pressed_since) pressed_since = now;
  if (!handled && now - pressed_since > 1500) {
    handled = true;
    bool console_alive = s_ever_packet && (now - s_last_config_ms) < LINK_IDLE_MS;
    if (console_alive) {
      Serial.println("[TV] norme imposee par la console : la changer dans Reglages > Sortie video.");
    } else {
      // PAL 50 -> NTSC -> PAL 60 -> PAL 50...
      s_requested_norm = s_current_norm == TVLINK_NORM_PAL ? TVLINK_NORM_NTSC
                       : s_current_norm == TVLINK_NORM_NTSC ? TVLINK_NORM_PAL60 : TVLINK_NORM_PAL;
      s_standard_change = true;
    }
  }
}

static void print_stats(uint32_t now) {
  static uint32_t last = 0, last_frames = 0, last_bands = 0;
  if (now - last < 5000) return;
  const TvLinkStats &st = s_rx.stats();
  float secs = (now - last) / 1000.0f;
  static uint32_t last_bytes = 0, last_errs = 0, last_packets = 0, last_forced = 0;
  const uint32_t errs = st.bad_checksum + st.bad_header + st.bad_length;
  const uint32_t d_bands = st.bands - last_bands, d_bytes = st.band_bytes - last_bytes;
  const uint32_t d_errs = errs - last_errs, d_pk = st.packets - last_packets;
  Serial.printf("[TV] %.1f im/s affichees (%lu hors retour de trame), %.0f bandes/s, taille %.0f %% | "
                "erreurs %.1f %% (somme=%lu en-tete=%lu taille=%lu) | fins perdues=%lu | colles=%lu reste=%lu | "
                "son : file=%lu ech, coupures=%lu, corrections=%lu | %s %s | %d tampons | tas libre %u o\n",
                (s_frames_shown - last_frames) / secs, (unsigned long)(s_presents_forced - last_forced),
                d_bands / secs, d_bands ? 100.0f * d_bytes / ((float)d_bands * TVLINK_BAND_PACKET_BYTES) : 0.0f,
                (d_pk + d_errs) ? 100.0f * d_errs / (d_pk + d_errs) : 0.0f,
                (unsigned long)st.bad_checksum, (unsigned long)st.bad_header, (unsigned long)st.bad_length,
                (unsigned long)st.missed_frame_ends, (unsigned long)st.merged, (unsigned long)st.trailing_garbage,
                (unsigned long)s_player.ring.count(),
                (unsigned long)s_player.underruns(), (unsigned long)s_player.corrections(),
                norm_geom(s_current_norm).name, s_progressive ? "progressif" : "entrelace", (int)s_rx_buffers,
                (unsigned)esp_get_free_heap_size());
  last = now;
  last_frames = s_frames_shown;
  last_bands = st.bands;
  last_bytes = st.band_bytes;
  last_errs = errs;
  last_packets = st.packets;
  last_forced = s_presents_forced;
}

// ============================ setup / loop ============================
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n=== Carte video TV (console NES) " COPRO_VERSION " ===");
  pinMode(PIN_BOOT, INPUT_PULLUP);
  if (PIN_LED >= 0) pinMode(PIN_LED, OUTPUT);

  s_prefs.begin("nestv", false);
  // Norme memorisee.
  uint8_t saved_norm = s_prefs.getUChar("norm", 0xFF);
  if (saved_norm > TVLINK_NORM_MAX) saved_norm = s_prefs.getBool("pal", true) ? TVLINK_NORM_PAL : TVLINK_NORM_NTSC;
  s_current_norm = saved_norm;
  s_requested_norm = saved_norm;
  s_panel_mutex = xSemaphoreCreateMutex();

  // Image en reconstruction : 16 bandes separees (pas besoin d'un bloc contigu
  // de 60 Ko), initialisees en noir, plus une bande de reserve pour la
  // decompression.
  for (int i = 0; i <= TVLINK_NUM_BANDS; i++) {
    s_bands[i] = (uint8_t *)heap_caps_malloc(TVLINK_BAND_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_bands[i]) {
      Serial.println("[TV] memoire insuffisante pour l'image : arret.");
      for (;;) delay(1000);
    }
    memset(s_bands[i], 0, TVLINK_BAND_BYTES);
  }
  s_rx.begin(s_bands, s_bands[TVLINK_NUM_BANDS]);

  // Video demarree ici, donc sur le coeur 1 (son interruption y restera).
  set_norm_resolution(s_current_norm);
  tv.setColorDepth(lgfx::color_depth_t::rgb332_1Byte);   // 1 octet par pixel, comme la liaison
  if (!tv.init()) Serial.println("[TV] echec d'initialisation de la sortie video.");
  Serial.printf("[TV] video composite sur GPIO%d, %s, image %dx%d, palette 24 bits envoyee par la console.\n",
                PIN_VIDEO, norm_geom(s_current_norm).name, (int)s_panel_w, TVLINK_HEIGHT);
  draw_status_screen(SCREEN_WAITING);

  xTaskCreatePinnedToCore(link_task, "tvlink", 4096, nullptr, 20, nullptr, 0);
  Serial.printf("[TV] son PWM sur GPIO%d. Appui long sur BOOT : PAL 50 / NTSC / PAL 60 (console debranchee).\n", PIN_AUDIO);
}

void loop() {
  uint32_t now = millis();

  if (s_standard_change) {
    uint8_t norm = s_requested_norm;
    s_standard_change = false;
    if (norm != s_current_norm) apply_standard(norm);
  }

  poll_boot_button(now);

  // Choix de l'ecran : jeu (images recues), console dans ses menus, attente.
  bool alive = s_ever_packet && (now - s_last_packet_ms) < LINK_IDLE_MS;
  bool config_fresh = (now - s_last_config_ms) < LINK_IDLE_MS;
  bool game = s_ever_frame && ((now - s_last_frame_ms) < LINK_IDLE_MS || (config_fresh && s_cfg_in_game));
  Screen want;
  if (alive && config_fresh && s_cfg_version != TVLINK_PROTOCOL_VERSION) want = SCREEN_MISMATCH;
  else if (game) want = SCREEN_GAME;
  else if (alive) want = SCREEN_MENU;
  else want = SCREEN_WAITING;

  static uint32_t last_redraw = 0;
  if (want != SCREEN_GAME && (want != s_screen || now - last_redraw > 2000)) {
    draw_status_screen(want);   // redessine aussi toutes les 2 s (compteurs)
    last_redraw = now;
  }

  if (PIN_LED >= 0) digitalWrite(PIN_LED, (s_ever_packet && now - s_last_packet_ms < 150) ? HIGH : LOW);
  print_stats(now);
  delay(20);
}
