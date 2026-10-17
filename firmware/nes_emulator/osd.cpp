// osd.cpp - couche "OSD" de nofrendo adaptee a cette carte : tout ce qui
// relie le coeur d'emulation au materiel.
//
//  - image : conversion du bitmap NES en RGB565 et envoi a l'ecran ILI9341
//    (vid_flush(), normal ou plein ecran, voir lcd_fullscreen.h), ou envoi
//    au coprocesseur video en sortie Televiseur (video_link.h) ;
//  - son : flux genere par l'APU, envoye en I2S au codec ES8311 ;
//  - manettes : osd_getinput() (controller.cpp) ;
//  - ROMs : liste des jeux (carte SD et memoire interne, catalogue),
//    chargement en PSRAM, controle prealable (osd_game_precheck()) ;
//  - logo de demarrage, ancienne interface des menus (USE_LVGL_UI = 0),
//    menu pause en partie, jeux choisis depuis le telephone, sauvegardes.
//
// Ecran : DISPLAY_WIDTH/DISPLAY_HEIGHT viennent de tft_driver.h (320x240).
// Carte SD : SD_MMC a la place de SD.h en SPI du depot d'origine (meme
// interface fs::FS/File d'Arduino).
//
// Descend de la couche osd du portage esp32-nesemu d'Espressif : osd.c
// (domaine public) et video_audio.c (Copyright 2015-2016 Espressif Systems
// (Shanghai) PTE LTD, licence Apache-2.0, texte dans LICENSES/), largement
// reecrite depuis. Voir NOTICE.
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_random.h>    // Numero de liste aleatoire au demarrage
#include <Preferences.h>  // Persistance du volume en NVS, voir plus bas
#include "FS.h"
#include "SD_MMC.h"
#include "hw_config.h"
#include "tft_driver.h"
#include "turning.h"
#include "crash_guard.h"
#include "video_link.h"
#include "wifi_boot.h"
#include "wifi_portal.h"
#include "bluetooth_controller.h"   // Demarrage du Bluetooth apres le Wi-Fi (show_menu)
#include "games_link.h"             // Jeux choisis depuis le telephone
#include "tv_menu.h"                // Liste des jeux sur la television
#include "osd_menu_api.h"   // MAX_GAMES + fonctions d'acces pour ui_lvgl.cpp
#include "game_catalog.h"   // Tous les jeux (PSRAM), liste affichee par pages
#include "rom_storage.h"    // Jeux sur carte SD OU en memoire interne (flash)
static_assert(OSD_WHERE_SD == ROM_WHERE_SD && OSD_WHERE_FLASH == ROM_WHERE_FLASH,
              "osd_menu_api.h et rom_storage.h doivent garder les memes valeurs");

#if USE_TOUCH_CONTROLS
#include "touch_controls.h"
#endif
#include "lcd_fullscreen.h"   // Plein ecran sur le LCD

#if USE_LVGL_UI
#include "ui_lvgl.h"        // Menus LVGL 8 (voir hw_config.h)
#endif

#if ENABLE_SOUND
#include <driver/i2s.h>
#endif

#include <string.h>
#include <stdarg.h>

extern "C" {
#include "noftypes.h"
#include "bitmap.h"
#include "osd.h"
#include "nofrendo.h"
#include "nesinput.h"
#include "event.h"
#include "nofconfig.h"
// Mapper gere par l'emulateur ? (nes_mmc.c)
bool mmc_peek(int map_num);
// "Recommencer le jeu" du menu pause (nes.c)
void nes_reset(int reset_type);
}
#include "savestate.h"      // Sauvegarde instantanee, RAM de sauvegarde des cartouches
#include "game_library.h"   // Recents, favoris, fichiers de sauvegarde

#define NES_SCREEN_WIDTH 256
#define NES_SCREEN_HEIGHT 240

// DISPLAY_WIDTH / DISPLAY_HEIGHT : fournis par tft_driver.h (320x240),
// PAS redefinis ici - voir l'explication en tete de fichier (point 2).

// AUDIO_SAMPLE_RATE = NES_AUDIO_SAMPLE_RATE (hw_config.h), PAS redefini a une
// valeur separee ici : audio_codec.cpp doit utiliser EXACTEMENT la meme
// frequence, sinon le son sortirait a la mauvaise vitesse/hauteur.
#define AUDIO_SAMPLE_RATE NES_AUDIO_SAMPLE_RATE
// Garde-fou de compilation : le coprocesseur video (canal audio, video_link.h)
// suppose exactement cette meme frequence d'echantillonnage. Si l'une des deux
// definitions change un jour sans l'autre, le son reproduit par la television
// sortirait a la mauvaise vitesse/hauteur - mieux vaut une erreur de
// compilation immediate qu'un bug audio silencieux.
#if ENABLE_VIDEO_LINK && (VIDEO_LINK_AUDIO_SAMPLE_RATE != AUDIO_SAMPLE_RATE)
#error "VIDEO_LINK_AUDIO_SAMPLE_RATE (video_link.h) doit etre egal a AUDIO_SAMPLE_RATE (osd.cpp / NES_AUDIO_SAMPLE_RATE dans hw_config.h)"
#endif
#define AUDIO_GAIN 100
#define TARGET_FRAME_MICROS 16666

// Volume logiciel de depart (35 %), utilise seulement au tout premier
// demarrage, avant tout reglage enregistre (voir load_saved_volume() /
// save_volume_if_changed() juste en dessous). Le volume materiel du codec
// (ES8311_VOICE_VOLUME, audio_codec.cpp) ne change pas : seul ce
// multiplicateur logiciel varie, jusqu'a 200 %. Une seule variable sert a la
// fois a l'affichage et au son.
int master_volume = 35;
bool show_fps = false;
bool select_pressed = false;
static bool runtime_sound_enabled = false;

// Persistance du volume en NVS (bibliotheque Preferences d'Arduino-ESP32, meme
// mecanisme que l'etalonnage tactile - "touchcal", touch_controls.cpp - et la
// liste noire de ROMs - "nescrash", crash_guard.cpp), dans son PROPRE espace
// de noms ("nesvol") pour ne jamais interferer avec ces deux-la.
#define VOLUME_NVS_NAMESPACE "nesvol"
static Preferences volume_prefs;

// A appeler UNE FOIS, tout au debut de show_menu() - AVANT show_magic()
// (qui peut deja jouer un son) - pour que le tout premier son entendu
// utilise deja le volume enregistre, pas le repli 35% ci-dessus.
static int s_volume_saved = -1;

static void load_saved_volume() {
  volume_prefs.begin(VOLUME_NVS_NAMESPACE, false);
  master_volume = volume_prefs.getInt("vol", master_volume);
  s_volume_saved = master_volume;
  Serial.printf("[VOLUME] Reglage rappele depuis la NVS : %d%%\n", master_volume);
}

// A appeler quand l'ecran Reglages se ferme (voir show_menu() plus bas) -
// n'ecrit en NVS QUE si la valeur a reellement change pendant cette
// visite de l'ecran, pour ne pas user la flash a chaque simple
// ouverture/fermeture de Reglages sans le moindre reglage touche (la NVS,
// comme toute flash, a un nombre fini de cycles d'effacement/ecriture -
// negligeable pour un usage occasionnel du volume, mais autant l'eviter
// quand ca ne coute rien).
static void save_volume_if_changed(int volume_before) {
  if (master_volume != volume_before) {
    volume_prefs.putInt("vol", master_volume);
    s_volume_saved = master_volume;
    Serial.printf("[VOLUME] Reglage enregistre en NVS : %d%%\n", master_volume);
  }
}

extern TFTDriver tft;
extern "C" int nes_get_gamepad_state();

static uint16_t scale_color_565(uint16_t color, uint8_t alpha) {
  uint8_t r = (color >> 11) & 0x1F;
  uint8_t g = (color >> 5) & 0x3F;
  uint8_t b = color & 0x1F;

  r = (r * alpha) / 255;
  g = (g * alpha) / 255;
  b = (b * alpha) / 255;

  return (uint16_t)((r << 11) | (g << 5) | b);
}

static uint16_t swap_rb_565(uint16_t color) {
  uint16_t r = (color >> 11) & 0x1F;
  uint16_t g = (color >> 5) & 0x3F;
  uint16_t b = color & 0x1F;
  return (uint16_t)((b << 11) | (g << 5) | r);
}

// Logo de demarrage en fondu, fluide si la PSRAM est disponible (c'est le
// cas sur l'ESP32-S3R8 ; sinon, repli sur un dessin ligne par ligne) :
//   1. La permutation R/B est calculee UNE SEULE FOIS, a la toute premiere
//      image (tampon PSRAM 'magic_base'), plutot qu'a chaque etape.
//   2. Chaque etape ne fait plus qu'un assombrissement (une multiplication
//      par pixel, peu couteux) puis UN SEUL appel tft.pushImage() pour
//      l'image complete - exactement le meme principe que vid_flush()
//      utilise deja pour l'image de jeu quand un tampon plein cadre est
//      disponible (voir plus bas, "frame_buffer") - au lieu de 240 appels
//      SPI separes par etape.
static uint16_t *magic_base = NULL;   // image de base, R/B deja permute, calculee une seule fois
static uint16_t *magic_frame = NULL;  // tampon de travail reenvoye a chaque etape

static void draw_magic(uint8_t alpha) {
  if (!magic_base) {
    magic_base = (uint16_t *)heap_caps_malloc((size_t)hakki_W * hakki_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (magic_base) {
      for (int i = 0; i < hakki_W * hakki_H; i++) {
        magic_base[i] = swap_rb_565(pgm_read_word(&hakki[i]));
      }
      magic_frame = (uint16_t *)heap_caps_malloc((size_t)hakki_W * hakki_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
      if (!magic_frame) {
        heap_caps_free(magic_base);
        magic_base = NULL;
      }
    }
  }

  if (magic_base && magic_frame) {
    // Chemin rapide (PSRAM disponible) : un seul assombrissement + un seul
    // envoi SPI pour l'image complete.
    for (int i = 0; i < hakki_W * hakki_H; i++) {
      magic_frame[i] = scale_color_565(magic_base[i], alpha);
    }
    tft.pushImage(0, 0, hakki_W, hakki_H, magic_frame, true);
    return;
  }

  // Repli (PSRAM indisponible) : dessin ligne par ligne.
  static uint16_t line[hakki_W];
  for (int y = 0; y < hakki_H; y++) {
    int row = y * hakki_W;
    for (int x = 0; x < hakki_W; x++) {
      uint16_t c = pgm_read_word(&hakki[row + x]);
      c = swap_rb_565(c);
      line[x] = scale_color_565(c, alpha);
    }
    // "true" (octets inverses par TFT_eSPI), comme le chemin rapide ci-dessus
    // - sans lui, les couleurs de ce repli etaient fausses.
    tft.pushImage(0, y, hakki_W, 1, line, true);
  }
}

static void show_magic() {
  static bool shown = false;
  if (shown) {
    return;
  }
  shown = true;

  const int fade_in_ms = 300;
  const int hold_ms = 500;
  const int fade_out_ms = 500;
  const int steps = 30;
  const int step_delay_in = fade_in_ms / steps;
  const int step_delay_out = fade_out_ms / steps;

  tft.fillScreen(0x0000);
  for (int i = 0; i <= steps; i++) {
    uint8_t alpha = (uint8_t)((i * 255) / steps);
    draw_magic(alpha);
    delay(step_delay_in);
  }

  // Remplace le simple delay(hold_ms) d'origine QUAND ENABLE_WIFI_PORTAL vaut
  // 1 (wifi_boot.h) : le logo reste affiche pendant la tentative de connexion
  // WiFi (au lieu d'un delai silencieux), avec un badge de statut anime puis
  // final dessine par wifi_boot_connect() elle-meme (wifi_boot.cpp - voir son
  // commentaire pour le pourquoi de cette repartition). Tant que
  // ENABLE_WIFI_PORTAL vaut 0, wifi_boot_connect() ne fait rien et le logo
  // reste affiche hold_ms.
  wifi_boot_connect();
  // Pas de connexion pendant le logo (Wi-Fi desactive, ou coupe au
  // demarrage) : le logo reste le temps prevu.
  if (g_wifi_boot_status == WIFI_BOOT_DISABLED) delay(hold_ms);

  for (int i = steps; i >= 0; i--) {
    uint8_t alpha = (uint8_t)((i * 255) / steps);
    draw_magic(alpha);
    delay(step_delay_out);
  }

  tft.fillScreen(0x0000);

  // Tampons PSRAM du logo utiles uniquement pendant cette animation
  // (shown=true empeche tout second appel) - liberes ici pour rendre cette
  // memoire au reste du programme (menu, puis jeu).
  if (magic_frame) { heap_caps_free(magic_frame); magic_frame = NULL; }
  if (magic_base) { heap_caps_free(magic_base); magic_base = NULL; }
}

// Tampons audio de 1024 echantillons mono : a 16000 Hz = 64 ms, large marge
// par rapport aux ~42 ms observes entre deux appels a ~24 im/s.
static int16_t stereo_buffer[2048];
static int16_t mono_buffer[1024];
// Son produit par morceaux de AUDIO_CHUNK_SAMPLES (taille de mono_buffer) ; au
// plus AUDIO_MAX_SAMPLES_PER_FLUSH par appel de vid_flush() (128 ms a 16000
// Hz), voir le bloc son de vid_flush().
#define AUDIO_CHUNK_SAMPLES 1024
#define AUDIO_MAX_SAMPLES_PER_FLUSH 2048
static void (*emulator_audio_callback)(void *buffer, int length) = NULL;
// Copie mono DEJA mise a l'echelle (volume + gain, memes valeurs que ce qui
// part reellement au codec local), remplie dans la meme boucle que
// stereo_buffer plus bas et transmise telle quelle au coprocesseur video via
// video_link_queue_audio() - voir vid_flush().
static int16_t audio_link_buffer[1024];
static_assert(AUDIO_CHUNK_SAMPLES <= (int)(sizeof(mono_buffer) / sizeof(mono_buffer[0])) &&
              2 * AUDIO_CHUNK_SAMPLES <= (int)(sizeof(stereo_buffer) / sizeof(stereo_buffer[0])) &&
              AUDIO_CHUNK_SAMPLES <= (int)(sizeof(audio_link_buffer) / sizeof(audio_link_buffer[0])),
              "AUDIO_CHUNK_SAMPLES depasse la taille des tampons audio");

static uint16_t myPalette565[256];
static uint16_t myPalette565_swapped[256];
// Palette de l'image du jeu sur l'ecran LCD (osd_blit(), rendu ligne par ligne
// de vid_flush()). tft.pushFrame() et tft.pushImage(..., true) demandent a
// TFT_eSPI d'inverser les deux octets de chaque pixel (setSwapBytes(true)) :
// c'est ce qu'attend une couleur RGB565 normale, comme celles des menus LVGL
// (ui_lvgl.cpp, disp_flush) et du logo (draw_magic()). La palette "_swapped",
// heritee du pilote du depot d'origine (qui envoyait les octets tels quels),
// etait donc inversee deux fois. Voir LCD_GAME_COLOR_FIX dans hw_config.h et
// pushImage() dans tft_driver.h.
#if LCD_GAME_COLOR_FIX
#define LCD_GAME_PALETTE myPalette565
#else
#define LCD_GAME_PALETTE myPalette565_swapped
#endif
// Palette RGB332 (1 octet/pixel : 3 bits rouge, 3 bits vert, 2 bits bleu),
// utilisee UNIQUEMENT par la liaison vers le coprocesseur video (voir
// video_link.h) : une image 256x240 tient en 61 440 octets au lieu de
// 122 880 en RGB565, deux fois moins de donnees sur la liaison SPI. L'ecran
// LCD utilise LCD_GAME_PALETTE (ci-dessus, myPalette565 par defaut).
static uint8_t myPaletteRGB332[256];
// Palette exacte du jeu (256 couleurs x R, G, B) pour la carte video
// (protocole v3) : la television recoit les indices et ces couleurs. En PSRAM
// (prise au premier osd_setpalette) : la memoire interne est comptee pour le
// Wi-Fi des menus.
static uint8_t *myPaletteRGB888 = NULL;
static bitmap_t *game_bitmap = NULL;
static bool video_ready = false;
static uint16_t *frame_buffer = NULL;
static bool low_mem_video_mode = false;

unsigned long frame_start_time = 0;
unsigned long frame_count = 0;
unsigned long last_fps_update = 0;
int current_fps = 0;

#define INP_JOYPAD0 0x0001
static nesinput_t joypad_p1;

#define HW_MASK_A 0x01
#define HW_MASK_B 0x02
#define HW_MASK_SELECT 0x04
#define HW_MASK_START 0x08
#define HW_MASK_UP 0x10
#define HW_MASK_DOWN 0x20
#define HW_MASK_LEFT 0x40
#define HW_MASK_RIGHT 0x80

#define INP_PAD_A 0x01
#define INP_PAD_B 0x02
#define INP_PAD_SELECT 0x04
#define INP_PAD_START 0x08
#define INP_PAD_UP 0x10
#define INP_PAD_DOWN 0x20
#define INP_PAD_LEFT 0x40
#define INP_PAD_RIGHT 0x80

esp_timer_handle_t nes_timer_handle = NULL;
void (*emu_timer_callback)(void) = NULL;
static uint32_t s_timer_period_us = 1000000 / 60;   // Periode de la minuterie (relance apres pause)

void IRAM_ATTR timer_callback_handler(void *arg) {
  if (emu_timer_callback) emu_timer_callback();
}

extern char *global_rom_data;
extern int global_rom_size;

extern "C" bool vid_preload_rom(const char *path);
extern "C" int osd_rom_open(const char *path);
extern "C" int osd_rom_read(void *dst, int len);
extern "C" void osd_rom_close(void);

// MAX_GAMES (50) est defini dans osd_menu_api.h, partage avec ui_lvgl.cpp.
// Noms de 64 caracteres : de quoi garder le nom complet des ROMs, indications
// de region comprises ("Life Force - Salamander (Europe).nes") - un nom
// tronque ne permettrait plus d'ouvrir ni de deplacer le fichier.
struct {
  char names[MAX_GAMES][64];
  int count;
  int selected;
  // Taille de chaque fichier (octets), relevee pendant le parcours de la carte
  // SD - affichee sous le nom du jeu dans la liste LVGL.
  uint32_t sizes[MAX_GAMES];
  // Emplacement de chaque jeu (ROM_WHERE_SD ou ROM_WHERE_FLASH, voir
  // rom_storage.h).
  uint8_t where[MAX_GAMES];
} game_list = {{}, 0, 0, {}, {}};

bool menu_brightness = true;

// Bascule d'affichage "Ecran LCD / Television", reglable dans le menu Reglages
// (voir show_menu() plus bas) - definie ici (declaree extern dans hw_config.h)
// pour rester accessible depuis controller.cpp (qui l'utilise pour savoir s'il
// doit ignorer le tactile). Sans effet tant que ENABLE_VIDEO_LINK vaut 0
// (video_link.h). Volontairement NON persistee en NVS : redemarre toujours a
// false (ecran LCD), voir le commentaire complet dans hw_config.h.
bool g_display_tv_mode = false;

// Voir le commentaire complet dans hw_config.h. Resume : true tant qu'on est
// dans un ecran de menu (toujours affiche sur l'ecran LCD), false une fois une
// partie lancee - mis a jour dans nes_emulator.ino juste avant
// nofrendo_main(). Permet a nes_get_gamepad_state() (controller.cpp) de ne
// desactiver le tactile en mode Television QUE pendant une partie, pas dans le
// menu Reglages qui reste, lui, toujours visible et tactile.
bool g_in_menu_screen = true;

// Partie affichee en plein ecran sur le LCD (voir LCD_FULLSCREEN_MODE dans
// hw_config.h et lcd_layout_update() plus bas).
bool g_lcd_fullscreen_active = false;

// Norme video (NTSC/PAL) pour la sortie television, reglable dans le menu
// Reglages (voir show_menu() plus bas) - voir le commentaire complet dans
// hw_config.h.
// PAL par defaut (televiseurs francais), et le choix fait dans les menus LVGL
// est memorise en NVS (ui_lvgl.cpp, "nesui"/"pal").
bool g_video_pal_mode = true;

// Message + spinner anime pendant le parcours de la carte SD. Cette fonction
// est appelee a CHAQUE affichage du menu (show_menu() plus bas), donc a chaque
// retour au menu apres une partie - pas seulement au tout premier demarrage
// (contrairement a show_magic(), qui ne se rejoue qu'une fois). Le spinner est
// mis a jour toutes les ~120 ms directement dans la boucle de parcours
// existante (base sur millis(), pas de delay() supplementaire qui ralentirait
// le scan) - indetermine (rotatif) plutot qu'une vraie barre de progression
// car le nombre total de fichiers n'est connu qu'a la toute fin du parcours.
// Le parcours lui-meme est separe du dessin : scan_games() et
// osd_games_rescan_quiet() (chemin LVGL, qui affiche son propre ecran
// d'attente) appellent tous deux scan_games_core() ci-dessous. Le rappel
// progress_cb (peut etre NULL) est appele a chaque fichier lu.
// La memoire interne est parcourue avant la carte SD ; un meme nom present
// aux deux endroits donne deux lignes, distinguees par leur couleur/etiquette.
static void scan_one_storage(int where, void (*progress_cb)()) {
  fs::FS *fs = rom_storage_fs(where);
  if (!fs) return;
  File root = fs->open("/");
  if (!root) {
    Serial.printf("[MENU] Racine illisible (%s)\n", where == ROM_WHERE_FLASH ? "memoire interne" : "carte SD");
    return;
  }
  int seen = 0;
  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;
    // Tous les jeux vont au catalogue (MAX_GAMES_TOTAL, PSRAM) ; la liste
    // affichee en est une page (MAX_GAMES).
    if (!entry.isDirectory()) {
      String name = entry.name();
      if ((name.endsWith(".nes") || name.endsWith(".NES")) && name.length() > 63) {
        // Un nom tronque ne designerait plus le bon fichier (jeu injouable) -
        // on l'ignore et on le signale ; a renommer sur PC (le portail web
        // refuse deja ces noms a l'envoi).
        Serial.printf("[MENU] Nom trop long (> 63 caracteres), ignore : %s\n", name.c_str());
      } else if (name.endsWith(".nes") || name.endsWith(".NES")) {
        catalog_add(entry.name(), (uint32_t)entry.size(), (uint8_t)where);
      }
    }
    entry.close();
    if (progress_cb) progress_cb();
    // Des centaines de fichiers - on laisse tourner les autres taches (chien
    // de garde) toutes les 32 entrees.
    if ((++seen & 31) == 0) vTaskDelay(1);
  }
  root.close();
}

// Numero de la liste (voir osd_games_generation()). Part d'une valeur
// aleatoire a chaque demarrage : une liste gardee par le telephone a travers
// un redemarrage de la console (ROM supprimee entre temps, numeros decales) ne
// passe pas pour la liste du moment.
static uint8_t s_games_generation = 0;
static bool s_games_generation_seeded = false;
// Numero du CATALOGUE (liste complete du telephone) : change a chaque
// relecture seulement - pas quand la page ou la recherche changent.
static uint8_t s_catalog_generation = 0;

// Jeu en cours de partie : index dans la page affichee (game_list ; -1 : pas
// sur cette page, ou aucun) et numero du catalogue (-1 : aucun). La page peut
// changer pendant une partie (jeu choisi sur le telephone dans une autre
// page) : view_materialize() recalcule alors s_running_index.
static int s_running_index = -1;
static int s_running_cat = -1;

// Page de la liste affichee (MAX_GAMES jeux) dans le catalogue de tous les
// jeux (game_catalog.h). game_list reste "la liste affichee" : tout le code
// qui la lit (menus, telephone, television, lancement) designe un jeu de la
// page en cours. Page gardee a travers le
// redemarrage de fin de partie (memoire RTC, pas effacee par esp_restart()) -
// sans filtre de recherche seulement.
#define VIEW_RTC_MAGIC 0x4E455350u   // "NESP"
RTC_NOINIT_ATTR static uint32_t s_rtc_view_magic;
RTC_NOINIT_ATTR static int32_t s_rtc_view_page;
RTC_NOINIT_ATTR static int32_t s_rtc_view_mode;   // Tous / favoris / recents
static char s_view_query[64] = "";
static int s_view_page = 0;
static bool s_view_restored = false;

// Liste affichee : tous les jeux, favoris ou recents (OSD_VIEW_*)
static int s_view_mode = 0;
static int16_t s_view_order[LIB_MAX_FAVORITES + LIB_MAX_RECENTS];
static int catalog_find_name(const char *name, int where);

static int build_view_order() {
  int n = 0;
  if (s_view_mode == OSD_VIEW_FAVS) {
    for (int ci = 0; ci < catalog_total() && n < LIB_MAX_FAVORITES; ci++) {
      const CatalogEntry *e = catalog_entry(ci);
      if (e && lib_fav_is(e->name)) s_view_order[n++] = (int16_t)ci;
    }
  } else if (s_view_mode == OSD_VIEW_RECENTS) {
    for (int i = 0; i < lib_recent_count(); i++) {
      const int ci = catalog_find_name(lib_recent_name(i), lib_recent_where(i));
      if (ci >= 0) s_view_order[n++] = (int16_t)ci;
    }
  }
  return n;
}

static void view_materialize() {
  if (s_view_mode == OSD_VIEW_ALL) catalog_set_view(s_view_query, s_view_page, MAX_GAMES);
  else catalog_set_view_list(s_view_query, s_view_page, MAX_GAMES, s_view_order, build_view_order());
  s_view_page = catalog_view_page();
  game_list.count = 0;
  for (int i = 0; i < catalog_view_count() && i < MAX_GAMES; i++) {
    const CatalogEntry *e = catalog_view_entry(i);
    if (!e) break;
    memcpy(game_list.names[i], e->name, sizeof(game_list.names[i]));
    game_list.names[i][63] = '\0';
    game_list.sizes[i] = e->size;
    game_list.where[i] = e->where;
    game_list.count++;
  }
  // Jeu en cours sur la nouvelle page ?
  s_running_index = -1;
  if (s_running_cat >= 0)
    for (int i = 0; i < game_list.count; i++)
      if (catalog_view_global(i) == s_running_cat) { s_running_index = i; break; }
  if (game_list.selected >= game_list.count) game_list.selected = (game_list.count > 0) ? game_list.count - 1 : 0;
  if (!s_view_query[0]) {
    s_rtc_view_page = s_view_page;
    s_rtc_view_mode = s_view_mode;
    s_rtc_view_magic = VIEW_RTC_MAGIC;
  }
}

static void scan_games_core(void (*progress_cb)()) {
  if (!s_games_generation_seeded) {
    s_games_generation = (uint8_t)esp_random();
    s_catalog_generation = (uint8_t)esp_random();
    s_games_generation_seeded = true;
  }
  s_games_generation++;   // Nouvelle liste
  s_catalog_generation++;   // Numeros du catalogue changes
  s_running_cat = -1;       // relecture : dans les menus seulement
  game_list.count = 0;
  if (!catalog_reset()) Serial.println("[MENU] Pas de memoire pour la liste des jeux !");
  const uint32_t t0 = millis();
  scan_one_storage(ROM_WHERE_FLASH, progress_cb);
  scan_one_storage(ROM_WHERE_SD, progress_cb);
  // Tri alphabetique (sans tenir compte des majuscules), carte SD avant
  // memoire interne a nom egal (tri du catalogue entier).
  catalog_sort();
  if (!s_view_restored) {
    s_view_restored = true;
    if (s_rtc_view_magic == VIEW_RTC_MAGIC && s_rtc_view_page >= 0 && s_rtc_view_page < MAX_GAMES_TOTAL) {
      s_view_page = (int)s_rtc_view_page;
      if (s_rtc_view_mode >= OSD_VIEW_ALL && s_rtc_view_mode <= OSD_VIEW_RECENTS) s_view_mode = (int)s_rtc_view_mode;
    }
  }
  view_materialize();
  Serial.printf("[MENU] %d jeu(x) lus en %lu ms%s ; page %d/%d\n", catalog_total(), (unsigned long)(millis() - t0),
                catalog_dropped() ? " (catalogue plein, jeux en trop ignores)" : "",
                catalog_view_page() + 1, catalog_view_pages());
}

// Chemin complet du jeu n° index ("/nom.nes" ou "flash:/nom.nes").
static void game_full_path(int index, char *out, size_t n) {
  rom_storage_make_path(game_list.where[index], game_list.names[index], out, n);
}

static int s_scan_spinner_i = 0;
static unsigned long s_scan_last_spin = 0;
static void scan_games_legacy_progress() {
  static const char *spinner_frames[] = {"|", "/", "-", "\\"};
  if (millis() - s_scan_last_spin > 120) {
    tft.drawFilledRect(220, 100, 20, 16, 0x0000);
    tft.drawString(220, 100, spinner_frames[s_scan_spinner_i % 4], 0xFFE0, 0x0000, 1);
    s_scan_spinner_i++;
    s_scan_last_spin = millis();
  }
}

void scan_games() {
  tft.fillScreen(0x0000);
  tft.drawString(20, 100, "Chargement des jeux...", 0xFFFF, 0x0000, 1);
  s_scan_spinner_i = 0;
  s_scan_last_spin = millis();
  scan_games_core(scan_games_legacy_progress);
}

// ==================== fonctions d'acces ====================
// Voir osd_menu_api.h.
int osd_games_count() { return game_list.count; }

// Catalogue et pages (osd_menu_api.h)
int osd_games_total() { return catalog_total(); }
int osd_games_matching() { return catalog_view_matching(); }
int osd_games_page() { return catalog_view_page(); }
int osd_games_page_count() { return catalog_view_pages(); }
int osd_games_page_first() { return catalog_view_first(); }
const char *osd_games_query() { return s_view_query; }
int osd_games_dropped() { return catalog_dropped(); }
void osd_games_set_view(const char *query, int page) {
  char q[sizeof(s_view_query)];   // query peut etre s_view_query lui-meme
  snprintf(q, sizeof(q), "%s", query ? query : "");
  memcpy(s_view_query, q, sizeof(s_view_query));
  s_view_page = page;
  s_games_generation++;   // les numeros de la liste changent (telephone)
  game_list.selected = 0;
  view_materialize();
}

uint8_t osd_games_generation() { return s_games_generation; }

// ---- catalogue complet (manette du telephone), osd_menu_api.h ----
int osd_catalog_count() { return catalog_total(); }
uint8_t osd_catalog_generation() { return s_catalog_generation; }
const char *osd_catalog_filename(int ci) {
  const CatalogEntry *e = catalog_entry(ci);
  return e ? e->name : "";
}
uint32_t osd_catalog_size(int ci) {
  const CatalogEntry *e = catalog_entry(ci);
  return e ? e->size : 0;
}
int osd_catalog_where(int ci) {
  const CatalogEntry *e = catalog_entry(ci);
  return e ? e->where : ROM_WHERE_SD;
}
bool osd_catalog_is_bad(int ci) {
  const CatalogEntry *e = catalog_entry(ci);
  if (!e) return false;
  char path[ROM_PATH_MAX];
  if (!rom_storage_make_path(e->where, e->name, path, sizeof(path))) return false;
  return crash_guard_is_rom_bad(path);
}
int osd_games_page_to_catalog(int index) {
  if (index < 0 || index >= game_list.count) return -1;
  return catalog_view_global(index);
}
static int page_index_of_catalog(int ci) {
  for (int i = 0; i < game_list.count; i++)
    if (catalog_view_global(i) == ci) return i;
  return -1;
}
int osd_games_show_catalog(int ci) {
  if (!catalog_entry(ci)) return -1;
  int i = page_index_of_catalog(ci);
  if (i >= 0) return i;   // deja sur la page affichee
  const int pos = catalog_view_position_of(ci);
  if (pos >= 0) osd_games_set_view(s_view_query, pos / MAX_GAMES);   // meme recherche, autre page
  else {
    // sans filtre, tous les jeux (la liste des favoris ou des recents ne le
    // contient pas) : rang = numero du catalogue
    s_view_mode = OSD_VIEW_ALL;
    osd_games_set_view("", ci / MAX_GAMES);
  }
  i = page_index_of_catalog(ci);
  if (i >= 0) game_list.selected = i;
  return i;
}
int osd_games_running_catalog() { return s_running_cat; }

// Numero du catalogue d'un nom de fichier (emplacement prefere where, -1 : n'importe lequel).
static int catalog_find_name(const char *name, int where) {
  int any = -1;
  for (int ci = 0; ci < catalog_total(); ci++) {
    const CatalogEntry *e = catalog_entry(ci);
    if (!e || strcmp(e->name, name) != 0) continue;
    if (where < 0 || e->where == where) return ci;
    if (any < 0) any = ci;
  }
  return any;
}

// ---- favoris, recents, reprise (osd_menu_api.h) ----
void osd_games_set_mode(int mode) {
  if (mode < OSD_VIEW_ALL || mode > OSD_VIEW_RECENTS) mode = OSD_VIEW_ALL;
  s_view_mode = mode;
  osd_games_set_view(s_view_query, 0);
}
int osd_games_mode() { return s_view_mode; }
bool osd_catalog_is_fav(int ci) {
  const CatalogEntry *e = catalog_entry(ci);
  return e && lib_fav_is(e->name);
}
bool osd_catalog_toggle_fav(int ci) {
  const CatalogEntry *e = catalog_entry(ci);
  return e ? lib_fav_toggle(e->name) : false;
}
bool osd_game_is_fav(int index) { return osd_catalog_is_fav(osd_games_page_to_catalog(index)); }
bool osd_game_toggle_fav(int index) { return osd_catalog_toggle_fav(osd_games_page_to_catalog(index)); }
bool osd_fav_full() { return lib_fav_full(); }
int osd_fav_count() { return lib_fav_count(); }
int osd_recent_count() { return lib_recent_count(); }
int osd_recent_catalog(int i) {
  if (i < 0 || i >= lib_recent_count()) return -1;
  return catalog_find_name(lib_recent_name(i), lib_recent_where(i));
}
uint32_t osd_library_generation() { return lib_generation(); }
int osd_resume_catalog(bool *has_state) {
  if (has_state) *has_state = false;
  for (int i = 0; i < lib_recent_count(); i++) {
    const int ci = catalog_find_name(lib_recent_name(i), lib_recent_where(i));
    if (ci < 0) continue;   // jeu supprime : le suivant
    if (has_state) *has_state = lib_save_find(lib_recent_name(i), lib_recent_where(i), "sta") >= 0;
    return ci;
  }
  return -1;
}

bool osd_catalog_has_resume(int ci) {
  const CatalogEntry *e = catalog_entry(ci);
  return e && lib_save_find(e->name, e->where, "sta") >= 0;
}
bool osd_game_has_resume(int index) { return osd_catalog_has_resume(osd_games_page_to_catalog(index)); }
const char *osd_games_name_at(int pos) {
  const CatalogEntry *e = catalog_view_at(pos);
  return e ? e->name : "";
}

// ---- jeux ecartes (osd_menu_api.h) ----
struct QuarEntry {
  char name[64];
  uint32_t size;
  uint8_t where;
};
static QuarEntry *s_quar = NULL;
static int s_quar_n = 0;

static void quar_add(const char *name, int where, uint32_t size, void *ctx) {
  (void)ctx;
  if (!s_quar || s_quar_n >= OSD_QUARANTINE_MAX) return;
  QuarEntry &e = s_quar[s_quar_n++];
  snprintf(e.name, sizeof(e.name), "%s", name);
  e.size = size;
  e.where = (uint8_t)where;
}

int osd_quarantine_refresh() {
  if (!s_quar) s_quar = (QuarEntry *)heap_caps_calloc(OSD_QUARANTINE_MAX, sizeof(QuarEntry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  s_quar_n = 0;
  if (!s_quar) return 0;
  rom_storage_quarantine_list(quar_add, NULL);
  return s_quar_n;
}
int osd_quarantine_count() { return s_quar_n; }
const char *osd_quarantine_name(int i) { return (s_quar && i >= 0 && i < s_quar_n) ? s_quar[i].name : ""; }
int osd_quarantine_where(int i) { return (s_quar && i >= 0 && i < s_quar_n) ? s_quar[i].where : ROM_WHERE_SD; }
uint32_t osd_quarantine_size(int i) { return (s_quar && i >= 0 && i < s_quar_n) ? s_quar[i].size : 0; }
const char *osd_quarantine_restore(int i) {
  if (!s_quar || i < 0 || i >= s_quar_n) return "Jeu introuvable";
  const QuarEntry &e = s_quar[i];
  const int r = rom_storage_quarantine_restore(e.where, e.name);
  switch (r) {
    case ROM_MOVE_OK: {
      char path[ROM_PATH_MAX];
      if (rom_storage_make_path(e.where, e.name, path, sizeof(path))) crash_guard_forget(path);
      return NULL;
    }
    case ROM_MOVE_ERR_EXISTS: return "Un jeu du m\xC3\xAAme nom est d\xC3\xA9j\xC3\xA0 dans la liste";
    case ROM_MOVE_ERR_ROOT: return "Racine pleine : supprimez des jeux ou formatez la carte en FAT32";
    case ROM_MOVE_ERR_UNAVAIL: return "Emplacement indisponible";
    case ROM_MOVE_ERR_NOFILE: return "Jeu introuvable";
    default: return "Erreur de la carte";
  }
}
bool osd_quarantine_delete(int i) {
  if (!s_quar || i < 0 || i >= s_quar_n) return false;
  return rom_storage_quarantine_remove(s_quar[i].where, s_quar[i].name);
}
const char *osd_take_notice() { return crash_guard_take_notice(); }

// Numero du catalogue d'un chemin complet ("/nom.nes", "flash:/nom.nes").
static int catalog_index_of_path(const char *path) {
  char p[ROM_PATH_MAX];
  for (int ci = 0; ci < catalog_total(); ci++) {
    const CatalogEntry *e = catalog_entry(ci);
    if (e && rom_storage_make_path(e->where, e->name, p, sizeof(p)) && strcmp(p, path) == 0) return ci;
  }
  return -1;
}
bool osd_tv_list_available() { return g_display_tv_mode && video_link_is_ready(); }

// Voir osd_menu_api.h. Lit les 16 octets d'en-tete et la taille du fichier ;
// calcul du mapper identique a rom_getheader() (nes_rom.c), y compris
// l'en-tete "DiskDude!".
int osd_game_precheck(int index, int *mapper) {
  if (mapper) *mapper = -1;
  if (index < 0 || index >= game_list.count) return OSD_PRECHECK_NOFILE;
  char path[ROM_PATH_MAX];
  game_full_path(index, path, sizeof(path));
  File f = rom_storage_open_read(path);
  if (!f) return OSD_PRECHECK_NOFILE;
  uint8_t h[16];
  const size_t got = f.read(h, sizeof(h));
  const size_t size = f.size();
  f.close();
  if (got != sizeof(h) || memcmp(h, "NES\x1A", 4) != 0) return OSD_PRECHECK_NOT_NES;
  static const uint8_t zeros[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  int m = h[6] >> 4;
  const bool disk_dude = (h[7] == 'D' && memcmp(h + 8, "iskDude!", 8) == 0);
  if (memcmp(h + 8, zeros, 8) == 0 || !disk_dude) m |= (h[7] & 0xF0);
  if (m == 0 && h[5] > 1) m = 3;   // Meme correction que rom_getheader() (nes_rom.c)
  if (mapper) *mapper = m;
  if (h[4] == 0) return OSD_PRECHECK_NOT_NES;   // aucune banque de programme
  const size_t need = 16 + ((h[6] & 0x04) ? 512 : 0) + (size_t)h[4] * 16384 + (size_t)h[5] * 8192;
  if (size < need) return OSD_PRECHECK_TRUNCATED;
  if (!mmc_peek(m)) return OSD_PRECHECK_MAPPER;
  return OSD_PRECHECK_OK;
}

const char *osd_precheck_text(int code, int mapper) {
  static char b[48];
  switch (code) {
    case OSD_PRECHECK_OK: return "";
    case OSD_PRECHECK_NOFILE: return "Fichier introuvable ou illisible";
    case OSD_PRECHECK_NOT_NES: return "Ce fichier n'est pas une ROM NES";
    case OSD_PRECHECK_TRUNCATED: return "ROM incompl\xC3\xA8te (fichier tronqu\xC3\xA9)";
    case OSD_PRECHECK_MAPPER:
      snprintf(b, sizeof(b), "Mapper %d non pris en charge", mapper);
      return b;
    default: return "Jeu impossible \xC3\xA0 lancer";
  }
}

const char *osd_game_filename(int index) {
  if (index < 0 || index >= game_list.count) return "";
  return game_list.names[index];
}

uint32_t osd_game_size(int index) {
  if (index < 0 || index >= game_list.count) return 0;
  return game_list.sizes[index];
}

bool osd_game_is_bad(int index) {
  if (index < 0 || index >= game_list.count) return false;
  char path[ROM_PATH_MAX];
  game_full_path(index, path, sizeof(path));
  return crash_guard_is_rom_bad(path);
}

int osd_game_where(int index) {
  if (index < 0 || index >= game_list.count) return ROM_WHERE_SD;
  return game_list.where[index];
}

// Suppression depuis l'ecran (ui_lvgl.cpp : appui long sur un jeu). Meme
// operation que la suppression par le portail web (rom_storage_remove).
bool osd_game_delete(int index) {
  if (index < 0 || index >= game_list.count) return false;
  char path[ROM_PATH_MAX];
  game_full_path(index, path, sizeof(path));
  bool ok = rom_storage_remove(path);
  Serial.printf("[MENU] Suppression de \"%s\" : %s\n", path, ok ? "OK" : "ECHEC");
  return ok;
}

const char *osd_flash_diag() {
  static char b[280];
  if (rom_storage_flash_ok()) return "";
  snprintf(b, sizeof(b), "M\xC3\xA9moire interne indisponible : %s.\n%s",
           rom_storage_flash_reason(), rom_storage_flash_detail());
  return b;
}

bool osd_storage_info(int where, uint32_t *free_kb, uint32_t *total_kb) {
  if (!rom_storage_available(where)) { *free_kb = 0; *total_kb = 0; return false; }
  *free_kb = (uint32_t)(rom_storage_free_bytes(where) / 1024);
  *total_kb = (uint32_t)(rom_storage_total_bytes(where) / 1024);
  return true;
}

void osd_games_rescan_quiet() {
  scan_games_core(NULL);
  if (game_list.selected >= game_list.count) {
    game_list.selected = (game_list.count > 0) ? game_list.count - 1 : 0;
  }
}

void osd_games_select(int index) {
  if (index >= 0 && index < game_list.count) game_list.selected = index;
}

int osd_get_volume() { return master_volume; }

void osd_set_volume(int percent) {
  if (percent < 0) percent = 0;
  if (percent > 200) percent = 200;
  master_volume = percent;
}

void osd_volume_commit() {
  if (master_volume != s_volume_saved) {
    volume_prefs.putInt("vol", master_volume);
    s_volume_saved = master_volume;
    Serial.printf("[VOLUME] Reglage enregistre en NVS : %d%%\n", master_volume);
  }
}

// Couleurs de l'ancienne interface (USE_LVGL_UI = 0), en RGB565 : bleu
// accent #3a6df0 et gris neutre #4a5064.
#define COLOR_ACCENT_BLUE  0x3B7E   // #3a6df0 - actif/selectionne (interrupteur ON, cercle A, ligne selectionnee)
#define COLOR_NEUTRAL_GREY 0x4A8C   // #4a5064 - inactif/secondaire (interrupteur OFF, cercle B, contour pilule)


// Interrupteur "pilule" (ecran Reglages) : fond bleu accent +
// rond blanc a droite si actif, fond gris + rond a gauche si inactif.
static void draw_toggle_switch(int x, int y, bool on) {
  const int w = 30, h = 16;
  uint16_t bg = on ? COLOR_ACCENT_BLUE : COLOR_NEUTRAL_GREY;
  tft.drawFilledRoundRect(x, y, w, h, h / 2, bg);
  int knob_d = h - 4;
  int knob_x = on ? (x + w - knob_d - 2) : (x + 2);
  tft.drawFilledCircle(knob_x + knob_d / 2, y + h / 2, knob_d / 2, 0xFFFF);
}

// Deux petits utilitaires pour la refonte de l'ecran Reglages en "lignes"
// label-a-gauche/controle-a-droite - la police embarquee (voir drawString(),
// tft_driver.h) est a largeur FIXE de 6 px/caractere a l'echelle 1, ce qui
// permet de calculer la largeur d'un texte sans avoir a le dessiner d'abord
// (meme technique que draw_zone_badge(), touch_controls.cpp).

// Dessine `text` cale a droite sur x_right (x_right = bord droit
// souhaite, PAS la position de depart).
static void draw_right_text(int x_right, int y, const char *text, uint16_t fg, uint16_t bg, int scale = 1) {
  int w = (int)strlen(text) * 6 * scale;
  tft.drawString(x_right - w, y, text, fg, bg, scale);
}

// Pilule pleine calee a droite sur x_right, texte en noir pour le contraste.
static void draw_status_pill(int x_right, int y_top, const char *text, uint16_t fill) {
  int text_w = (int)strlen(text) * 6;
  int w = text_w + 10;
  const int h = 16;
  int x = x_right - w;
  tft.drawFilledRoundRect(x, y_top, w, h, h / 2, fill);
  tft.drawString(x + 5, y_top + 4, text, 0x0000, fill, 1);
}

// Ecran "Portail actif", affiche depuis le menu Reglages (HW_MASK_START, voir
// plus bas). Vit ici (pas dans wifi_portal.cpp) parce que c'est ici que vivent
// deja HW_MASK_* et nes_get_gamepad_state() - meme repartition que le reste de
// ce fichier (wifi_portal.cpp ne sait que servir des routes HTTP, pas dessiner
// ni lire les boutons).
#if ENABLE_WIFI_PORTAL
static void run_wifi_portal_screen() {
  if (g_wifi_boot_status != WIFI_BOOT_STATION_OK && g_wifi_boot_status != WIFI_BOOT_AP_FALLBACK) {
    // WiFi desactive ou echec au demarrage (voir wifi_boot.cpp) - rien a
    // servir, message bref plutot que d'ouvrir un portail inaccessible.
    tft.fillScreen(0x0000);
    tft.drawString(20, 100, "WiFi indisponible", 0xF800, 0x0000, 1);
    tft.drawString(20, 120, "(echec au demarrage)", 0xF800, 0x0000, 1);
    delay(1500);
    return;
  }

  // Le serveur demarre automatiquement des que le WiFi se connecte
  // (wifi_boot.cpp, do_wifi_connect_attempt()) - cet appel est garde par
  // s_portal_started (wifi_portal.cpp) et ne fait donc plus rien dans le cas
  // courant ; conserve par securite pour le cas marginal ou cette fonction
  // serait atteinte avant que le WiFi n'ait fini de se connecter.
  wifi_portal_start();

  tft.fillScreen(0x0000);

  // Bande tactile dessinee EN PREMIER (elle repeint toute la bande gauche en
  // noir), puis le contenu, decale de PORTAL_X0 pour rester dans la bande
  // centrale - comme les ecrans GAME MENU et REGLAGES (MENU_X0/X1, SET_X0/X1).
#if USE_TOUCH_CONTROLS
  // Etiquette tactile "B" sur la zone correspondante (bande droite, zone 1) -
  // voir touch_controls.cpp pour le contexte complet.
  touch_controls_draw_portal_overlay();
  const int PORTAL_X0 = TOUCH_CONTENT_X0;
  const int PORTAL_X1 = TOUCH_CONTENT_X1;
#else
  const int PORTAL_X0 = 0;
  const int PORTAL_X1 = DISPLAY_WIDTH;
#endif
  const int PORTAL_W = PORTAL_X1 - PORTAL_X0;

  tft.drawFilledRect(PORTAL_X0, 0, PORTAL_W, 40, 0x001F);
  tft.drawString(PORTAL_X0 + 10, 10, "PORTAIL WIFI", 0xFFFF, 0x001F, 2);

  // Adresses a l'echelle 1 (6 px/caractere) : "http://255.255.255.255/" (24
  // caracteres) tient dans la bande centrale ; a l'echelle 2, il deborderait
  // de l'ecran et sur la bande tactile droite.
  tft.drawString(PORTAL_X0 + 10, 70, "Ouvre cette adresse :", 0xFFE0, 0x0000, 1);
  char url_line[48];
  if (g_wifi_boot_status == WIFI_BOOT_STATION_OK) {
    snprintf(url_line, sizeof(url_line), "http://%s/", g_wifi_boot_status_text);
    tft.drawString(PORTAL_X0 + 10, 95, url_line, 0x07E0, 0x0000, 1);
  } else {
    // Point d'acces (WIFI_BOOT_AP_FALLBACK) : g_wifi_boot_status_text
    // contient le SSID (voir wifi_boot.cpp), pas une IP - il faut
    // d'abord rejoindre ce reseau, PUIS ouvrir l'adresse fixe du point
    // d'acces (192.168.4.1, valeur par defaut de WiFi.softAP()).
    snprintf(url_line, sizeof(url_line), "Reseau: %s", g_wifi_boot_status_text);
    tft.drawString(PORTAL_X0 + 10, 95, url_line, 0x07FF, 0x0000, 1);
    tft.drawString(PORTAL_X0 + 10, 115, "puis http://192.168.4.1/", 0x07E0, 0x0000, 1);
  }

  // Cet ecran montre l'adresse exacte a saisir dans un navigateur pour joindre
  // le portail (etiquette "INFO", zone tactile START) ; le serveur lui-meme
  // demarre des que le Wi-Fi se connecte (wifi_boot.cpp). Le vrai bouton "B"
  // est le badge de la bande tactile droite
  // (touch_controls_draw_portal_overlay()), pas ce texte.
  tft.drawString(PORTAL_X0 + 10, DISPLAY_HEIGHT - 20, "B: Fermer", 0xFFFF, 0x0000, 1);

  bool running = true;
  unsigned long last_input = 0;
  while (running) {
    // server.handleClient() (wifi_portal_loop()) doit tourner tres
    // souvent pour rester reactif - pas de gros delay() dans cette
    // boucle, contrairement aux autres ecrans de menu.
    wifi_portal_loop();
    bluetooth_controller_radio_check();   // Pause du Bluetooth (point d'acces)

    if ((millis() - last_input) > 150) {
      int hw2 = nes_get_gamepad_state();
      if (hw2 & HW_MASK_B) {
        running = false;
      }
      last_input = millis();
    }
    delay(5);
  }

}
#endif

// Ecran "SORTIE VIDEO" de l'interface sans LVGL, ouvert par START depuis le
// menu principal : Sortie ECRAN LCD/TELEVISEUR et Norme NTSC/PAL. Toujours
// compile : sans coprocesseur (ENABLE_VIDEO_LINK = 0), video_link_is_ready()
// renvoie toujours false et video_link_set_video_standard() ne fait rien
// (voir video_link.cpp).
static void run_video_output_screen() {
  tft.fillScreen(0x0000);

#if USE_TOUCH_CONTROLS
  touch_controls_draw_video_output_overlay();
  const int VID_X0 = TOUCH_CONTENT_X0;
  const int VID_X1 = TOUCH_CONTENT_X1;
#else
  const int VID_X0 = 0;
  const int VID_X1 = DISPLAY_WIDTH;
#endif
  const int VID_W = VID_X1 - VID_X0;

  // Meme mise en page "en lignes" que le bloc AUDIO/WI-FI de l'ecran Reglages
  // - titre + legende de section grise, puis chaque reglage sur sa propre
  // ligne (etiquette+etat a gauche, interrupteur visuel a droite).
  tft.drawString(VID_X0 + 10, 10, "SORTIE VIDEO", 0xFFFF, 0x0000, 2);
  tft.drawString(VID_X0 + 10, 32, "AFFICHAGE", COLOR_NEUTRAL_GREY, 0x0000, 1);
  tft.drawString(VID_X0 + 10, DISPLAY_HEIGHT - 20,
                  "LEFT/RIGHT: Sortie  B: Norme  A: Fermer", 0xFFFF, 0x0000, 1);

  bool running = true;
  unsigned long last_input = 0;
  int last_tv_mode = -1;
  int last_pal_mode = -1;

  while (running) {
    if (last_tv_mode != (int)g_display_tv_mode) {
      tft.drawFilledRect(VID_X0 + 10, 46, VID_W - 20, 36, 0x0000);
      tft.drawString(VID_X0 + 10, 48,
                      g_display_tv_mode ? "Sortie: TELEVISEUR" : "Sortie: ECRAN LCD",
                      g_display_tv_mode ? 0x07FF : 0xFFE0, 0x0000, 1);
      draw_toggle_switch(VID_X0 + VID_W - 30, 46, g_display_tv_mode);
      if (g_display_tv_mode && !video_link_is_ready()) {
        // Avertissement seulement : rien n'empeche de choisir ce mode avant que le
        // coprocesseur ne soit cable.
        tft.drawString(VID_X0 + 10, 66, "(coprocesseur non detecte)", 0xFD20, 0x0000, 1);
      }
      last_tv_mode = (int)g_display_tv_mode;
    }

    if (last_pal_mode != (int)g_video_pal_mode) {
      tft.drawFilledRect(VID_X0 + 10, 90, VID_W - 20, 20, 0x0000);
      tft.drawString(VID_X0 + 10, 92,
                      g_video_pal_mode ? "Norme: PAL" : "Norme: NTSC",
                      g_video_pal_mode ? 0x07FF : 0xFFE0, 0x0000, 1);
      draw_toggle_switch(VID_X0 + VID_W - 30, 90, g_video_pal_mode);
      last_pal_mode = (int)g_video_pal_mode;
    }

    // Meme rythme de scrutation que run_wifi_portal_screen() juste au-dessus
    // (last_input remis a millis() sans condition en bas de ce bloc, PAS
    // uniquement quand un bouton est presse) : un nouveau relevé toutes les
    // ~150 ms, pas un anti-rebond declenche par l'action.
    if ((millis() - last_input) > 150) {
      int hw = nes_get_gamepad_state();
      if (hw & HW_MASK_LEFT || hw & HW_MASK_RIGHT) {
        g_display_tv_mode = !g_display_tv_mode;
        last_tv_mode = -1;  // force le redessin (inclut l'avertissement coprocesseur)
      }
      if (hw & HW_MASK_B) {
        g_video_pal_mode = !g_video_pal_mode;
        video_link_set_video_standard(g_video_pal_mode);
        last_pal_mode = -1;
      }
      if (hw & HW_MASK_A) {
        running = false;
      }
      last_input = millis();
    }
    delay(20);
  }
}

void draw_menu() {
  tft.fillScreen(0x0000);
  delay(100);

  tft.drawFilledRect(0, 0, DISPLAY_WIDTH, 50, 0xF800);
  delay(50);

  Serial.println("[MENU] Drawing title...");
  tft.drawString(20, 10, "NES EMULATOR", 0xFFFF, 0xF800, 2);

  Serial.println("[MENU] Drawing game list...");
  int start_y = 70;
  for (int i = 0; i < game_list.count && i < 4; i++) {
    uint16_t color = (i == game_list.selected) ? 0xF800 : 0xFFFF;
    char display_name[32];
    strncpy(display_name, game_list.names[i], 31);
    display_name[31] = '\0';

    char *dot = strchr(display_name, '.');
    if (dot) *dot = '\0';

    Serial.printf("  [%d] %s\n", i, display_name);
    tft.drawString(20, start_y + (i * 30), display_name, color, 0x0000, 1);
  }

  Serial.println("[MENU] Drawing controls...");
  tft.drawString(10, 220, "UP/DOWN:Select  A:Play  SELECT:Settings", 0xFFFF, 0x0000, 1);
  delay(100);
}

void draw_settings() {
  tft.fillScreen(0x0000);
  delay(50);

  tft.drawFilledRect(0, 0, DISPLAY_WIDTH, 50, 0x001F);

  tft.drawString(20, 10, "SETTINGS", 0xFFFF, 0x001F, 2);

  tft.drawString(20, 80, "Volume: ", 0xFFFF, 0x0000, 1);
  char vol_str[10];
  // "menu_volume" (variable separee) retiree du fichier - voir le commentaire
  // pres de master_volume, plus haut. Cette fonction-ci (draw_settings())
  // n'est de toute facon appelee nulle part ailleurs dans le projet
  // (verifie) : l'ecran Reglages reellement utilise est dessine plus bas, dans
  // show_menu().
  snprintf(vol_str, 10, "%d%%", master_volume);
  tft.drawString(100, 80, vol_str, 0x07E0, 0x0000, 1);

  tft.drawString(10, 220, "UP/DOWN:Volume  A:Save  B:Back", 0xFFFF, 0x0000, 1);
}

extern "C" int show_menu() {
  Serial.println("\n[MENU] Starting menu system...");

  // Rappelle le volume enregistre AVANT show_magic() (qui peut deja jouer un
  // son) - voir load_saved_volume() plus haut dans ce fichier.
  load_saved_volume();

#if USE_LVGL_UI
  // Interface LVGL : luminosite enregistree appliquee des le logo, puis tous
  // les menus sont confies a ui_lvgl.cpp. ui_lvgl_run_menu() ne rend la main
  // que lorsqu'un jeu a ete choisi (sans jeu, l'interface reste ouverte pour
  // permettre d'en envoyer par le portail).
  ui_lvgl_apply_saved_brightness();
  show_magic();
  // Bluetooth demarre ici, connexion Wi-Fi terminee - ou differe si la console
  // a ouvert son point d'acces (voir bluetooth_controller.h).
  bluetooth_controller_radio_check();
  {
    int chosen = ui_lvgl_run_menu();
    osd_games_select(chosen);
#if USE_TOUCH_CONTROLS
    // Avec le tactile, sur l'ecran LCD, l'ecran "Lancement..." de LVGL reste
    // affiche pendant le chargement de la ROM : setup() redessine ensuite les
    // deux bandes tactiles, et la premiere image du jeu recouvre le centre. En
    // mode Televiseur, l'image du jeu ne vient jamais sur le LCD : on l'efface
    // pour ne pas laisser "Lancement..." affiche toute la partie (comme les
    // menus).
    if (g_display_tv_mode) tft.fillScreen(0x0000);
#else
    tft.fillScreen(0x0000);
#endif
    return game_list.selected;
  }
#endif

  show_magic();
  bluetooth_controller_radio_check();   // Voir plus haut

  scan_games();
  Serial.printf("[MENU] Found %d games\n", game_list.count);

  if (game_list.count == 0) {
    Serial.println("[MENU] No games found!");
    tft.fillScreen(0xF800);
    delay(3000);
    return -1;
  }

  game_list.selected = 0;
  int top_index = 0;
  bool in_menu = true;
  unsigned long last_input = 0;
  int last_selected = -1;

  bool needs_full_redraw = true;
  bool needs_list_redraw = false;

#if USE_TOUCH_CONTROLS
  const int MENU_X0 = TOUCH_CONTENT_X0;
  const int MENU_X1 = TOUCH_CONTENT_X1;
#else
  const int MENU_X0 = 0;
  const int MENU_X1 = DISPLAY_WIDTH;
#endif
  const int MENU_W = MENU_X1 - MENU_X0;

  // ROMs defaillantes (voir crash_guard.h) : "!" en prefixe + couleur orange,
  // lisible meme si les teintes sont difficiles a distinguer. Ligne
  // selectionnee : fond bleu accent + triangle blanc (pas de coche : glyphe
  // absent de la police 5x7 embarquee, voir font_5x7 plus haut).
  auto draw_menu_item = [&](int screen_idx, int game_idx, bool selected) {
    int y_pos = 50 + (screen_idx * 40);
    uint16_t bg = selected ? COLOR_ACCENT_BLUE : 0x0000;

    char bad_path[ROM_PATH_MAX];
    game_full_path(game_idx, bad_path, sizeof(bad_path));
    bool is_bad = crash_guard_is_rom_bad(bad_path);
    uint16_t fg = is_bad ? 0xFD20 : 0xFFFF;

    char display_name[36];
    if (is_bad) {
      snprintf(display_name, sizeof(display_name), "! %s", game_list.names[game_idx]);
    } else {
      snprintf(display_name, sizeof(display_name), "%s", game_list.names[game_idx]);
    }
    char *dot = strchr(display_name, '.');
    if (dot) *dot = '\0';

    tft.drawFilledRect(MENU_X0, y_pos - 5, MENU_W, 35, bg);

    // Triangle cyan pour un jeu en memoire interne.
    uint16_t tri_color = selected ? 0xFFFF
                       : (game_list.where[game_idx] == ROM_WHERE_FLASH ? 0x07FF : COLOR_NEUTRAL_GREY);
    int tx = MENU_X0 + 6, ty = y_pos + 3;
    tft.drawFilledTriangle(tx, ty - 5, tx, ty + 5, tx + 8, ty, tri_color);

    tft.drawString(MENU_X0 + 24, y_pos, display_name, fg, bg, 1);
  };

  // Barre de defilement a droite de la liste, proportionnelle a la
  // position dans game_list. N'apparait que si tous les jeux ne tiennent pas
  // dans les 4 lignes visibles.
  auto draw_scrollbar = [&]() {
    if (game_list.count <= 4) return;
    const int track_y0 = 45, track_h = 155;
    int bar_h = max(10, track_h * 4 / game_list.count);
    int max_top = game_list.count - 4;
    int bar_y = track_y0 + (max_top > 0 ? (track_h - bar_h) * top_index / max_top : 0);
    tft.drawFilledRect(MENU_X1 - 4, track_y0, 3, track_h, 0x2104);
    tft.drawFilledRect(MENU_X1 - 4, bar_y, 3, bar_h, COLOR_ACCENT_BLUE);
  };

  while (in_menu) {
    // Liste relue des qu'un envoi ou une suppression de ROM a reussi depuis le
    // portail web (g_portal_games_changed, wifi_portal.h). game_list.selected et
    // top_index sont ramenes a des valeurs sures si la liste a retreci (ex. ROM
    // supprimee alors qu'elle etait selectionnee).
#if ENABLE_WIFI_PORTAL
    if (g_portal_games_changed) {
      g_portal_games_changed = false;
      scan_games();
      if (game_list.selected >= game_list.count) {
        game_list.selected = (game_list.count > 0) ? game_list.count - 1 : 0;
      }
      top_index = 0;
      last_selected = -1;
      needs_full_redraw = true;
    }
#endif
    if (needs_full_redraw) {
      tft.fillScreen(0x0000);
#if USE_TOUCH_CONTROLS
      touch_controls_draw_menu_overlay();
#endif
      tft.drawString(MENU_X0 + 10, 10, "GAME MENU", 0xFFFF, 0x0000, 2);
      // Numero de version en petit, en haut a droite de la bande centrale (pas de
      // l'ecran, pour ne pas recouvrir l'etiquette tactile "PLAY").
      tft.drawString(MENU_X1 - 30, 10, NES_EMULATOR_VERSION_SHORT, 0x7BEF, 0x0000, 1);

      for (int i = 0; i < 4 && (top_index + i) < game_list.count; i++) {
        draw_menu_item(i, top_index + i, (top_index + i) == game_list.selected);
      }
      draw_scrollbar();

      // Les vrais boutons "A" et "SELECT" (ceux qui fonctionnent) sont les
      // badges de la bande tactile droite
      // (touch_controls_draw_menu_overlay()), pas ce texte.
      tft.drawString(MENU_X0 + 10, DISPLAY_HEIGHT - 40, "UP/DN: Naviguer", 0x07E0, 0x0000, 1);
      tft.drawString(MENU_X0 + 10, DISPLAY_HEIGHT - 20, "A: Jouer  SELECT: Reglages", 0xFFFF, 0x0000, 1);

      last_selected = game_list.selected;
      needs_full_redraw = false;
      needs_list_redraw = false;
    }
    else if (needs_list_redraw) {
      tft.drawFilledRect(MENU_X0, 45, MENU_W, 155, 0x0000);

      for (int i = 0; i < 4 && (top_index + i) < game_list.count; i++) {
        draw_menu_item(i, top_index + i, (top_index + i) == game_list.selected);
      }
      draw_scrollbar();

      last_selected = game_list.selected;
      needs_list_redraw = false;
    }
    else if (last_selected != game_list.selected) {
      if (last_selected >= top_index && last_selected < top_index + 4) {
        draw_menu_item(last_selected - top_index, last_selected, false);
      }
      draw_menu_item(game_list.selected - top_index, game_list.selected, true);
      last_selected = game_list.selected;
    }

    int hw = nes_get_gamepad_state();

    if ((millis() - last_input) > 150) {
      if (hw & HW_MASK_UP) {
        if (game_list.selected > 0) {
          game_list.selected--;
          if (game_list.selected < top_index) {
            top_index = game_list.selected;
            needs_list_redraw = true;
          }
          last_input = millis();
        }
      }
      if (hw & HW_MASK_DOWN) {
        if (game_list.selected < game_list.count - 1) {
          game_list.selected++;
          if (game_list.selected >= top_index + 4) {
            top_index = game_list.selected - 3;
            needs_list_redraw = true;
          }
          last_input = millis();
        }
      }
      // Garde-fou game_list.count > 0 : supprimer la derniere ROM via le portail
      // pendant que le menu est ouvert peut vider la liste en cours de route -
      // sans cette garde, A tenterait de lancer un index inexistant.
      if (hw & HW_MASK_A && game_list.count > 0) {
        tft.fillScreen(0x0000);
        delay(300);
        return game_list.selected;
      }
      if (hw & HW_MASK_SELECT) {
        bool in_settings = true;
        unsigned long settings_input = 0;
        // Volume releve a l'ouverture : il n'est enregistre en NVS a la fermeture que
        // s'il a change (voir save_volume_if_changed()).
        int volume_at_settings_open = master_volume;
        int last_menu_volume = -1;
        int last_menu_tv_mode = -1;    // Force le premier dessin de la ligne "Sortie"
        int last_menu_pal_mode = -1;   // Force le premier dessin de la ligne "Norme"
        int last_menu_wifi_status = -1; // Force le premier dessin de la ligne "WiFi"
        bool settings_full_redraw = true;

        // Meme raisonnement que MENU_X0/X1 ci-dessus, applique a ce
        // sous-menu : sans ca, la barre de volume (qui allait jusqu'a
        // DISPLAY_WIDTH-40 = x=300) recouvrait l'etiquette tactile "OK" a
        // droite (bande 288..319).
#if USE_TOUCH_CONTROLS
        const int SET_X0 = TOUCH_CONTENT_X0;
        const int SET_X1 = TOUCH_CONTENT_X1;
#else
        const int SET_X0 = 0;
        const int SET_X1 = DISPLAY_WIDTH;
#endif
        const int SET_W = SET_X1 - SET_X0;

        while (in_settings) {
          if (settings_full_redraw) {
            tft.fillScreen(0x0000);
#if USE_TOUCH_CONTROLS
            touch_controls_draw_settings_overlay();
#endif
            // Titre generalise ("SETTINGS" au lieu de "VOLUME SETTINGS")
            // puisque cet ecran couvre aussi la bascule d'affichage
            // ci-dessous.
            tft.drawString(SET_X0 + 10, 10, "SETTINGS", 0xFFFF, 0x0000, 2);
            // Labels uniformises avec l'ecran GAME MENU ci-dessus (meme format
            // "BOUTON: Action", meme vocabulaire francais - "OK"/"Cancel"
            // deviennent "Fermer"/"Annuler").
            // Texte simple, sur UNE SEULE ligne par groupe.
            {
              int by1 = DISPLAY_HEIGHT - 40;
              char line1[48];
              snprintf(line1, sizeof(line1), "UP/DN: Vol"
#if ENABLE_VIDEO_LINK
                       "  L/R: Sortie"
#elif ENABLE_WIFI_PORTAL
                       // L/R bascule "WiFi active" (voir le bloc WI-FI plus
                       // bas) dans cette configuration (L/R libres puisque
                       // ENABLE_VIDEO_LINK est desactive).
                       "  L/R: WiFi"
#endif
#if ENABLE_WIFI_PORTAL
                       "  START: Portail"
#endif
              );
              tft.drawString(SET_X0 + 10, by1, line1, 0x07E0, 0x0000, 1);
            }
            // "B: Annuler" devient "B: Norme" quand le lien video est actif : B sert
            // alors au reglage NTSC/PAL (voir le traitement de HW_MASK_B plus bas) et
            // seul A ferme le menu.
            {
              int by2 = DISPLAY_HEIGHT - 20;
#if ENABLE_VIDEO_LINK
              tft.drawString(SET_X0 + 10, by2, "A: Fermer  B: Norme", 0xFFFF, 0x0000, 1);
#else
              tft.drawString(SET_X0 + 10, by2, "A: Fermer  B: Annuler", 0xFFFF, 0x0000, 1);
#endif
            }
            settings_full_redraw = false;
            last_menu_volume = -1;
            last_menu_tv_mode = -1;
            last_menu_pal_mode = -1;
            last_menu_wifi_status = -1;
          }

          // Ecran en "lignes" : libelle a gauche, valeur ou interrupteur cale a droite.
          //
          // Volume : libelle "Volume" a gauche, pourcentage cale a droite, barre fine
          // (10 px) sur toute la largeur juste en dessous. Affiche master_volume, la
          // variable utilisee aussi pour le son (vid_flush(), plus bas).
          if (last_menu_volume != master_volume) {
            tft.drawFilledRect(SET_X0 + 20, 30, SET_W - 40, 44, 0x0000);
            tft.drawString(SET_X0 + 20, 30, "AUDIO", COLOR_NEUTRAL_GREY, 0x0000, 1);

            tft.drawString(SET_X0 + 20, 46, "Volume", 0xFFFF, 0x0000, 1);
            char vol_str[8];
            snprintf(vol_str, sizeof(vol_str), "%d%%", master_volume);
            draw_right_text(SET_X0 + SET_W - 20, 46, vol_str, 0x07E0, 0x0000, 1);

            int bar_width = (master_volume * (SET_W - 40)) / 200;
            tft.drawFilledRect(SET_X0 + 20, 62, SET_W - 40, 10, 0x4208);
            tft.drawFilledRect(SET_X0 + 20, 62, bar_width, 10, 0xF800);

            last_menu_volume = master_volume;
          }

#if ENABLE_WIFI_PORTAL && !ENABLE_VIDEO_LINK
          // Infos Wi-Fi (statut, IP) et case a cocher "WiFi active", reglable au L/R
          // (libres ici : utilises seulement avec ENABLE_VIDEO_LINK) - voir le
          // traitement de HW_MASK_LEFT/RIGHT plus bas.
          if (last_menu_wifi_status != (int)g_wifi_boot_status) {
            tft.drawFilledRect(SET_X0 + 20, 92, SET_W - 40, 70, 0x0000);
            tft.drawString(SET_X0 + 20, 92, "WI-FI", COLOR_NEUTRAL_GREY, 0x0000, 1);

            bool wifi_is_on = (g_wifi_boot_status == WIFI_BOOT_STATION_OK ||
                               g_wifi_boot_status == WIFI_BOOT_AP_FALLBACK ||
                               g_wifi_boot_status == WIFI_BOOT_CONNECTING);
            tft.drawString(SET_X0 + 20, 108, "WiFi active", 0xFFFF, 0x0000, 1);
            draw_toggle_switch(SET_X0 + SET_W - 40, 106, wifi_is_on);

            tft.drawString(SET_X0 + 20, 134, "Statut", 0xFFFF, 0x0000, 1);
            if (g_wifi_boot_status == WIFI_BOOT_STATION_OK) {
              draw_status_pill(SET_X0 + SET_W - 20, 130, "connecte", 0x07E0);
              tft.drawString(SET_X0 + 20, 156, "IP", 0xFFFF, 0x0000, 1);
              draw_right_text(SET_X0 + SET_W - 20, 156, g_wifi_boot_status_text, 0x07E0, 0x0000, 1);
            } else if (g_wifi_boot_status == WIFI_BOOT_AP_FALLBACK) {
              draw_status_pill(SET_X0 + SET_W - 20, 130, "point d'acces", 0x07FF);
              tft.drawString(SET_X0 + 20, 156, "SSID", 0xFFFF, 0x0000, 1);
              draw_right_text(SET_X0 + SET_W - 20, 156, g_wifi_boot_status_text, 0x07FF, 0x0000, 1);
            } else if (g_wifi_boot_status == WIFI_BOOT_CONNECTING) {
              draw_status_pill(SET_X0 + SET_W - 20, 130, "connexion...", 0xFFE0);
            } else if (g_wifi_boot_status == WIFI_BOOT_DISABLED) {
              draw_status_pill(SET_X0 + SET_W - 20, 130, "desactive", COLOR_NEUTRAL_GREY);
            } else {
              draw_status_pill(SET_X0 + SET_W - 20, 130, "indisponible", 0xF800);
            }
            last_menu_wifi_status = (int)g_wifi_boot_status;
          }
#endif

#if ENABLE_VIDEO_LINK
          // Ligne "Sortie" (ecran LCD / television), reglable au LEFT/RIGHT -
          // voir g_display_tv_mode (hw_config.h) et vid_flush() (plus haut
          // dans ce fichier) pour son effet reel. Place sous la barre de
          // volume, meme mise en page.
          if (last_menu_tv_mode != (int)g_display_tv_mode) {
            tft.drawFilledRect(SET_X0 + 20, 165, SET_W - 40, 20, 0x0000);
            tft.drawString(SET_X0 + 20, 165,
                            g_display_tv_mode ? "Sortie: TELEVISEUR" : "Sortie: ECRAN LCD",
                            g_display_tv_mode ? 0x07FF : 0xFFE0, 0x0000, 1);
            // Interrupteur visuel a droite de la ligne, en plus du texte.
            draw_toggle_switch(SET_X0 + SET_W - 40, 163, g_display_tv_mode);
            last_menu_tv_mode = (int)g_display_tv_mode;
          }

          // Ligne "Norme" (NTSC/PAL), reglable au B - voir g_video_pal_mode
          // (hw_config.h) et video_link.h pour le detail (pourquoi pas de
          // SECAM). Sous la ligne "Sortie", meme mise en page.
          if (last_menu_pal_mode != (int)g_video_pal_mode) {
            tft.drawFilledRect(SET_X0 + 20, 190, SET_W - 40, 20, 0x0000);
            tft.drawString(SET_X0 + 20, 190,
                            g_video_pal_mode ? "Norme: PAL" : "Norme: NTSC",
                            g_video_pal_mode ? 0x07FF : 0xFFE0, 0x0000, 1);
            // Meme interrupteur visuel que "Sortie" ci-dessus.
            draw_toggle_switch(SET_X0 + SET_W - 40, 188, g_video_pal_mode);
            last_menu_pal_mode = (int)g_video_pal_mode;
          }
#endif

          int hw2 = nes_get_gamepad_state();

          if ((millis() - settings_input) > 150) {
            // Ajuste directement master_volume ("menu_volume" retiree, voir
            // plus haut) - persiste en NVS a la fermeture de Reglages
            // seulement (save_volume_if_changed(), appelee juste apres la fin
            // de cette boucle while(in_settings)), pas a chaque pression
            // individuelle.
            if (hw2 & HW_MASK_UP) {
              master_volume = min(200, master_volume + 10);
              settings_input = millis();
            }
            if (hw2 & HW_MASK_DOWN) {
              master_volume = max(0, master_volume - 10);
              settings_input = millis();
            }
#if ENABLE_VIDEO_LINK
            if (hw2 & HW_MASK_LEFT || hw2 & HW_MASK_RIGHT) {
              g_display_tv_mode = !g_display_tv_mode;
              if (g_display_tv_mode && !video_link_is_ready()) {
                // Avertissement seulement (pas de blocage) : le coprocesseur ne repond pas
                // ou n'est pas cable, le mode reste choisi.
                Serial.println("[MENU] Sortie TV selectionnee mais le lien coprocesseur n'est pas pret (verifie le cablage/l'alimentation du coprocesseur).");
              }
              settings_input = millis();
            }

            // B bascule NTSC/PAL au lieu de fermer le menu (voir le texte d'aide
            // "B: Norme" plus haut).
            // video_link_set_video_standard() renvoie la valeur choisie tout
            // de suite ; un renvoi periodique complementaire est fait depuis
            // vid_flush() (voir osd.cpp) tant que le mode TV est actif, car la
            // liaison SPI est a sens unique (voir video_link.h).
            if (hw2 & HW_MASK_B) {
              g_video_pal_mode = !g_video_pal_mode;
              video_link_set_video_standard(g_video_pal_mode);
              settings_input = millis();
            }
#elif ENABLE_WIFI_PORTAL
            // L/R (libres ici, ENABLE_VIDEO_LINK desactive) bascule le Wi-Fi - voir la
            // ligne "WiFi active" du bloc WI-FI plus haut. Activer le Wi-Fi appelle
            // wifi_boot_enable(), qui refait la meme tentative bloquante box -> repli
            // point d'acces qu'au demarrage (jusqu'a WIFI_STA_TIMEOUT_MS + ~800 ms, voir
            // wifi_boot.cpp) : un badge "connexion..." est affiche AVANT l'appel pour
            // eviter un ecran fige sans indication.
            if (hw2 & HW_MASK_LEFT || hw2 & HW_MASK_RIGHT) {
              bool wifi_is_on = (g_wifi_boot_status == WIFI_BOOT_STATION_OK ||
                                 g_wifi_boot_status == WIFI_BOOT_AP_FALLBACK ||
                                 g_wifi_boot_status == WIFI_BOOT_CONNECTING);
              if (wifi_is_on) {
                wifi_boot_disable();
              } else {
                tft.drawFilledRect(SET_X0 + 20, 130, SET_W - 40, 16, 0x0000);
                draw_status_pill(SET_X0 + SET_W - 20, 130, "connexion...", 0xFFE0);
                wifi_boot_enable();
              }
              last_menu_wifi_status = -1;  // force le redessin complet du bloc WI-FI
              settings_input = millis();
            }
#endif
            // START ouvre l'ecran "Portail actif" (run_wifi_portal_screen(),
            // juste au-dessus de draw_menu() dans ce fichier) - START n'etait
            // utilise nulle part ailleurs dans cet ecran Reglages, aucun
            // conflit. Independant de ENABLE_VIDEO_LINK (contrairement a L/R
            // et B ci-dessus).
#if ENABLE_WIFI_PORTAL
            if (hw2 & HW_MASK_START) {
              run_wifi_portal_screen();
              settings_full_redraw = true;
              settings_input = millis();
            }
#endif
            // Quand ENABLE_VIDEO_LINK est actif, B est reserve au reglage de norme
            // ci-dessus et ne ferme pas le menu (seul A le fait) ; sinon A et B ferment
            // tous les deux.
#if ENABLE_VIDEO_LINK
            if (hw2 & HW_MASK_A) {
              in_settings = false;
            }
#else
            if (hw2 & HW_MASK_A || hw2 & HW_MASK_B) {
              in_settings = false;
            }
#endif
          }
#if ENABLE_WIFI_PORTAL
          wifi_portal_loop();
#endif
          bluetooth_controller_radio_check();   // Wi-Fi coupe ou rallume ici
          delay(50);
        }
        // Enregistre le volume en NVS a la fermeture de Reglages, uniquement
        // s'il a change pendant cette visite - voir save_volume_if_changed()
        // et le commentaire pres de volume_at_settings_open, plus haut.
        save_volume_if_changed(volume_at_settings_open);
        last_input = millis();
        needs_full_redraw = true;
      }

      // Voir run_video_output_screen() au-dessus de show_menu() dans ce
      // fichier.
      if (hw & HW_MASK_START) {
        run_video_output_screen();
        needs_full_redraw = true;
        last_input = millis();
      }
    }
#if ENABLE_WIFI_PORTAL
    wifi_portal_loop();
#endif
    // Bluetooth demarre, mis en pause ou repris selon le point d'acces (Wi-Fi
    // coupe ou rallume depuis Reglages) - voir bluetooth_controller.h. Ne fait
    // rien tant que l'etat ne change pas.
    bluetooth_controller_radio_check();
    delay(20);
  }
  return -1;
}

extern "C" const char *get_selected_game() {
  // ROM_PATH_MAX (80) : "/" ou "flash:/" + le nom complet (63 caracteres au
  // plus) + le caractere nul.
  static char game_path[ROM_PATH_MAX];
  if (game_list.selected < game_list.count) {
    game_full_path(game_list.selected, game_path, sizeof(game_path));
    return game_path;
  }
  return "/mario.nes";  // Default fallback
}

extern "C" char *osd_getromdata(void) {
  const char *rom_path = get_selected_game();

  Serial.printf("[ROM] osd_getromdata loading: %s\n", rom_path);
  Serial.flush();

  if (global_rom_data == NULL) {
    Serial.println("[ROM] global_rom_data is NULL - loading from SD...");
    Serial.flush();

    bool loaded = false;
    try {
      loaded = vid_preload_rom(rom_path);
    } catch (...) {
      Serial.println("[ROM] EXCEPTION during ROM preload!");
      loaded = false;
    }

    if (!loaded) {
      Serial.printf("[ROM] CRITICAL: Failed to load ROM file: %s\n", rom_path);
      return NULL;
    }
  }

  if (global_rom_data == NULL) {
    Serial.println("[ROM] FATAL: Still no ROM data after load attempt!");
    return NULL;
  }

  Serial.printf("[ROM] ROM ready: %d bytes\n", global_rom_size);
  return global_rom_data;
}

extern "C" void osd_release_romdata(void) {
  if (global_rom_data) {
    free(global_rom_data);
    global_rom_data = NULL;
    global_rom_size = 0;
  }
}

extern "C" int osd_init_sound(void) {
  if (!runtime_sound_enabled) {
    Serial.println("Audio disabled by runtime config (-sound not set)");
    return 0;
  }
#if ENABLE_SOUND
  Serial.println("Initializing I2S audio...");
  // >>> ADAPTATION (carte avec codec ES8311, pas un DAC I2S "muet" comme
  // le MAX98357A du depot d'origine) : deux champs ajoutes par rapport
  // au depot d'origine, tous les deux INDISPENSABLES pour que l'ES8311
  // recoive une horloge MCLK correcte (sa PLL interne en a besoin, un
  // DAC I2S simple type MAX98357A non) :
  //   - mclk_multiple : doit correspondre exactement a ce que
  //     audio_codec.cpp a deja configure cote codec (ES8311_MCLK_MULTIPLE,
  //     256x la frequence d'echantillonnage) - une valeur differente
  //     desynchroniserait l'horloge MCLK reellement produite de celle
  //     attendue par le codec.
  //   - le .mck_io_num ajoute ci-dessous dans i2s_pin_config_t (absent
  //     du depot d'origine, qui laissait ce champ a sa valeur par defaut
  //     zero, donc GPIO0 - sans consequence pour eux puisqu'un
  //     MAX98357A n'a pas d'entree MCLK a cabler).
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = AUDIO_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S_MSB,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 512,
    .use_apll = true,
    .tx_desc_auto_clear = true,
    .mclk_multiple = I2S_MCLK_MULTIPLE_256  // doit correspondre a audio_codec.cpp (ES8311_MCLK_MULTIPLE)
  };
  i2s_pin_config_t pin_config = {
    .mck_io_num = I2S_MCK,   // ajoute pour cette carte - voir le commentaire ci-dessus
    .bck_io_num = I2S_BCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_DO,
    .data_in_num = I2S_PIN_NO_CHANGE
  };

  esp_err_t err;
  if ((err = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL)) != ESP_OK) {
    Serial.printf("I2S driver install failed: %d\n", err);
    return -1;
  }
  if ((err = i2s_set_pin(I2S_NUM_0, &pin_config)) != ESP_OK) {
    Serial.printf("I2S set pin failed: %d\n", err);
    return -1;
  }
  i2s_set_clk(I2S_NUM_0, AUDIO_SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
  Serial.println("I2S audio initialized successfully");
  return 0;
#else
  Serial.println("Sound disabled in configuration");
  return 0;
#endif
}

extern "C" void osd_getsoundinfo(sndinfo_t *info) {
  info->sample_rate = AUDIO_SAMPLE_RATE;
  info->bps = 16;
}

extern "C" void osd_setsound(void (*playfunc)(void *buffer, int length)) {
  emulator_audio_callback = runtime_sound_enabled ? playfunc : NULL;
}

extern "C" void osd_stopsound(void) {
  if (!runtime_sound_enabled) return;
#if ENABLE_SOUND
  i2s_zero_dma_buffer(I2S_NUM_0);
#endif
}

extern "C" void osd_writesound(void *stream, int len) {}

extern "C" void osd_initvideo(int *lines) {
  *lines = NES_SCREEN_HEIGHT;
  Serial.printf("osd_initvideo called, lines=%d\n", *lines);
}

extern "C" void osd_shutdownvideo() {}

extern "C" void osd_setscreen(int x, int y, int width, int height) {}

extern "C" void osd_setpalette(rgb_t *pal) {
  if (!pal) return;
  if (!myPaletteRGB888) {
    myPaletteRGB888 = (uint8_t *)heap_caps_malloc(256 * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!myPaletteRGB888) myPaletteRGB888 = (uint8_t *)heap_caps_malloc(256 * 3, MALLOC_CAP_8BIT);
  }
  for (int i = 0; i < 256; i++) {
    uint16_t c = tft.color565(pal[i].r, pal[i].g, pal[i].b);
    myPalette565[i] = c;
    myPalette565_swapped[i] = __builtin_bswap16(c);
    // RGB332 : 3 bits rouge (bits 7-5), 3 bits vert (bits 4-2), 2 bits bleu
    // (bits 1-0) - conversion standard, memes valeurs r/g/b source que la
    // palette 565 ci-dessus (donc meme rendu de couleur, juste moins de
    // nuances - normal et attendu pour une sortie composite dont la definition
    // est de toute facon bien plus faible).
    myPaletteRGB332[i] = (uint8_t)((pal[i].r & 0xE0) | ((pal[i].g & 0xE0) >> 3) | (pal[i].b >> 6));
    if (myPaletteRGB888) {
      myPaletteRGB888[i * 3] = pal[i].r;
      myPaletteRGB888[i * 3 + 1] = pal[i].g;
      myPaletteRGB888[i * 3 + 2] = pal[i].b;
    }
  }
}

extern "C" int nes_get_gamepad_state();
#include "bluetooth_controller.h"   // Bouton MENU du telephone

// ---------------------------------------------------------------------------
// PLEIN ECRAN SUR LE LCD. Disposition de l'ecran LCD pendant une partie :
//   LCD_LAYOUT_UNKNOWN : avant la premiere image ;
//   LCD_LAYOUT_NORMAL  : image 256x240 au centre, bandes tactiles de 32 px ;
//   LCD_LAYOUT_FULL    : image 320x240 sur tout l'ecran (lcd_fullscreen.h).
// Choisie avant chaque image par lcd_layout_update() (vid_flush(), mode LCD
// seulement), selon LCD_FULLSCREEN_MODE (hw_config.h) :
//   - mode 1 : plein ecran des que le telephone est connecte. S'il se
//     deconnecte, le plein ecran est garde LCD_FULLSCREEN_EXIT_DELAY_MS
//     (coupure breve : la page du telephone se reconnecte seule), puis
//     l'affichage normal revient avec les bandes tactiles redessinees ;
//   - mode 2 : toujours ; mode 0 : jamais.
// Le plein ecran demande un tampon d'image de 320x240 en PSRAM (alloue par
// vid_flush()) ; sans lui, affichage normal.
// ---------------------------------------------------------------------------
#define LCD_LAYOUT_UNKNOWN -1
#define LCD_LAYOUT_NORMAL   0
#define LCD_LAYOUT_FULL     1
static_assert(DISPLAY_WIDTH == LCD_FS_DST_W && DISPLAY_HEIGHT == LCD_FS_DST_H &&
              NES_SCREEN_WIDTH == LCD_FS_SRC_W && NES_SCREEN_HEIGHT == LCD_FS_SRC_H,
              "lcd_fullscreen.h suppose un ecran 320x240 et une image NES 256x240");
#if LCD_FULLSCREEN_MODE < 0 || LCD_FULLSCREEN_MODE > 2
#error "LCD_FULLSCREEN_MODE (hw_config.h) : 0 (jamais), 1 (avec le telephone) ou 2 (toujours)"
#endif
// Plein ecran possible avec ces reglages ? (mode 1 sans Bluetooth : jamais,
// le grand tampon n'est alors pas alloue).
#if LCD_FULLSCREEN_MODE == 2 || (LCD_FULLSCREEN_MODE == 1 && ENABLE_BLE_GAMEPAD)
#define LCD_FULLSCREEN_POSSIBLE 1
#else
#define LCD_FULLSCREEN_POSSIBLE 0
#endif

static int8_t s_lcd_layout = LCD_LAYOUT_UNKNOWN;
static bool s_fs_buffer_ok = false;          // frame_buffer de 320x240 disponible
static uint8_t s_fs_row_map[LCD_FS_DST_H];   // ligne NES lue pour chaque ligne de l'ecran
#if LCD_FULLSCREEN_MODE == 1
static uint32_t s_fs_phone_lost_at = 0;      // perte du telephone en plein ecran (0 = aucune)
#endif

// Plein ecran voulu pour l'image a venir ? (sans tenir compte du tampon)
static bool lcd_fullscreen_wanted(void) {
#if LCD_FULLSCREEN_MODE == 2
  return true;
#elif LCD_FULLSCREEN_MODE == 1
  if (bluetooth_controller_is_connected()) {
    s_fs_phone_lost_at = 0;
    return true;
  }
  if (s_lcd_layout != LCD_LAYOUT_FULL) return false;
  uint32_t now = millis();
  if (s_fs_phone_lost_at == 0) {
    s_fs_phone_lost_at = now ? now : 1;
    Serial.println("[ECRAN] telephone deconnecte : plein ecran garde quelques secondes");
  }
  return (uint32_t)(now - s_fs_phone_lost_at) < (uint32_t)LCD_FULLSCREEN_EXIT_DELAY_MS;
#else
  return false;
#endif
}

// Bandes laterales de l'affichage normal : touches tactiles, ou deux bandes
// noires sans tactile.
static void lcd_draw_bands(void) {
#if USE_TOUCH_CONTROLS
  touch_controls_draw_gameplay_overlay();
#else
  const int band = (DISPLAY_WIDTH - NES_SCREEN_WIDTH) / 2;
  tft.drawFilledRect(0, 0, band, DISPLAY_HEIGHT, 0x0000);
  tft.drawFilledRect(DISPLAY_WIDTH - band, 0, band, DISPLAY_HEIGHT, 0x0000);
#endif
}

// Applique une disposition. Retour a l'affichage normal : les bandes sont
// redessinees, l'image du jeu ne couvrant plus que le centre. Passage en
// plein ecran : rien a dessiner, l'image suivante recouvre tout l'ecran.
static void lcd_layout_set(int8_t layout) {
  if (layout == s_lcd_layout) return;
  if (layout == LCD_LAYOUT_NORMAL) lcd_draw_bands();
  Serial.printf("[ECRAN] partie affichee %s\n",
                layout == LCD_LAYOUT_FULL ? "en plein ecran (320x240)" : "en 256x240 avec les bandes tactiles");
  s_lcd_layout = layout;
  g_lcd_fullscreen_active = (layout == LCD_LAYOUT_FULL);
}

// Appelee par vid_flush() avant la conversion de chaque image (mode LCD).
static void lcd_layout_update(void) {
  const bool full = s_fs_buffer_ok && lcd_fullscreen_wanted();
  lcd_layout_set(full ? LCD_LAYOUT_FULL : LCD_LAYOUT_NORMAL);
}

// Appelee par nes_emulator.ino juste avant la partie, a la place de
// touch_controls_draw_gameplay_overlay(). Partie attendue en plein ecran
// (telephone deja connecte, ou mode 2) : rien a dessiner, l'ecran
// "Lancement..." reste affiche pendant le chargement de la ROM et la premiere
// image couvre tout l'ecran (si l'affichage normal l'emporte finalement a la
// premiere image, lcd_layout_set() dessine alors les bandes). Sinon : bandes
// dessinees. Mode Televiseur : inchange (etiquettes tactiles laissees sur
// l'ecran LCD).
extern "C" void osd_lcd_prepare_game_screen(void) {
  if (g_display_tv_mode) {
#if USE_TOUCH_CONTROLS
    touch_controls_draw_gameplay_overlay();
#endif
    return;
  }
  bool full_expected = false;
#if LCD_FULLSCREEN_MODE == 2
  full_expected = true;
#elif LCD_FULLSCREEN_MODE == 1
  full_expected = bluetooth_controller_is_connected();
#endif
  if (full_expected) return;
  lcd_draw_bands();
  // Bandes deja en place : la premiere image en affichage normal n'a pas
  // a les redessiner.
  s_lcd_layout = LCD_LAYOUT_NORMAL;
}

// Retour au menu depuis une partie. nofrendo_main() est une boucle bloquante,
// et plusieurs elements de ce portage (pilote I2S, tampon d'image alloue une
// seule fois) supposent qu'elle ne tourne qu'UNE fois dans la vie du
// programme : le retour au menu se fait donc par un redemarrage logiciel
// complet (ESP.restart()), 2-3 secondes de coupure, sans aucun risque d'etat
// residuel.
//
// Declenchement : maintenir SELECT enfonce en continu pendant 3 secondes
// PENDANT une partie (pas dans le menu, ou SELECT ouvre deja les reglages
// de volume). Un appui bref reste un SELECT NES normal, transmis tel quel
// au jeu (necessaire, beaucoup de jeux l'utilisent) - seul un maintien
// prolonge et deliberate declenche le retour au menu, pour eviter tout
// retour accidentel en pleine partie. Une barre de progression s'affiche
// dans la bande tactile SELECT (coin bas-droit, zone deja reservee et
// jamais dessinee par le jeu - voir touch_controls.h) pendant le maintien,
// pour que l'appui ne semble pas "ne rien faire" pendant les 3 secondes -
// utile aussi bien au tactile qu'avec le 74HC165 physique, les deux
// passent par le meme masque HW_MASK_SELECT.
#define QUIT_HOLD_MS 3000
#define QUIT_ZONE_X  288
#define QUIT_ZONE_Y  180
#define QUIT_ZONE_W  32
#define QUIT_ZONE_H  60

// ============================================================================
// Partie en cours - sauvegardes de la cartouche (pile), sauvegarde instantanee
// (savestate.c), recents. Fichiers : game_library.h.
// ============================================================================
static char s_game_file[64] = "";        // fichier du jeu en cours ("Zelda.nes"), "" : aucun
static int s_game_where = ROM_WHERE_SD;
static uint8_t *s_batt_copy = NULL;      // derniere RAM de sauvegarde ecrite (comparaison)
static uint32_t s_batt_copy_size = 0;
static uint32_t s_batt_frames = 0;
static bool s_resume_next = false;       // osd_request_resume() : reprise au prochain lancement
static bool s_resume_pending = false;    // etat "reprise" a charger a la premiere image

void osd_request_resume(bool on) { s_resume_next = on; }

static void game_identity(const char *path) {
  const char *fs_path = path;
  s_game_where = rom_storage_split(path, &fs_path);
  const char *base = strrchr(fs_path, '/');
  snprintf(s_game_file, sizeof(s_game_file), "%s", base ? base + 1 : fs_path);
}

static void battery_snapshot() {
  uint32 size = 0;   // type de nofrendo (savestate.h)
  uint8_t *ram = ss_battery_ram(&size);
  if (!ram || !size) { s_batt_copy_size = 0; return; }
  if (!s_batt_copy || s_batt_copy_size < size) {
    if (s_batt_copy) free(s_batt_copy);
    s_batt_copy = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_batt_copy) s_batt_copy = (uint8_t *)malloc(size);
  }
  if (!s_batt_copy) { s_batt_copy_size = 0; return; }
  memcpy(s_batt_copy, ram, size);
  s_batt_copy_size = size;
}

// Au lancement : RAM de sauvegarde relue (jeux a pile : Zelda, Final Fantasy,
// Dragon Warrior...).
static void battery_load() {
  uint32 size = 0;   // type de nofrendo (savestate.h)
  uint8_t *ram = ss_battery_ram(&size);
  if (!ram || !size || !s_game_file[0]) { s_batt_copy_size = 0; return; }
  const long n = lib_save_read(s_game_file, s_game_where, "sav", ram, size);
  if (n > 0) Serial.printf("[SAUVEGARDE] Pile relue : %ld octets (%s)\n", n, s_game_file);
  else Serial.printf("[SAUVEGARDE] Jeu a pile, pas encore de sauvegarde (%u octets)\n", (unsigned)size);
  battery_snapshot();
  s_batt_frames = 0;
}

// Ecrit la RAM de sauvegarde si elle a change depuis la derniere ecriture.
static bool battery_flush(const char *why) {
  uint32 size = 0;   // type de nofrendo (savestate.h)
  uint8_t *ram = ss_battery_ram(&size);
  if (!ram || !size || !s_game_file[0]) return false;
  if (s_batt_copy && s_batt_copy_size == size && memcmp(ram, s_batt_copy, size) == 0) return false;
  const uint32_t t0 = millis();
  const bool ok = lib_save_write(s_game_file, s_game_where, "sav", ram, size);
  Serial.printf("[SAUVEGARDE] Pile ecrite (%s) : %s, %lu ms\n", why, ok ? "OK" : "ECHEC", (unsigned long)(millis() - t0));
  if (ok) battery_snapshot();
  return ok;
}

// Sauvegarde instantanee dans "<jeu>.<ext>" (st1..st3, sta : reprise).
static bool state_write(const char *ext, const char **err) {
  if (err) *err = "";
  if (!s_game_file[0]) return false;
  if (!ss_supported()) { if (err) *err = ss_error_text(-1); return false; }
  const uint32_t t0 = millis();
  size_t len = 0;
  uint8_t *buf = ss_save(&len);
  if (!buf) { if (err) *err = "Pas assez de m\xC3\xA9moire"; return false; }
  const bool ok = lib_save_write(s_game_file, s_game_where, ext, buf, len);
  ss_free(buf);
  Serial.printf("[SAUVEGARDE] Etat .%s : %s (%u octets, %lu ms)\n", ext, ok ? "OK" : "ECHEC", (unsigned)len,
                (unsigned long)(millis() - t0));
  if (!ok && err) *err = "\xC3\x89" "criture impossible (support plein ?)";
  return ok;
}

// 0 : charge ; sinon refus (texte dans *err).
static int state_read(const char *ext, const char **err) {
  if (err) *err = "";
  if (!ss_supported()) { if (err) *err = ss_error_text(-1); return -1; }
  const long size = lib_save_size(s_game_file, s_game_where, ext);
  if (size <= 0) { if (err) *err = "Emplacement vide"; return -2; }
  uint8_t *buf = (uint8_t *)heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) buf = (uint8_t *)malloc((size_t)size);
  if (!buf) { if (err) *err = "Pas assez de m\xC3\xA9moire"; return -3; }
  const long n = lib_save_read(s_game_file, s_game_where, ext, buf, (size_t)size);
  const int e = (n == size) ? ss_load(buf, (size_t)n) : -13;
  free(buf);
  Serial.printf("[SAUVEGARDE] Chargement de l'etat .%s : %s (%d)\n", ext, e ? "REFUSE" : "OK", e);
  if (e && err) *err = ss_error_text(e);
  if (!e) battery_snapshot();   // la RAM de sauvegarde vient de l'etat
  return e;
}

// En quittant un jeu (changement, retour au menu) : pile et reprise auto.
static void game_exit_save(const char *why) {
  if (!s_game_file[0]) return;
  battery_flush(why);
  const char *err = NULL;
  if (ss_supported()) state_write("sta", &err);
}

// Toutes les 3 s : RAM de sauvegarde ecrite si elle a change (le jeu vient
// d'enregistrer la partie) - rien a faire pour le joueur.
static void battery_poll() {
  if (++s_batt_frames < 180) return;
  s_batt_frames = 0;
  battery_flush("pendant la partie");
}


// Sequence de sortie commune a SELECT maintenu 3 s et au bouton "MENU JEUX" du
// telephone (identique a celle plus la coupure propre de la liaison Bluetooth
// avant le redemarrage).
static void return_to_menu_by_restart(const char *why) {
  Serial.printf("[MENU] %s -> retour au menu (redemarrage)...\n", why);
  // Fait en premier, avant la coupure Bluetooth ci-dessous.
  crash_guard_clear_for_clean_exit();
  tft.fillScreen(0x0000);
  tft.drawString(40, 110, "Retour au menu...", 0xFFFF, 0x0000, 1);
  game_exit_save("retour au menu");   // Pile + reprise automatique
  bluetooth_controller_disconnect_before_restart();
  delay(150);  // laisse le temps au print/a l'ecran de partir avant le reset
  ESP.restart();
}

static void update_quit_hold_feedback(bool select_held) {
  static unsigned long hold_start = 0;
  static int last_progress = -1;
  // En plein ecran, la zone SELECT fait partie de l'image du jeu. Chaque image
  // envoyee efface donc la barre : elle est redessinee a chaque appel (juste
  // apres l'envoi de l'image, voir system_video() dans nes.c), et au
  // relachement l'image suivante la fait disparaitre.
  const bool full = (s_lcd_layout == LCD_LAYOUT_FULL);

  if (!select_held) {
    if (hold_start != 0) {
      // Relache avant les 3 secondes : effacer la barre de progression.
      hold_start = 0;
      last_progress = -1;
      if (!full) {
        tft.drawFilledRect(QUIT_ZONE_X, QUIT_ZONE_Y, QUIT_ZONE_W, QUIT_ZONE_H, 0x0000);
#if USE_TOUCH_CONTROLS
        // Uniquement en mode tactile : redessiner l'etiquette SELECT normale
        // ("SE", meme convention que touch_controls_draw_gameplay_overlay()).
        // Sans tactile (74HC165 physique), cette bande est une marge jamais
        // dessinee habituellement - la laisser simplement noire plutot que
        // d'y faire apparaitre un "SE" qui n'a pas de sens dans ce mode.
        tft.drawString(QUIT_ZONE_X + 4, QUIT_ZONE_Y + (QUIT_ZONE_H / 2) - 3, "SE", 0x7BEF, 0x0000, 1);
#endif
      }
    }
    return;
  }

  if (hold_start == 0) {
    hold_start = millis();
    return;
  }

  unsigned long held = millis() - hold_start;

  if (held >= QUIT_HOLD_MS) {
    return_to_menu_by_restart("SELECT maintenu 3s");
    return;  // jamais atteint - ESP.restart() ne rend pas la main
  }

  // Barre de progression : remplit la zone SELECT du bas vers le haut au
  // fur et a mesure du maintien, redessinee seulement quand elle change
  // reellement (evite de spammer le bus SPI a chaque frame pour rien).
  int progress = (int)((held * (unsigned long)QUIT_ZONE_H) / QUIT_HOLD_MS);
  if (progress > QUIT_ZONE_H) progress = QUIT_ZONE_H;
  if (progress != last_progress || full) {
    // En plein ecran, seule la partie rouge est dessinee : l'image du jeu
    // reste visible au-dessus.
    if (!full) tft.drawFilledRect(QUIT_ZONE_X, QUIT_ZONE_Y, QUIT_ZONE_W, QUIT_ZONE_H - progress, 0x0000);
    tft.drawFilledRect(QUIT_ZONE_X, QUIT_ZONE_Y + (QUIT_ZONE_H - progress), QUIT_ZONE_W, progress, 0xF800);
    last_progress = progress;
  }
}

// ============================================================================
// Jeux choisis depuis la manette du telephone - liste, pause et changement de
// jeu SANS REDEMARRER LA CARTE (protocole : games_link.h ; changement
// lui-meme : nofrendo.c). La partie s'arrete a la fin de l'image en cours, le
// jeu suivant est charge par la boucle de nofrendo ; son, ecran, minuterie,
// Bluetooth et plein ecran restent en place. Le retour au menu de la console
// (SELECT 3 s, MENU du telephone depuis son panneau) reste un redemarrage.
// ============================================================================
// s_running_index (index de game_list) est declare plus haut, avec la page
// affichee (view_materialize() le recalcule).
static bool s_switch_pending = false;   // changement demande, fin de l'image en cours

static int game_index_of_path(const char *path) {
  char p[ROM_PATH_MAX];
  for (int i = 0; i < game_list.count; i++) {
    game_full_path(i, p, sizeof(p));
    if (strcmp(p, path) == 0) return i;
  }
  return -1;
}

// Nom du jeu pour la police 5x7 de l'ecran (ASCII seulement) : sans dossier
// ni extension, caracteres accentues changes en '?', coupe a max_chars.
static void banner_name(const char *path, char *out, size_t n, size_t max_chars) {
  const char *base = strrchr(path, '/');
  base = base ? base + 1 : path;
  size_t len = strlen(base);
  if (len > 4 && strcasecmp(base + len - 4, ".nes") == 0) len -= 4;
  size_t k = 0;
  for (size_t i = 0; i < len && k + 1 < n && k < max_chars; i++) {
    const uint8_t c = (uint8_t)base[i];
    if (c < 0x80) out[k++] = (c < 0x20) ? ' ' : (char)c;
    else if (c >= 0xC0) out[k++] = '?';   // debut d'un caractere UTF-8 : un seul '?'
  }
  out[k] = '\0';
}

// Bandeau au centre de l'ecran LCD, efface par l'image suivante du jeu (le
// bandeau tient dans l'image, en affichage normal comme en plein ecran).
static void draw_center_line(const char *t, int y, uint16_t color) {
  const int w = (int)strlen(t) * 6;
  tft.drawString((DISPLAY_WIDTH - w) / 2, y, t, color, 0x0000, 1);
}

// En mode Televiseur, le bandeau s'affiche sur la television, par- dessus
// l'image du jeu assombrie.
static void tv_notice(const char *l1, const char *l2) {
  if (!video_link_is_ready() || !game_bitmap) return;
  if (!tv_menu_render_notice(l1, l2, game_bitmap->line, myPaletteRGB332)) return;
  const uint32_t t0 = millis();
  while (!tv_menu_submit() && millis() - t0 < 300) {   // image precedente encore en route
    video_link_tick(true);
    delay(5);
  }
  video_link_flush(150);   // toute l'image part avant le chargement du jeu
}

static void draw_center_banner(const char *l1, const char *l2) {
  if (g_display_tv_mode) return;   // la partie est sur le televiseur (voir tv_notice())
  const int w = 228, h = 40;
  const int x = (DISPLAY_WIDTH - w) / 2, y = (DISPLAY_HEIGHT - h) / 2;
  tft.drawFilledRect(x, y, w, h, 0xFFE0);
  tft.drawFilledRect(x + 2, y + 2, w - 4, h - 4, 0x0000);
  draw_center_line(l1, y + 9, 0xFFE0);
  draw_center_line(l2, y + 24, 0xFFFF);
}

static int precheck_to_reason(int pc) {
  switch (pc) {
    case OSD_PRECHECK_NOFILE: return GAMES_LINK_REFUSE_NOFILE;
    case OSD_PRECHECK_MAPPER: return GAMES_LINK_REFUSE_MAPPER;
    default: return GAMES_LINK_REFUSE_NOT_NES;
  }
}

// Controle du jeu demande puis, s'il peut etre charge, demande du changement
// (la boucle de nofrendo s'arrete a la fin de l'image en cours). Faux si
// refuse : la partie en cours continue. resume : le jeu reprend sa partie
// gardee ("Reprendre" du telephone, ou du menu pause pour un jeu qui en a
// une).
static bool try_hot_switch(int idx, bool resume) {
  int mapper = -1;
  const int pc = osd_game_precheck(idx, &mapper);
  if (pc != OSD_PRECHECK_OK) {
    Serial.printf("[JEU] Changement refuse (%s) : %s\n", osd_precheck_text(pc, mapper), osd_game_filename(idx));
    games_link_report_refused(precheck_to_reason(pc), mapper);
    return false;
  }
  char path[ROM_PATH_MAX];
  game_full_path(idx, path, sizeof(path));
  game_list.selected = idx;
  s_switch_pending = true;
  joypad_p1.data = 0;
  games_link_report_loading(idx);
  Serial.printf("[JEU] Changement de jeu demande : %s\n", path);
  game_exit_save("changement de jeu");   // Pile + reprise du jeu quitte
  osd_request_resume(resume);
  nofrendo_request_switch(path);
  return true;
}

// Pause demandee par le telephone (son panneau de jeux est ouvert). La
// minuterie de l'emulateur est arretee (pas de rattrapage d'images a la
// reprise) ; le son s'arrete de lui-meme (pilote I2S en tx_desc_auto_clear).
// Sortie : REPRISE du telephone, changement de jeu accepte, START (tactile
// ou manette physique), ou telephone absent depuis 3 s - la console ne
// reste jamais bloquee en pause.
// Le temps de pause ne compte pas comme du jeu pour crash_guard : un jeu
// lance puis mis en pause tout de suite n'est confirme qu'apres quelques
// secondes de jeu reel.
static void phone_pause_loop() {
  Serial.println("[JEU] Pause : liste des jeux ouverte sur le telephone.");
  if (nes_timer_handle) esp_timer_stop(nes_timer_handle);
  joypad_p1.data = 0;
  draw_center_banner("PAUSE - START : reprise", "Choix du jeu sur le telephone");
  if (g_display_tv_mode) tv_notice("Pause", "Choix du jeu sur le t\xC3\xA9l\xC3\xA9phone");
  const unsigned long pause_t0 = millis();
  bool switching = false;
  bool start_was_down = (nes_get_gamepad_state() & HW_MASK_START) != 0;
  uint32_t lost_since = 0;
  for (;;) {
    int idx = -1;
    const int a = games_link_poll(GAMES_LINK_STATE_PAUSED, s_running_index, &idx);
    if (a == GAMES_LINK_RESUME) break;
    if (a == GAMES_LINK_LAUNCH && try_hot_switch(idx, games_link_launch_resume())) { switching = true; break; }
    if (bluetooth_controller_take_menu_request()) return_to_menu_by_restart("bouton MENU du telephone (pause)");
    if (!bluetooth_controller_is_connected()) {
      if (!lost_since) lost_since = millis() | 1;
      else if ((uint32_t)(millis() - lost_since) > 3000) {
        Serial.println("[JEU] Telephone deconnecte pendant la pause : reprise de la partie.");
        break;
      }
    } else {
      lost_since = 0;
    }
    const bool start_down = (nes_get_gamepad_state() & HW_MASK_START) != 0;
    if (start_down && !start_was_down) {
      // START relache avant la reprise : sinon il mettrait le jeu en pause.
      const uint32_t t0 = millis();
      while ((nes_get_gamepad_state() & HW_MASK_START) && millis() - t0 < 2000) {
        crash_guard_feed();
        delay(10);
      }
      break;
    }
    start_was_down = start_down;
    crash_guard_feed();   // chien de garde nourri, lancement pas confirme pour autant
    if (g_display_tv_mode && video_link_is_ready()) video_link_tick(true);
    delay(5);
  }
  crash_guard_extend_grace(millis() - pause_t0);
  if (!switching) {
    if (nes_timer_handle) esp_timer_start_periodic(nes_timer_handle, s_timer_period_us);
    Serial.println("[JEU] Reprise de la partie.");
  }
  // En cas de changement, la minuterie repart au lancement du jeu suivant
  // (osd_installtimer()).
}

static void pause_menu_loop(int open_mode);
#define PM_OPEN_MAIN 0
#define PM_OPEN_ALL  1

// Appelee a chaque image (fin de osd_getinput()).
static void phone_games_in_game() {
  if (s_switch_pending) return;   // changement deja demande : fin de l'image en cours
  int idx = -1;
  const int a = games_link_poll(GAMES_LINK_STATE_GAME, s_running_index, &idx);
  if (a == GAMES_LINK_PAUSE) phone_pause_loop();
  else if (a == GAMES_LINK_LAUNCH) try_hot_switch(idx, games_link_launch_resume());
  // MENU JEUX du telephone en sortie Televiseur : la liste s'ouvre sur la
  // television (comme SELECT + START tenus 1 s).
  else if (a == GAMES_LINK_TVLIST && osd_tv_list_available()) pause_menu_loop(PM_OPEN_ALL);
}

// ============================================================================
// MENU PAUSE pendant une partie - ecran LCD ET television
// ============================================================================
// Ouverture : SELECT + START tenus 1 s (toute manette), doigt pose 1 s sur
// l'image du jeu (ecran tactile), MENU JEUX du telephone en sortie
// Televiseur (liste des jeux directement). Le jeu est en pause, le son
// coupe. Dessin : tv_menu.cpp (polices de l'interface, accents), envoye a
// la television ou recopie sur l'ecran LCD.
//   HAUT/BAS : ligne ; GAUCHE/DROITE : valeur (emplacement, volume) ou
//   saut de 50 jeux ; A ou START : choisir ; B : retour / reprendre ;
//   SELECT (listes) : favori. Doigt : toucher une ligne la choisit.
#define TV_COMBO_MS          1000
#define TV_REPEAT_DELAY_MS   450
#define TV_REPEAT_MS         150

enum { PMI_RESUME, PMI_SAVE, PMI_LOAD, PMI_VOLUME, PMI_FAV, PMI_RECENTS, PMI_FAVS, PMI_ALL, PMI_RESET, PMI_MENU, PMI_COUNT };
enum { PL_NONE = -1, PL_RECENTS = 0, PL_FAVS, PL_ALL };

static int s_pm_list = PL_NONE;
static int s_pm_save_slot = 1;           // 1..3
static int s_pm_load_slot = 0;           // 0 : reprise automatique, 1..3
static bool s_pm_slot_used[4];
static int16_t s_pm_list_ci[LIB_MAX_FAVORITES];   // recents / favoris : numero du catalogue (-1 : absent)
static int s_pm_list_n = 0;
static bool s_pm_states_ok = false;
static bool s_pm_confirm_reset = false;

static const char *slot_ext(int slot) {
  static const char *e[4] = {"sta", "st1", "st2", "st3"};
  return e[slot & 3];
}

static void pm_refresh_slots() {
  for (int k = 0; k < 4; k++) s_pm_slot_used[k] = lib_save_find(s_game_file, s_game_where, slot_ext(k)) >= 0;
}

static void pm_build_list(int kind) {
  s_pm_list = kind;
  s_pm_list_n = 0;
  if (kind == PL_RECENTS) {
    for (int i = 0; i < lib_recent_count() && s_pm_list_n < LIB_MAX_FAVORITES; i++)
      s_pm_list_ci[s_pm_list_n++] = (int16_t)catalog_find_name(lib_recent_name(i), lib_recent_where(i));
  } else if (kind == PL_FAVS) {
    for (int i = 0; i < lib_fav_count() && s_pm_list_n < LIB_MAX_FAVORITES; i++)
      s_pm_list_ci[s_pm_list_n++] = (int16_t)catalog_find_name(lib_fav_name(i), -1);
  } else if (kind == PL_ALL) {
    s_pm_list_n = osd_catalog_count();
  }
}

static int pm_list_ci(int row) {
  if (row < 0 || row >= s_pm_list_n) return -1;
  return (s_pm_list == PL_ALL) ? row : s_pm_list_ci[row];
}

static const char *pm_list_fname(int row) {
  if (s_pm_list == PL_ALL) return osd_catalog_filename(row);
  if (s_pm_list == PL_RECENTS) return lib_recent_name(row);
  return lib_fav_name(row);
}

static const char *pm_strip(const char *f) {
  static char name[64];
  snprintf(name, sizeof(name), "%s", f);
  const size_t n = strlen(name);
  if (n > 4 && strcasecmp(name + n - 4, ".nes") == 0) name[n - 4] = '\0';
  return name;
}

static const char *pm_list_row(int row, uint8_t *flags) {
  const int ci = pm_list_ci(row);
  const char *f = pm_list_fname(row);
  *flags = 0;
  if (ci >= 0) {
    if (osd_catalog_is_bad(ci)) *flags |= TVM_BAD;
    if (osd_catalog_where(ci) == OSD_WHERE_FLASH) *flags |= TVM_FLASH;
    if (ci == s_running_cat) *flags |= TVM_CURRENT;
  } else {
    *flags |= TVM_DIM;   // jeu supprime ou deplace
  }
  if (lib_fav_is(f)) *flags |= TVM_FAV;
  return pm_strip(f);
}

static const char *pm_main_row(int row, uint8_t *flags) {
  *flags = 0;
  switch (row) {
    case PMI_RESUME: return "Reprendre la partie";
    case PMI_SAVE: if (!s_pm_states_ok) *flags |= TVM_DIM; return "Sauvegarder";
    case PMI_LOAD:
      if (!s_pm_states_ok || !s_pm_slot_used[s_pm_load_slot]) *flags |= TVM_DIM;
      return "Charger une sauvegarde";
    case PMI_VOLUME: return "Volume";
    case PMI_FAV: return lib_fav_is(s_game_file) ? "Retirer des favoris" : "Ajouter aux favoris";
    case PMI_RECENTS: return "Jeux r\xC3\xA9" "cents";
    case PMI_FAVS: return "Favoris";
    case PMI_ALL: return "Tous les jeux";
    case PMI_RESET: return s_pm_confirm_reset ? "Recommencer ? (A pour confirmer)" : "Recommencer le jeu";
    case PMI_MENU: return "Menu de la console";
  }
  return "";
}

static const char *pm_main_value(int row) {
  static char b[40];
  switch (row) {
    case PMI_SAVE:
      snprintf(b, sizeof(b), TVM_SYM_LEFT " %d " TVM_SYM_RIGHT, s_pm_save_slot);
      return b;
    case PMI_LOAD:
      if (s_pm_load_slot == 0) snprintf(b, sizeof(b), TVM_SYM_LEFT " Reprise " TVM_SYM_RIGHT);
      else snprintf(b, sizeof(b), TVM_SYM_LEFT " %d " TVM_SYM_RIGHT, s_pm_load_slot);
      return b;
    case PMI_VOLUME:
      snprintf(b, sizeof(b), TVM_SYM_LEFT " %d %% " TVM_SYM_RIGHT, master_volume);
      return b;
    case PMI_FAV: return lib_fav_is(s_game_file) ? TVM_SYM_STAR : "";
    case PMI_RECENTS: snprintf(b, sizeof(b), "%d  " TVM_SYM_RIGHT, lib_recent_count()); return b;
    case PMI_FAVS: snprintf(b, sizeof(b), "%d  " TVM_SYM_RIGHT, lib_fav_count()); return b;
    case PMI_ALL: snprintf(b, sizeof(b), "%d  " TVM_SYM_RIGHT, osd_catalog_count()); return b;
  }
  return NULL;
}

// Aide du bas de l'ecran selon la ligne
static const char *pm_hint(int cursor) {
  // 44 caracteres au plus : l'image du menu fait 256 pixels de large
  static char b[96];
  if (s_pm_list != PL_NONE) {
    if (s_pm_list == PL_ALL)
      return "A : lancer \xC2\xB7 SELECT : favori \xC2\xB7 " TVM_SYM_LEFT TVM_SYM_RIGHT " : 50 jeux";
    if (s_pm_list == PL_RECENTS)   // Un jeu recent reprend sa partie gardee
      return "A : reprendre \xC2\xB7 SELECT : favori \xC2\xB7 B : retour";
    return "A : lancer \xC2\xB7 SELECT : favori \xC2\xB7 B : retour";
  }
  switch (cursor) {
    case PMI_SAVE:
      if (!s_pm_states_ok) return "Sauvegarde impossible pour ce jeu";
      snprintf(b, sizeof(b), s_pm_slot_used[s_pm_save_slot] ? "Emplacement %d occup\xC3\xA9 : A le remplace"
                                                            : "Emplacement %d libre : A pour sauvegarder", s_pm_save_slot);
      return b;
    case PMI_LOAD:
      if (!s_pm_states_ok) return "Sauvegarde impossible pour ce jeu";
      if (s_pm_load_slot == 0)
        snprintf(b, sizeof(b), "Reprise (en quittant le jeu) : %s", s_pm_slot_used[0] ? "A pour charger" : "vide");
      else
        snprintf(b, sizeof(b), "Emplacement %d : %s", s_pm_load_slot, s_pm_slot_used[s_pm_load_slot] ? "A pour charger" : "vide");
      return b;
    case PMI_VOLUME: return TVM_SYM_LEFT TVM_SYM_RIGHT " r\xC3\xA9gler \xC2\xB7 en jeu : SELECT + haut/bas";
    case PMI_MENU: return "Partie gard\xC3\xA9" "e : \xC2\xAB Reprendre \xC2\xBB au menu";
  }
  return "A : choisir \xC2\xB7 B : reprendre la partie";
}

// ---- Affichage de l'image du menu sur l'ecran LCD ----
static uint16_t *s_rgb332_565 = NULL;   // table de couleurs, en PSRAM (memoire interne comptee)
static bool s_rgb332_lut = false;

static void lcd_show_menu_image() {
  uint8_t *const *lines = tv_menu_lines();
  if (!lines || !frame_buffer) return;
  if (!s_rgb332_565) {
    s_rgb332_565 = (uint16_t *)heap_caps_malloc(256 * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rgb332_565) s_rgb332_565 = (uint16_t *)malloc(256 * sizeof(uint16_t));
    if (!s_rgb332_565) return;
  }
  if (!s_rgb332_lut) {
    for (int c = 0; c < 256; c++) {
      const int r = ((c >> 5) & 7) * 255 / 7, g = ((c >> 2) & 7) * 255 / 7, b = (c & 3) * 255 / 3;
      const uint16_t v = tft.color565(r, g, b);
#if LCD_GAME_COLOR_FIX
      s_rgb332_565[c] = v;
#else
      s_rgb332_565[c] = __builtin_bswap16(v);
#endif
    }
    s_rgb332_lut = true;
  }
  if (s_lcd_layout == LCD_LAYOUT_FULL) {
    for (int y = 0; y < DISPLAY_HEIGHT; y++)
      lcd_fs_scale_line(lines[s_fs_row_map[y]], &frame_buffer[y * DISPLAY_WIDTH], s_rgb332_565);
    tft.pushFrame(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, frame_buffer);
  } else {
    for (int y = 0; y < NES_SCREEN_HEIGHT; y++) {
      const uint8_t *src = lines[y];
      uint16_t *dst = &frame_buffer[y * NES_SCREEN_WIDTH];
      for (int x = 0; x < NES_SCREEN_WIDTH; x++) dst[x] = s_rgb332_565[src[x]];
    }
    tft.pushFrame((DISPLAY_WIDTH - NES_SCREEN_WIDTH) / 2, 0, NES_SCREEN_WIDTH, NES_SCREEN_HEIGHT, frame_buffer);
  }
}

// Point de l'ecran LCD -> point de l'image 256 x 240 (-1 : hors de l'image)
static bool lcd_to_image(int x, int y, int *ix, int *iy) {
  if (s_lcd_layout == LCD_LAYOUT_FULL) {
    if (x < 0 || x >= DISPLAY_WIDTH || y < 0 || y >= DISPLAY_HEIGHT) return false;
    *ix = x * NES_SCREEN_WIDTH / DISPLAY_WIDTH;
    *iy = s_fs_row_map[y];
    return true;
  }
  const int x0 = (DISPLAY_WIDTH - NES_SCREEN_WIDTH) / 2;
  if (x < x0 || x >= x0 + NES_SCREEN_WIDTH || y < 0 || y >= NES_SCREEN_HEIGHT) return false;
  *ix = x - x0;
  *iy = y;
  return true;
}

// Attend que plus aucun bouton ne soit tenu (1,5 s au plus) : un bouton
// encore appuye a la reprise serait lu par le jeu.
static void wait_buttons_released() {
  const uint32_t t0 = millis();
  while (nes_get_gamepad_state() && millis() - t0 < 1500) {
    crash_guard_feed();
    if (video_link_is_ready()) video_link_tick(true);
    delay(10);
  }
}

static void pause_menu_loop(int open_mode) {
  const bool tv = g_display_tv_mode && video_link_is_ready();
  if (!tv && !frame_buffer) return;   // rien pour dessiner
  Serial.printf("[JEU] Menu pause (%s).\n", tv ? "television" : "ecran");
  if (nes_timer_handle) esp_timer_stop(nes_timer_handle);
  joypad_p1.data = 0;
#if ENABLE_SOUND
  if (runtime_sound_enabled) i2s_zero_dma_buffer(I2S_NUM_0);
#endif
  const unsigned long pause_t0 = millis();
  battery_flush("menu pause");   // moment calme pour ecrire la pile
  s_pm_states_ok = ss_supported();
  pm_refresh_slots();
  if (!s_pm_slot_used[s_pm_load_slot]) {   // emplacement a charger : le plus pertinent
    for (int k = 0; k < 4; k++) if (s_pm_slot_used[k]) { s_pm_load_slot = k; break; }
  }
  s_pm_confirm_reset = false;
  const int vol0 = master_volume;

  int cursor = 0, main_cursor = 0;
  bool from_tvlist = (open_mode == PM_OPEN_ALL);
  s_pm_list = PL_NONE;
  if (open_mode == PM_OPEN_ALL) {
    pm_build_list(PL_ALL);
    cursor = (s_running_cat >= 0 && s_running_cat < s_pm_list_n) ? s_running_cat : 0;
    main_cursor = PMI_ALL;
  }
  tv_menu_reset_scroll();

  char sub[96];
  snprintf(sub, sizeof(sub), "En pause : %s", s_game_file[0] ? pm_strip(s_game_file) : "?");
  char badge[24] = "";
  char title[40];
  char msg[96] = "";
  bool msg_err = false;
  uint32_t msg_until = 0;

  bool switching = false, dirty = true, exit_now = false;
  uint32_t last_sent = 0, repeat_at = 0;
  int prev = nes_get_gamepad_state();   // boutons de l'ouverture encore tenus : pas d'appui
  int arrows_prev = prev & (HW_MASK_UP | HW_MASK_DOWN | HW_MASK_LEFT | HW_MASK_RIGHT);
  bool phone_pause = false;
  uint32_t lost_since = 0;
  bool rendered_once = false;
  // doigt : appui puis relachement sur la meme ligne = choix
  int touch_row = -1, touch_x0 = 0, touch_y0 = 0;
  bool touch_down = false, touch_was = true;   // doigt de l'ouverture : attendre qu'il se leve

  auto set_msg = [&](const char *m, bool err) {
    snprintf(msg, sizeof(msg), "%s", m);
    msg_err = err;
    msg_until = millis() + 3000;
    dirty = true;
  };

  for (;;) {
    const uint32_t now = millis();
    const int st = nes_get_gamepad_state();
    int pressed = st & ~prev;
    prev = st;
    const int count = (s_pm_list == PL_NONE) ? PMI_COUNT : s_pm_list_n;

    // Doigt sur l'ecran LCD : choisir une ligne
    int tapped = -1;
    if (!tv) {
      int x, y, ix, iy;
      const bool down = touch_controls_point(&x, &y) && lcd_to_image(x, y, &ix, &iy);
      if (down && !touch_was) {
        touch_down = true;
        touch_x0 = ix;
        touch_y0 = iy;
        touch_row = tv_menu_row_at(ix, iy);
      } else if (down && touch_down && (abs(ix - touch_x0) > 24 || abs(iy - touch_y0) > 14)) {
        touch_row = -1;   // le doigt glisse : pas un choix
      } else if (!down && touch_down) {
        touch_down = false;
        if (touch_row >= 0) tapped = touch_row;
      }
      touch_was = down;
    }
    if (tapped >= 0) {
      if (tapped != cursor) { cursor = tapped; dirty = true; }
      pressed |= HW_MASK_A;
    }

    // Fleches (avec repetition)
    int arrows_now = st & (HW_MASK_UP | HW_MASK_DOWN | HW_MASK_LEFT | HW_MASK_RIGHT);
    if (arrows_now & (HW_MASK_UP | HW_MASK_DOWN)) arrows_now &= ~(HW_MASK_LEFT | HW_MASK_RIGHT);
    const int arrows_new = arrows_now & ~arrows_prev;
    arrows_prev = arrows_now;
    int held = 0;
    if (arrows_new) {
      held = arrows_new;
      repeat_at = now + TV_REPEAT_DELAY_MS;
    } else if (arrows_now && (int32_t)(now - repeat_at) >= 0) {
      held = arrows_now;
      repeat_at = now + TV_REPEAT_MS;
    }
    if (held && count > 0) {
      if (held & (HW_MASK_UP | HW_MASK_DOWN)) {
        int c = cursor + ((held & HW_MASK_UP) ? -1 : 1);
        if (c < 0) c = count - 1;          // le menu boucle
        if (c > count - 1) c = 0;
        if (c != cursor) { cursor = c; s_pm_confirm_reset = false; dirty = true; }
      } else {
        const int d = (held & HW_MASK_LEFT) ? -1 : 1;
        if (s_pm_list != PL_NONE) {
          int c = cursor + d * (s_pm_list == PL_ALL ? 50 : TVM_VISIBLE_ROWS);
          if (c < 0) c = 0;
          if (c > count - 1) c = count - 1;
          if (c != cursor) { cursor = c; dirty = true; }
        } else if (cursor == PMI_SAVE) {
          s_pm_save_slot = 1 + ((s_pm_save_slot - 1 + d + 3) % 3);
          dirty = true;
        } else if (cursor == PMI_LOAD) {
          s_pm_load_slot = (s_pm_load_slot + d + 4) % 4;
          dirty = true;
        } else if (cursor == PMI_VOLUME) {
          int v = master_volume + d * 5;
          if (v < 0) v = 0;
          if (v > 200) v = 200;
          master_volume = v;
          dirty = true;
        }
      }
    }

    // SELECT dans une liste : favori
    if ((pressed & HW_MASK_SELECT) && s_pm_list != PL_NONE && cursor < count) {
      const char *f = pm_list_fname(cursor);
      if (!lib_fav_is(f) && lib_fav_full()) set_msg("Favoris pleins (60 au plus)", true);
      else set_msg(lib_fav_toggle(f) ? "Ajout\xC3\xA9 aux favoris" : "Retir\xC3\xA9 des favoris", false);
      if (s_pm_list == PL_FAVS) {   // la liste des favoris change
        pm_build_list(PL_FAVS);
        if (cursor > s_pm_list_n - 1) cursor = s_pm_list_n > 0 ? s_pm_list_n - 1 : 0;
      }
    }

    // B : retour au menu principal, ou reprise
    if (pressed & HW_MASK_B) {
      if (s_pm_list != PL_NONE && !from_tvlist) {
        s_pm_list = PL_NONE;
        cursor = main_cursor;
        tv_menu_reset_scroll();
        dirty = true;
      } else if (s_pm_list != PL_NONE && from_tvlist) {
        from_tvlist = false;   // ouvert par MENU JEUX : B montre le menu
        s_pm_list = PL_NONE;
        cursor = main_cursor;
        tv_menu_reset_scroll();
        dirty = true;
      } else {
        break;
      }
    }

    // A ou START : choisir
    if ((pressed & (HW_MASK_A | HW_MASK_START)) && count > 0) {
      if (s_pm_list != PL_NONE) {
        const int ci = pm_list_ci(cursor);
        if (ci < 0) {
          set_msg("Jeu introuvable (supprim\xC3\xA9 ou d\xC3\xA9plac\xC3\xA9)", true);
        } else if (ci == s_running_cat) {
          break;   // jeu en cours : reprise
        } else {
          const int pidx = osd_games_show_catalog(ci);
          int mapper = -1;
          const int pc = pidx >= 0 ? osd_game_precheck(pidx, &mapper) : OSD_PRECHECK_NOFILE;
          if (pc != OSD_PRECHECK_OK) set_msg(osd_precheck_text(pc, mapper), true);
          else if (try_hot_switch(pidx, s_pm_list == PL_RECENTS && osd_catalog_has_resume(ci))) { switching = true; break; }
        }
      } else {
        const char *err = NULL;
        switch (cursor) {
          case PMI_RESUME: exit_now = true; break;
          case PMI_SAVE:
            if (!s_pm_states_ok) { set_msg(ss_error_text(-1), true); break; }
            if (state_write(slot_ext(s_pm_save_slot), &err)) {
              char b[64];
              snprintf(b, sizeof(b), "Sauvegard\xC3\xA9 dans l'emplacement %d", s_pm_save_slot);
              set_msg(b, false);
              pm_refresh_slots();
              s_pm_load_slot = s_pm_save_slot;
            } else {
              set_msg(err && err[0] ? err : "Sauvegarde impossible", true);
            }
            break;
          case PMI_LOAD:
            if (!s_pm_states_ok) { set_msg(ss_error_text(-1), true); break; }
            if (!s_pm_slot_used[s_pm_load_slot]) { set_msg("Emplacement vide", true); break; }
            if (state_read(slot_ext(s_pm_load_slot), &err) == 0) exit_now = true;   // la partie reprend la
            else set_msg(err && err[0] ? err : "Chargement impossible", true);
            break;
          case PMI_VOLUME: break;
          case PMI_FAV:
            if (!lib_fav_is(s_game_file) && lib_fav_full()) set_msg("Favoris pleins (60 au plus)", true);
            else set_msg(lib_fav_toggle(s_game_file) ? "Ajout\xC3\xA9 aux favoris" : "Retir\xC3\xA9 des favoris", false);
            break;
          case PMI_RECENTS:
          case PMI_FAVS:
          case PMI_ALL:
            main_cursor = cursor;
            pm_build_list(cursor == PMI_RECENTS ? PL_RECENTS : cursor == PMI_FAVS ? PL_FAVS : PL_ALL);
            cursor = 0;
            if (s_pm_list == PL_ALL && s_running_cat >= 0 && s_running_cat < s_pm_list_n) cursor = s_running_cat;
            tv_menu_reset_scroll();
            dirty = true;
            break;
          case PMI_RESET:
            if (!s_pm_confirm_reset) { s_pm_confirm_reset = true; dirty = true; break; }
            Serial.println("[JEU] Jeu recommence (RESET).");
            nes_reset(0);   // SOFT_RESET (nes.h) : bouton RESET de la console
            exit_now = true;
            break;
          case PMI_MENU:
            if (master_volume != vol0) osd_volume_commit();
            return_to_menu_by_restart("menu pause");
            break;
        }
      }
    }
    if (exit_now) break;

    // Panneau des jeux du telephone : lancement, pause, reprise
    {
      int idx = -1;
      const int a = games_link_poll(GAMES_LINK_STATE_PAUSED, s_running_index, &idx);
      if (a == GAMES_LINK_LAUNCH && try_hot_switch(idx, games_link_launch_resume())) { switching = true; break; }
      if (a == GAMES_LINK_PAUSE) phone_pause = true;
      if (a == GAMES_LINK_RESUME) { phone_pause = false; break; }
      if (a == GAMES_LINK_TVLIST) break;   // MENU JEUX de nouveau : reprise
      if (a == GAMES_LINK_LAUNCH) dirty = true;
    }
    if (bluetooth_controller_take_menu_request()) {
      if (master_volume != vol0) osd_volume_commit();
      return_to_menu_by_restart("bouton MENU du telephone (menu pause)");
    }
#if !HAS_PHYSICAL_CONTROLLER
    // Television sans manette physique : le telephone est la seule manette ;
    // parti depuis 3 s, la partie reprend.
    if (tv) {
      if (!bluetooth_controller_is_connected()) {
        if (!lost_since) lost_since = now | 1;
        else if (now - lost_since > 3000) {
          Serial.println("[JEU] Telephone deconnecte pendant le menu pause : reprise de la partie.");
          break;
        }
      } else {
        lost_since = 0;
      }
    }
#endif

    if (msg[0] && (int32_t)(now - msg_until) >= 0) {
      msg[0] = '\0';
      dirty = true;
    }
    if (dirty) {
      TvMenuView v = {};
      if (s_pm_list == PL_NONE) {
        v.title = "Pause";
        v.badge = NULL;
        v.subtitle = sub;
        v.count = PMI_COUNT;
        v.row = pm_main_row;
        v.value = pm_main_value;
      } else {
        static const char *names[3] = {"Jeux r\xC3\xA9" "cents", "Favoris", "Tous les jeux"};
        snprintf(title, sizeof(title), "%s", names[s_pm_list]);
        snprintf(badge, sizeof(badge), "%d jeu%s", s_pm_list_n, s_pm_list_n > 1 ? "x" : "");
        v.title = title;
        v.badge = badge;
        v.subtitle = sub;
        v.count = s_pm_list_n;
        v.row = pm_list_row;
      }
      v.cursor = cursor;
      v.hint = pm_hint(cursor);
      v.message = msg[0] ? msg : NULL;
      v.message_error = msg_err;
      if (tv_menu_render(v, NULL, NULL)) {
        dirty = false;
        rendered_once = true;
        if (tv) last_sent = 0;
        else lcd_show_menu_image();
      } else if (!rendered_once) {
        Serial.println("[JEU] Menu pause impossible a afficher (memoire) : reprise de la partie.");
        break;
      }
    }
    if (tv && (last_sent == 0 || now - last_sent >= 150)) {
      if (tv_menu_submit()) last_sent = now | 1;
    }
    if (tv) video_link_tick(true);
    crash_guard_feed();
    delay(5);
  }
  if (master_volume != vol0) osd_volume_commit();
  s_pm_list = PL_NONE;
  if (!switching) {
    wait_buttons_released();
    crash_guard_extend_grace(millis() - pause_t0);
    if (!tv && s_lcd_layout != LCD_LAYOUT_FULL) lcd_draw_bands();   // bandes tactiles intactes, par prudence
    if (phone_pause && bluetooth_controller_is_connected()) {
      if (nes_timer_handle) esp_timer_start_periodic(nes_timer_handle, s_timer_period_us);
      phone_pause_loop();
      return;
    }
    if (nes_timer_handle) esp_timer_start_periodic(nes_timer_handle, s_timer_period_us);
    Serial.println("[JEU] Reprise de la partie (menu pause ferme).");
  } else {
    crash_guard_extend_grace(millis() - pause_t0);
  }
}

// Bits SELECT/START a retirer de l'entree du jeu (TV_HOLD_* ci-dessous) ;
// *paused : la liste vient d'etre ouverte puis refermee (entree a remettre a
// zero pour cette image).
//
// SELECT + START tenus 1 s ouvrent la liste. Tant que les deux sont tenus,
// le jeu ne recoit ni l'un ni l'autre. Sur le telephone, ce sont deux
// appuis separes : un SELECT ou un START tout neuf est retenu 80 ms, le
// temps de voir si l'autre suit ; s'il est relache avant, le jeu le recoit
// quand meme (60 ms), un peu en retard.
#define TV_HOLD_MS   80
#define TV_PULSE_MS  60
enum { TB_IDLE, TB_WAIT, TB_GIVEN, TB_EATEN };
struct TvBtn {
  uint8_t st;       // TB_* : relache, retenu (pas encore transmis), transmis, mange par la combinaison
  uint32_t t;       // debut de l'appui
  uint32_t pulse;   // appui court a rendre au jeu jusqu'a (0 : aucun)
};

static void tv_btn_step(TvBtn &b, bool down, uint32_t now) {
  if (!down) {
    if (b.st == TB_WAIT) b.pulse = (now + TV_PULSE_MS) | 1;   // appui court jamais transmis
    b.st = TB_IDLE;
    return;
  }
  if (b.st == TB_IDLE) {
    b.st = TB_WAIT;
    b.t = now;
  } else if (b.st == TB_WAIT && now - b.t >= TV_HOLD_MS) {
    b.st = TB_GIVEN;
  }
}

static bool tv_btn_pulse(TvBtn &b, uint32_t now) {
  if (b.pulse && (int32_t)(now - b.pulse) < 0) return true;
  b.pulse = 0;
  return false;
}

static int tv_combo_poll(int hw, bool *paused, int *pulse) {
  static uint32_t since = 0;   // debut de la combinaison
  static TvBtn bs = {TB_IDLE, 0, 0}, bt = {TB_IDLE, 0, 0};
  *paused = false;
  *pulse = 0;
  // Ecran LCD aussi (menu pause), plus seulement la television
  if (s_switch_pending || (g_display_tv_mode ? !video_link_is_ready() : !frame_buffer)) {
    since = 0;
    bs = {TB_IDLE, 0, 0};
    bt = {TB_IDLE, 0, 0};
    return 0;
  }
  const uint32_t now = millis();
  const bool sel = (hw & HW_MASK_SELECT) != 0, start = (hw & HW_MASK_START) != 0;
  tv_btn_step(bs, sel, now);
  tv_btn_step(bt, start, now);

  if (sel && start) {
    // La combinaison mange les deux boutons jusqu'a leur relachement.
    bs.st = bt.st = TB_EATEN;
    bs.pulse = bt.pulse = 0;
    if (!since) {
      since = now | 1;
    } else if (now - since >= TV_COMBO_MS) {
      since = 0;
      pause_menu_loop(PM_OPEN_MAIN);
      *paused = true;
    }
    return HW_MASK_SELECT | HW_MASK_START;
  }
  since = 0;
  int mask = 0;
  if (bs.st == TB_WAIT || bs.st == TB_EATEN) mask |= HW_MASK_SELECT;
  if (bt.st == TB_WAIT || bt.st == TB_EATEN) mask |= HW_MASK_START;
  if (tv_btn_pulse(bs, now)) *pulse |= HW_MASK_SELECT;
  if (tv_btn_pulse(bt, now)) *pulse |= HW_MASK_START;
  return mask;
}

// ---- Rappels de nofrendo.c (boucle des parties) ----
extern "C" void osd_game_will_load(const char *filename, int is_switch) {
  if (!is_switch) return;   // premier jeu : setup() a deja tout prepare
  Serial.printf("[JEU] Chargement de %s (sans redemarrer la carte)...\n", filename);
  wifi_boot_log_memory("avant changement de jeu");
#if ENABLE_SOUND
  if (runtime_sound_enabled) i2s_zero_dma_buffer(I2S_NUM_0);
#endif
  char name[40];
  banner_name(filename, name, sizeof(name), 34);
  draw_center_banner("Chargement...", name);
  if (g_display_tv_mode) {   // Nom complet (accents) sur la television
    char full[ROM_PATH_MAX];
    const char *base = strrchr(filename, '/');
    snprintf(full, sizeof(full), "%s", base ? base + 1 : filename);
    const size_t fl = strlen(full);
    if (fl > 4 && strcasecmp(full + fl - 4, ".nes") == 0) full[fl - 4] = '\0';
    tv_notice("Chargement\xE2\x80\xA6", full);
  }
  crash_guard_begin_launch_hot(filename);
}

extern "C" void osd_game_started(const char *filename) {
  const bool was_switch = s_switch_pending;
  s_switch_pending = false;
  s_running_index = game_index_of_path(filename);
  if (s_running_index >= 0) game_list.selected = s_running_index;
  // Numero du catalogue (liste complete du telephone)
  s_running_cat = (s_running_index >= 0) ? osd_games_page_to_catalog(s_running_index) : catalog_index_of_path(filename);
  joypad_p1.data = 0;
  // Lancements demandes pendant le chargement oublies ; une PAUSE (panneau
  // rouvert pendant le chargement) ou une demande de liste restent valables.
  bluetooth_controller_flush_games_launches();
  if (s_running_index >= 0 || s_running_cat >= 0) games_link_report_started(s_running_index);
  Serial.printf("[JEU] Partie lancee : %s%s\n", filename, was_switch ? " (sans redemarrage)" : "");
  // Recents, RAM de sauvegarde, reprise demandee par le menu
  game_identity(filename);
  lib_recent_push(s_game_file, s_game_where);
  battery_load();
  s_resume_pending = s_resume_next;
  s_resume_next = false;
}

extern "C" void osd_game_load_failed(const char *filename, int fallback) {
  Serial.printf("[JEU] Echec du chargement de %s%s.\n", filename,
                fallback ? " : retour au jeu precedent" : "");
  // Pas un plantage : rien a marquer ni a supprimer.
  crash_guard_clear_for_clean_exit();
  // Detail : 1 = retour au jeu precedent, 0 = la console revient a son menu.
  games_link_report_refused(GAMES_LINK_REFUSE_LOAD_FAILED, fallback ? 1 : 0);
  s_switch_pending = false;
}

// ---- volume pendant la partie (SELECT + haut/bas) ----
// SELECT tenu puis HAUT ou BAS : volume +/- 5 % (tenu : continue). Tant que
// SELECT reste tenu apres un reglage, le jeu ne recoit ni SELECT ni la
// croix haut/bas, et SELECT ne compte pas pour le retour au menu. Barre
// dessinee dans l'image du jeu (ecran et television), 1,5 s.
static bool s_vol_mode = false;
static int s_vol_prev_ud = 0;
static uint32_t s_vol_repeat_at = 0, s_vol_show_until = 0, s_vol_commit_at = 0;

// Volume regle depuis le telephone (osd_menu_api.h)
void osd_volume_remote(int percent) {
  osd_set_volume(percent);
  osd_volume_commit();
  s_vol_show_until = (millis() + 1500) | 1;   // barre de volume si une partie tourne
  Serial.printf("[VOLUME] Regle depuis le telephone : %d%%\n", master_volume);
}

static int volume_combo(int hw) {
  const uint32_t now = millis();
  const int ud = hw & (HW_MASK_UP | HW_MASK_DOWN);
  if (hw & HW_MASK_SELECT) {
    const int ud_new = ud & ~s_vol_prev_ud;
    int step = 0;
    if (ud_new) {
      step = (ud_new & HW_MASK_UP) ? 5 : -5;
      s_vol_repeat_at = now + 400;
    } else if (ud && s_vol_mode && (int32_t)(now - s_vol_repeat_at) >= 0) {
      step = (ud & HW_MASK_UP) ? 5 : -5;
      s_vol_repeat_at = now + 120;
    }
    if (step) {
      s_vol_mode = true;
      int v = master_volume + step;
      if (v < 0) v = 0;
      if (v > 200) v = 200;
      master_volume = v;
      s_vol_show_until = (now + 1500) | 1;
      s_vol_commit_at = (now + 2500) | 1;
    }
  } else {
    s_vol_mode = false;
  }
  s_vol_prev_ud = ud;
  if (s_vol_commit_at && (int32_t)(now - s_vol_commit_at) >= 0) {
    s_vol_commit_at = 0;
    osd_volume_commit();
  }
  return s_vol_mode ? (HW_MASK_SELECT | HW_MASK_UP | HW_MASK_DOWN) : 0;
}

// Chiffres 3 x 5 pour la barre de volume (bit 2 = colonne de gauche)
static const uint8_t k_digits[11][5] = {
  {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1},
  {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7},
  {5, 1, 2, 4, 5}};   // '%'

static void bmp_fill(bitmap_t *b, int x, int y, int w, int h, uint8_t c) {
  for (int j = y; j < y + h; j++) {
    if (j < 0 || j >= NES_SCREEN_HEIGHT) continue;
    for (int i = x; i < x + w; i++)
      if (i >= 0 && i < NES_SCREEN_WIDTH) b->line[j][i] = c;
  }
}

static void bmp_digit(bitmap_t *b, int x, int y, int d, uint8_t c) {
  for (int r = 0; r < 5; r++)
    for (int k = 0; k < 3; k++)
      if (k_digits[d][r] & (4 >> k)) bmp_fill(b, x + k * 2, y + r * 2, 2, 2, c);
}

// Appelee par vid_flush() avant l'envoi de l'image (ecran et television).
static void osd_overlay_draw(bitmap_t *b) {
  if (!b || !s_vol_show_until) return;
  if ((int32_t)(millis() - s_vol_show_until) >= 0) { s_vol_show_until = 0; return; }
  const uint8_t BLACK = 0x0F, WHITE = 0x30, GREEN = 0x2A, ORANGE = 0x27, GREY = 0x00;
  const int x = 56, y = 18, w = 144, h = 22;
  bmp_fill(b, x - 2, y - 2, w + 4, h + 4, WHITE);
  bmp_fill(b, x, y, w, h, BLACK);
  // haut-parleur
  bmp_fill(b, x + 6, y + 8, 4, 6, WHITE);
  bmp_fill(b, x + 10, y + 5, 2, 12, WHITE);
  bmp_fill(b, x + 12, y + 3, 2, 16, WHITE);
  // barre 0..200 % (repere a 100 %)
  const int bx = x + 20, bw = 80, by = y + 7, bh = 8;
  bmp_fill(b, bx, by, bw, bh, GREY);
  const int fill = master_volume * bw / 200;
  bmp_fill(b, bx, by, fill > bw / 2 ? bw / 2 : fill, bh, GREEN);
  if (fill > bw / 2) bmp_fill(b, bx + bw / 2, by, fill - bw / 2, bh, ORANGE);
  bmp_fill(b, bx + bw / 2, by - 2, 1, bh + 4, WHITE);
  // valeur en %
  char t[6];
  snprintf(t, sizeof(t), "%d", master_volume);
  int tx = x + 106;
  for (const char *c = t; *c; c++) { bmp_digit(b, tx, y + 6, *c - '0', WHITE); tx += 8; }
  bmp_digit(b, tx, y + 6, 10, WHITE);
}

// Bandeau bloquant (ecran ou television), ms millisecondes, jeu arrete.
static void notice_show(const char *l1, const char *l2, uint32_t ms) {
  const bool tv = g_display_tv_mode && video_link_is_ready();
  if (!tv_menu_render_notice(l1, l2, NULL, NULL)) return;
  if (nes_timer_handle) esp_timer_stop(nes_timer_handle);
  const uint32_t t0 = millis();
  if (!tv) lcd_show_menu_image();
  while (millis() - t0 < ms) {
    if (tv) { tv_menu_submit(); video_link_tick(true); }
    crash_guard_feed();
    delay(20);
  }
  crash_guard_extend_grace(millis() - t0);
  if (nes_timer_handle) esp_timer_start_periodic(nes_timer_handle, s_timer_period_us);
}

// ---- doigt pose 1 s au centre de l'image : menu pause (ecran LCD) ----
static bool touch_hold_poll() {
  static uint32_t t0 = 0;
  static int x0 = 0, y0 = 0;
  int x, y;
  if (g_display_tv_mode || !frame_buffer || !touch_controls_point(&x, &y) ||
      x < 40 || x >= DISPLAY_WIDTH - 40) {   // les bandes tactiles sont des boutons
    t0 = 0;
    return false;
  }
  const uint32_t now = millis();
  if (!t0 || abs(x - x0) > 20 || abs(y - y0) > 20) {
    t0 = now | 1;
    x0 = x;
    y0 = y;
    return false;
  }
  if (now - t0 >= 1000) {
    t0 = 0;
    return true;
  }
  return false;
}

extern "C" void osd_getinput(void) {
  // "Reprendre" du menu de la console : etat de la derniere partie
  if (s_resume_pending) {
    s_resume_pending = false;
    const char *err = NULL;
    crash_guard_note_resume();   // un plantage efface la reprise, pas le jeu
    if (state_read("sta", &err) != 0) {
      Serial.printf("[JEU] Reprise impossible : %s\n", err ? err : "?");
      notice_show("Reprise impossible", err && err[0] ? err : "Le jeu commence au d\xC3\xA9" "but", 2000);
    }
  }
  battery_poll();

  int hw = nes_get_gamepad_state();
  int nes_data = 0;

  if (hw & HW_MASK_A) nes_data |= INP_PAD_A;
  if (hw & HW_MASK_B) nes_data |= INP_PAD_B;
  if (hw & HW_MASK_SELECT) nes_data |= INP_PAD_SELECT;
  if (hw & HW_MASK_START) nes_data |= INP_PAD_START;
  if (hw & HW_MASK_UP) nes_data |= INP_PAD_UP;
  if (hw & HW_MASK_DOWN) nes_data |= INP_PAD_DOWN;
  if (hw & HW_MASK_LEFT) nes_data |= INP_PAD_LEFT;
  if (hw & HW_MASK_RIGHT) nes_data |= INP_PAD_RIGHT;

  // SELECT + START tenus = liste des jeux sur la television (mode Televiseur,
  // voir tv_combo_poll()) : le jeu ne les recoit pas, et SELECT ne compte pas
  // pour le retour au menu.
  bool tv_paused = false;
  int tv_pulse = 0;
  const int tv_mask = tv_combo_poll(hw, &tv_paused, &tv_pulse);
  if (tv_mask & HW_MASK_SELECT) nes_data &= ~INP_PAD_SELECT;
  if (tv_mask & HW_MASK_START) nes_data &= ~INP_PAD_START;
  if (tv_pulse & HW_MASK_SELECT) nes_data |= INP_PAD_SELECT;
  if (tv_pulse & HW_MASK_START) nes_data |= INP_PAD_START;
  if (tv_paused) nes_data = 0;   // boutons lus avant la liste : rien pour le jeu
  // SELECT + haut/bas = volume
  const int vol_mask = tv_paused ? 0 : volume_combo(hw);
  if (vol_mask) nes_data &= ~(INP_PAD_SELECT | INP_PAD_UP | INP_PAD_DOWN);
  // Doigt pose 1 s sur l'image : menu pause
  if (!tv_paused && touch_hold_poll()) {
    pause_menu_loop(PM_OPEN_MAIN);
    nes_data = 0;
    tv_paused = true;
  }
  joypad_p1.data = nes_data;

  // SELECT retenu ou mange par la combinaison : ne compte pas pour le
  // retour au menu (SELECT maintenu 3 s).
  update_quit_hold_feedback((hw & HW_MASK_SELECT) != 0 && !(tv_mask & HW_MASK_SELECT) && !tv_paused && !vol_mask);

  // Bouton "MENU JEUX" du telephone = meme sortie que SELECT maintenu 3 s,
  // sans attente (la page demande deja un second appui de confirmation).
  if (bluetooth_controller_take_menu_request()) {
    return_to_menu_by_restart("bouton MENU du telephone");
  }

  // Liste des jeux, pause et changement de jeu depuis le telephone (voir plus
  // haut).
  phone_games_in_game();
}

extern "C" int osd_init() {
  Serial.println("[OSD] Initializing...");
  Serial.printf("[OSD] Heap=%u, SPIRAM=%u\n", esp_get_free_heap_size(), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  Serial.println("[OSD] Initializing joypad...");
  joypad_p1.type = INP_JOYPAD0;
  joypad_p1.data = 0;

  input_register(&joypad_p1);
  Serial.println("[OSD] Input registered successfully");

  if (osd_init_sound() != 0) {
    Serial.println("[OSD] Sound Init FAILED");
  }

  Serial.printf("[OSD] Post-init Heap=%u, SPIRAM=%u\n", esp_get_free_heap_size(), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  Serial.println("[OSD] Initialization complete");
  return 0;
}

extern "C" void osd_shutdown() {
  if (nes_timer_handle) {
    esp_timer_stop(nes_timer_handle);
    esp_timer_delete(nes_timer_handle);
    nes_timer_handle = NULL;
  }
}

// Appelee a CHAQUE lancement de jeu (internal_insert(), nofrendo.c) - donc
// plusieurs fois depuis le changement de jeu sans redemarrage. La minuterie
// n'est creee qu'une fois, puis seulement relancee : avant, chaque appel en
// creait une de plus, et le 2e jeu aurait recu deux fois trop de tops (jeu
// jusqu'a deux fois trop rapide).
extern "C" int osd_installtimer(int freq, void *func, int func_param, void *func2, int func2_param) {
  emu_timer_callback = (void (*)(void))func;
  s_timer_period_us = 1000000 / freq;
  if (!nes_timer_handle) {
    const esp_timer_create_args_t timer_args = { .callback = &timer_callback_handler, .name = "nes_timer" };
    if (esp_timer_create(&timer_args, &nes_timer_handle) != ESP_OK) {
      nes_timer_handle = NULL;
      Serial.println("[OSD] ERREUR : minuterie de l'emulateur impossible a creer.");
      return -1;
    }
  } else {
    esp_timer_stop(nes_timer_handle);   // deja creee : arretee puis relancee
  }
  esp_timer_start_periodic(nes_timer_handle, s_timer_period_us);
  return 0;
}

extern "C" int osd_gettime(void) {
  return millis();
}

extern "C" int osd_makesnapname(char *buf, int len) {
  return 0;
}

extern "C" void osd_getmouse(int *x, int *y, int *button) {
  *x = 0;
  *y = 0;
  *button = 0;
}

char *global_rom_data = NULL;
int global_rom_size = 0;
static File rom_stream_file;

extern "C" bool vid_preload_rom(const char *path) {
  if (global_rom_data) {
    free(global_rom_data);
    global_rom_data = NULL;
  }

  Serial.printf("[ROM] Opening file: %s\n", path);
  Serial.flush();
  delay(100);

  File file;
  try {
    file = rom_storage_open_read(path);   // Carte SD ou memoire interne
  } catch (...) {
    Serial.println("[ROM] EXCEPTION during open()!");
    return false;
  }

  if (!file) {
    Serial.println("[ROM] Failed to open file");
    return false;
  }

  global_rom_size = file.size();
  Serial.printf("[ROM] File size: %d bytes\n", global_rom_size);

  Serial.println("[ROM] Allocating memory...");
  global_rom_data = (char *)heap_caps_malloc(global_rom_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!global_rom_data) {
    Serial.println("[ROM] SPIRAM allocation failed, trying regular heap...");
    global_rom_data = (char *)malloc(global_rom_size);
  }

  if (!global_rom_data) {
    Serial.println("[ROM] Memory allocation failed!");
    file.close();
    return false;
  }

  Serial.println("[ROM] Reading file into memory...");
  size_t bytes_read = file.readBytes(global_rom_data, global_rom_size);
  file.close();

  Serial.printf("[ROM] Read %d bytes\n", bytes_read);
  if (bytes_read != global_rom_size) {
    Serial.printf("[ROM] Error: Expected %d bytes, got %d\n", global_rom_size, bytes_read);
    free(global_rom_data);
    global_rom_data = NULL;
    return false;
  }

  Serial.println("[ROM] ROM loaded successfully!");
  return true;
}

extern "C" void IRAM_ATTR osd_blit(bitmap_t *bmp) {
  if (!video_ready || !bmp || !bmp->line || !bmp->line[0] || !frame_buffer) {
    return;
  }

  // Plein ecran, image 320x240 (voir lcd_fullscreen.h).
  if (s_lcd_layout == LCD_LAYOUT_FULL) {
    for (int y = 0; y < DISPLAY_HEIGHT; y++) {
      lcd_fs_scale_line(bmp->line[s_fs_row_map[y]], &frame_buffer[y * DISPLAY_WIDTH], LCD_GAME_PALETTE);
    }
    return;
  }

  for (int y = 0; y < NES_SCREEN_HEIGHT; y++) {
    uint8_t *src = bmp->line[y];
    uint16_t *dst = &frame_buffer[y * NES_SCREEN_WIDTH];

    for (int x = 0; x < NES_SCREEN_WIDTH; x++) {
      dst[x] = LCD_GAME_PALETTE[src[x]];
    }
  }
}

extern "C" void vid_flush() {
  static unsigned long last_display_update = 0;
  static bool tried_frame_buffer_alloc = false;
  static uint16_t line_buffer[NES_SCREEN_WIDTH];

  frame_start_time = micros();
  osd_overlay_draw(game_bitmap);   // Barre de volume

  if (!tried_frame_buffer_alloc && !frame_buffer) {
    tried_frame_buffer_alloc = true;
#if LCD_FULLSCREEN_POSSIBLE
    // Tampon de la taille de l'ecran (320x240, 150 Ko en PSRAM) pour pouvoir
    // passer en plein ecran ; en cas d'echec, tampon 256x240 habituel et
    // affichage normal seulement. Pas en mode Televiseur (l'ecran LCD
    // n'affiche pas la partie, tampon inutilise).
    if (!g_display_tv_mode) {
      frame_buffer = (uint16_t *)heap_caps_malloc(DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (frame_buffer) {
        s_fs_buffer_ok = true;
        lcd_fs_build_row_map(s_fs_row_map, LCD_FULLSCREEN_CROP_LINES);
      } else {
        Serial.println("[ECRAN] pas de tampon 320x240 en PSRAM : plein ecran impossible");
      }
    }
#endif
    if (!frame_buffer) {
      frame_buffer = (uint16_t *)heap_caps_malloc(NES_SCREEN_WIDTH * NES_SCREEN_HEIGHT * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!frame_buffer) {
      frame_buffer = (uint16_t *)malloc(NES_SCREEN_WIDTH * NES_SCREEN_HEIGHT * sizeof(uint16_t));
    }
    if (!frame_buffer) {
      low_mem_video_mode = true;
      Serial.println("Low-memory video mode enabled (line-by-line rendering)");
    }
  }

  // osd_blit() (conversion vers frame_buffer RGB565, pour l'ecran LCD
  // UNIQUEMENT) est sautee en mode de sortie "Television" (g_display_tv_mode)
  // - inutile de la calculer puisque rien ne la consommera plus bas (le bloc
  // de push LCD est lui aussi saute, voir plus bas). Le bloc coprocesseur
  // ci-dessous utilise directement game_bitmap, pas frame_buffer - aucun
  // impact sur la sortie TV.
  // Choix de la disposition (plein ecran ou normale) juste avant la
  // conversion, qui en depend.
  if (!g_display_tv_mode) {
    lcd_layout_update();
  }
  if (frame_buffer && !g_display_tv_mode) {
    osd_blit(game_bitmap);
  }

  unsigned long now = micros();
  // Tout ce bloc (le push vers l'ecran LCD proprement dit - ~33,6 ms mesures
  // le plus gros poste de temps de tout ce pipeline d'affichage) est saute en
  // mode de sortie "Television". C'est LE changement qui apporte le vrai gain
  // de fluidite cote TV : l'image du jeu ne coute alors plus que le push
  // RGB332 vers le coprocesseur ci-dessus, deux fois plus leger en octets que
  // ce push LCD en RGB565.
  if (!g_display_tv_mode && now - last_display_update > 16666) {
    int x_offset = (DISPLAY_WIDTH - NES_SCREEN_WIDTH) / 2;
    unsigned long t1 = micros();

    // tft.pushFrame() (tft_driver.h) au lieu de tft.pushImage(..., true) :
    // meme ordre des octets, mais l'image part d'un seul bloc depuis
    // frame_buffer, sans la recopie ligne par ligne (bus SPI a l'arret pendant
    // chaque copie) de la version de TFT_eSPI prevue pour les images en
    // memoire flash.
    if (frame_buffer && s_lcd_layout == LCD_LAYOUT_FULL) {
      // Plein ecran, tout l'ecran d'un coup.
      tft.pushFrame(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, frame_buffer);
    } else if (frame_buffer) {
      tft.pushFrame(x_offset, 0, NES_SCREEN_WIDTH, NES_SCREEN_HEIGHT, frame_buffer);
    } else if (low_mem_video_mode && game_bitmap && game_bitmap->line && game_bitmap->line[0]) {
      static bool lowmem_logged = false;
      if (!lowmem_logged) {
        Serial.println("Display mode: low-memory line render (FPS will be lower)");
        lowmem_logged = true;
      }
      for (int y = 0; y < NES_SCREEN_HEIGHT; y++) {
        uint8_t *src = game_bitmap->line[y];
        for (int x = 0; x < NES_SCREEN_WIDTH; x++) {
          line_buffer[x] = LCD_GAME_PALETTE[src[x]];
        }
        tft.pushImage(x_offset, y, NES_SCREEN_WIDTH, 1, line_buffer, true);
      }
    }

    unsigned long push_t = micros() - t1;
    last_display_update = now;

    static unsigned long last_print = 0;
    if (millis() - last_print > 3000) {
      // "plein ecran" ajoute pour comparer les deux affichages.
      Serial.printf("push=%luus emul_fps=%d%s\n", push_t, current_fps,
                    s_lcd_layout == LCD_LAYOUT_FULL ? " (plein ecran)" : "");
      last_print = millis();
    }
  }

  if (runtime_sound_enabled && emulator_audio_callback) {
    // Nombre d'echantillons calcule a partir du temps REEL ecoule depuis l'appel
    // precedent (audio_elapsed_us), pas d'un 1/60e de seconde fixe : vid_flush()
    // n'est pas appelee 60 fois par seconde (l'emulation tourne souvent plus
    // lentement), et un 1/60e fixe viderait le tampon I2S entre deux appels (son
    // hache). Borne a la taille des tampons (1024 echantillons, 64 ms a
    // 16000 Hz) si un appel tarde anormalement (chargement, menus...).
    static unsigned long last_audio_time = 0;
    unsigned long audio_now = micros();

    int samples_needed;
    if (last_audio_time == 0) {
      // Tout premier appel : pas de duree mesurable, 1/60e de seconde.
      samples_needed = AUDIO_SAMPLE_RATE / 60;
    } else {
      unsigned long audio_elapsed_us = audio_now - last_audio_time;
      samples_needed = (int)((audio_elapsed_us * (unsigned long)AUDIO_SAMPLE_RATE) / 1000000UL);
    }
    last_audio_time = audio_now;

    if (samples_needed < 1) samples_needed = 1;
    // Borne a AUDIO_MAX_SAMPLES_PER_FLUSH (128 ms) pour un appel tres tardif.
    if (samples_needed > AUDIO_MAX_SAMPLES_PER_FLUSH) samples_needed = AUDIO_MAX_SAMPLES_PER_FLUSH;

    while (samples_needed > 0) {   // (corps non re-indente : code d'origine)
    const int chunk = samples_needed > AUDIO_CHUNK_SAMPLES ? AUDIO_CHUNK_SAMPLES : samples_needed;
    samples_needed -= chunk;

    memset(mono_buffer, 0, chunk * 2);
    emulator_audio_callback(mono_buffer, chunk);

#if ENABLE_SOUND
    int sample_count = chunk;
    for (int i = 0; i < sample_count; i++) {
      int32_t sample = mono_buffer[i];

      sample = (sample * master_volume) / 100;
      sample = (sample * AUDIO_GAIN) / 100;

      if (sample > 32767) sample = 32767;
      if (sample < -32768) sample = -32768;

      stereo_buffer[i * 2] = (int16_t)sample;
      stereo_buffer[i * 2 + 1] = (int16_t)sample;
      audio_link_buffer[i] = (int16_t)sample;
    }

    // Transmet ces memes echantillons (deja mis a l'echelle du volume/gain) au
    // coprocesseur video, sur le meme lien SPI que l'image - pour que le son
    // sorte aussi par la television, en plus du haut-parleur local ci-dessous
    // (les deux sorties restent actives simultanement, ce bloc ne remplace
    // rien). Sans effet si ENABLE_VIDEO_LINK vaut 0 ou si le lien n'a pas pu
    // s'initialiser (voir video_link.h/.cpp).
    // N'envoie plus l'audio au coprocesseur QUE si le mode de sortie
    // "Television" est actif (g_display_tv_mode) - meme logique que pour
    // l'image juste au-dessus : inutile de streamer du son vers un televiseur
    // qu'on n'utilise pas quand on joue sur l'ecran LCD.
    if (g_display_tv_mode) {
      video_link_queue_audio(audio_link_buffer, sample_count);
    }

    // i2s_write() avec un delai d'attente, et la valeur "written" verifiee : le
    // reste est reecrit au lieu d'etre perdu. Marge de temps large ici : le rendu
    // ecran (~33 ms) domine le temps de boucle, quelques millisecondes de plus
    // sont sans consequence.
    size_t total_bytes = (size_t)sample_count * 4;
    size_t total_written = 0;
    int stalled_attempts = 0;
    while (total_written < total_bytes) {
      size_t written = 0;
      i2s_write(I2S_NUM_0,
                (const char *)stereo_buffer + total_written,
                total_bytes - total_written,
                &written,
                pdMS_TO_TICKS(5));
      if (written == 0) {
        stalled_attempts++;
        if (stalled_attempts > 4) break;  // securite anti-blocage si le DMA reste sature anormalement longtemps
        continue;
      }
      total_written += written;
      stalled_attempts = 0;
    }

    static uint32_t audio_underrun_events = 0;
    static unsigned long last_underrun_print = 0;
    if (total_written < total_bytes) {
      audio_underrun_events++;
      if (millis() - last_underrun_print > 3000) {
        Serial.printf("[AUDIO] echantillons perdus (tampon DMA sature) - %lu evenement(s) au total\n",
                      (unsigned long)audio_underrun_events);
        last_underrun_print = millis();
      }
    }
#endif
    }  // Fin du morceau (boucle while ci-dessus)
  }

  // Image et son vers le coprocesseur video (sortie Television), APRES le bloc
  // son ci-dessus : le son n'est jamais retarde par le travail sur l'image.
  // Sans effet (fonctions vides) tant que ENABLE_VIDEO_LINK vaut 0 dans
  // video_link.h - voir ce fichier pour le principe et le brochage. N'envoie
  // rien si le mode de sortie "Television" n'est pas actif (g_display_tv_mode).
  // video_link_submit_frame() convertit elle-meme l'image en RGB332 (palette
  // myPaletteRGB332, osd_setpalette()), seulement quand une nouvelle image peut
  // vraiment partir : la liaison saute d'elle-meme des images quand le bus est
  // occupe. video_link_tick() renvoie la norme PAL/NTSC chaque seconde et
  // entretient l'envoi. Voir video_link.h.
  if (g_display_tv_mode && video_link_is_ready()) {
    if (game_bitmap && game_bitmap->line[0]) {
      // Couleurs exactes ; tout renvoye toutes les 500 ms (une image
      // compressee ne coute presque rien : une bande abimee en route ne reste
      // plus 2 s a l'ecran).
      video_link_submit_frame(game_bitmap->line, myPaletteRGB888, 500);
    }
    video_link_tick(true);
  }

  crash_guard_confirm_boot();

  frame_count++;
  if (millis() - last_fps_update >= 1000) {
    current_fps = frame_count;
    frame_count = 0;
    last_fps_update = millis();
  }
}

// Image du jeu creee avec une marge de 8 pixels de chaque cote de chaque
// ligne (GAME_BITMAP_OVERDRAW), comme dans le nofrendo d'origine (nes.c :
// bmp_create(..., 8)) : le PPU dessine 33 tuiles par ligne a partir de
// "ligne - decalage fin" et ecrirait sinon jusqu'a 8 octets hors du tampon.
// Les lignes sont lues par leurs pointeurs (bmp->line[]).
#define GAME_BITMAP_OVERDRAW 8

extern "C" int vid_init(int width, int height, viddriver_t *osd_driver) {
  if (!game_bitmap) game_bitmap = bmp_create(NES_SCREEN_WIDTH, NES_SCREEN_HEIGHT, GAME_BITMAP_OVERDRAW);
  frame_buffer = NULL;
  low_mem_video_mode = false;
  video_ready = (game_bitmap && game_bitmap->data);
  Serial.printf("vid_init heap snapshot: heap=%u spiram=%u\n", esp_get_free_heap_size(), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  Serial.printf("vid_init: bitmap %p, frame_buffer %p, ready %d, deferred_fb_alloc=1\n", game_bitmap, frame_buffer, video_ready);
  return video_ready ? 0 : -1;
}

extern "C" int osd_rom_open(const char *path) {
  if (rom_stream_file) {
    rom_stream_file.close();
  }

  Serial.printf("[ROM] Streaming open: %s\n", path ? path : "(null)");
  // "/nom.nes" (carte SD) ou "flash:/nom.nes" (memoire interne), voir
  // rom_storage.h.
  rom_stream_file = path ? rom_storage_open_read(path) : File();
  if (!rom_stream_file) {
    Serial.println("[ROM] Streaming open failed");
    return -1;
  }
  return 0;
}

extern "C" int osd_rom_read(void *dst, int len) {
  if (!rom_stream_file || !dst || len <= 0) {
    return -1;
  }
  int n = rom_stream_file.read((uint8_t *)dst, len);
  return n;
}

extern "C" void osd_rom_close(void) {
  if (rom_stream_file) {
    rom_stream_file.close();
  }
}

extern "C" void vid_shutdown() {
  video_ready = false;
}

extern "C" int vid_setmode(int width, int height) {
  return 0;
}

extern "C" void vid_setpalette(rgb_t *pal) {
  osd_setpalette(pal);
}

extern "C" bitmap_t *vid_getbuffer() {
  if (!game_bitmap) game_bitmap = bmp_create(NES_SCREEN_WIDTH, NES_SCREEN_HEIGHT, GAME_BITMAP_OVERDRAW);
  video_ready = (game_bitmap && game_bitmap->data);
  return game_bitmap;
}

extern "C" void osd_getvideoinfo(vidinfo_t *info) {
  info->default_width = NES_SCREEN_WIDTH;
  info->default_height = NES_SCREEN_HEIGHT;
  info->driver = 0;
}

extern "C" void osd_togglefullscreen(int code) {}

extern "C" char *osd_newextension(char *string, char *ext) {
  return string;
}

extern "C" void osd_fullname(char *fullname, const char *shortname) {
  strcpy(fullname, shortname);
}

extern "C" int osd_main(int argc, char *argv[]) {
  runtime_sound_enabled = false;
  for (int i = 1; i < argc; i++) {
    if (argv[i] && strcmp(argv[i], "-sound") == 0) {
      runtime_sound_enabled = true;
    }
    if (argv[i] && strcmp(argv[i], "-nosound") == 0) {
      runtime_sound_enabled = false;
    }
  }

  char *rom_name = "rom";
  if (argc > 0) rom_name = argv[argc - 1];
  return main_loop(rom_name, system_nes);
}

extern "C" int nofrendo_log_init(void) {
  return 0;
}

extern "C" void nofrendo_log_shutdown(void) {}

extern "C" int nofrendo_log_print(const char *s) {
  if (s) {
    Serial.print(s);
  }
  return 0;
}

extern "C" int nofrendo_log_printf(const char *format, ...) {
  va_list args;
  va_start(args, format);
  int written = Serial.vprintf(format, args);
  va_end(args);
  return written;
}

extern "C" void nofrendo_log_assert(int expr, int line, const char *file, char *msg) {
  if (!expr) {
    Serial.printf("ASSERT FAILED: %s:%d %s\n", file ? file : "?", line, msg ? msg : "");
  }
}
