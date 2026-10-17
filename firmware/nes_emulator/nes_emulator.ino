// nes_emulator.ino - console NES portable sur ESP32-S3 (carte LCDWIKI
// ES3C28P : ESP32-S3R8, ecran ILI9341 2,8" 320x240, tactile FT6336, codec
// audio ES8311, lecteur microSD).
//
// Croquis principal : initialise le materiel, affiche le logo et les menus
// (LVGL, ui_lvgl.cpp), puis lance l'emulateur nofrendo sur le jeu choisi.
// Derive du depot "Esp32-S3-nes-emulator-by-DSN" (derdacavga, GPL-3.0), lui
// meme base sur le coeur d'emulation nofrendo (Matthew Conte).
//
// Organisation du croquis :
//   - coeur nofrendo (nes*.c, map*.c, nofrendo.c...) : CPU 6502, PPU, APU,
//     mappers ; quelques mappers refaits ou ajoutes (MMC1, MMC3, MMC5,
//     FME-7, VRC...), sauvegardes instantanees (savestate.c) ;
//   - osd.cpp : lien entre nofrendo et la carte (image, son, manettes,
//     boucle de jeu, menu pause) ;
//   - ui_lvgl.cpp : menus (liste des jeux, recherche, favoris, Reglages,
//     portail, themes) ;
//   - tft_driver.h, lcd_fullscreen.h : ecran ILI9341 (TFT_eSPI) ;
//   - audio_codec.cpp, es8311.cpp : configuration I2C du codec ES8311 ;
//   - controller.cpp, touch_controls.cpp, ft6336_touch.cpp,
//     bluetooth_controller.cpp : manette filaire (CD4021 / 74HC165), zones
//     tactiles et manette du telephone en Bluetooth (page manette8.html) ;
//   - rom_storage.cpp, game_catalog.cpp, game_library.cpp : jeux sur carte
//     SD et en memoire interne, catalogue, recents, favoris, sauvegardes ;
//   - wifi_boot.cpp, wifi_portal.cpp : Wi-Fi (box ou point d'acces) et
//     portail web (envoi de jeux, mise a jour du logiciel par OTA) ;
//   - video_link.cpp, tv_menu.cpp : sortie television par le coprocesseur
//     video (ESP32 classique, croquis video_coprocessor) ;
//   - crash_guard.cpp : mise a l'ecart des jeux qui plantent au lancement ;
//   - hw_config.h : broches et options materielles.
//
// Reglages de l'IDE Arduino (core esp32 3.x) : carte "ESP32S3 Dev Module",
// USB CDC On Boot "Enabled", PSRAM "OPI PSRAM", Flash Size 16 Mo, Partition
// Scheme "16M Flash (3MB APP/9.9MB FATFS)". Voir README.md.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include "FS.h"
#include "SD_MMC.h"
#include "rom_storage.h"
#include "game_library.h"   // Recents, favoris, sauvegardes
#include <esp_heap_caps.h>
#include "hw_config.h"
#include "tft_driver.h"
#include "audio_codec.h"
#include "touch_controls.h"
#include "crash_guard.h"
#include "video_link.h"
#include "wifi_boot.h"
#include "bluetooth_controller.h"   // Manette du telephone

TFT_eSPI my_lcd = TFT_eSPI();
TFTDriver tft;

extern void setup_controller();
extern "C" int nofrendo_main(int argc, char *argv[]);
extern "C" void osd_lcd_prepare_game_screen(void);   // osd.cpp
extern "C" bool vid_preload_rom(const char *path);
extern "C" int show_menu();
extern "C" const char *get_selected_game();

// Message d'erreur bloquant minimal, affiche directement via TFTDriver (pas de
// LVGL ici : LVGL ne sert qu'aux menus, voir ui_lvgl.cpp, et n'est pas encore
// initialise a ce stade).
static void show_fatal_error(const char *line1, const char *line2) {
  tft.fillScreen(0xF800);   // fond rouge
  tft.drawString(10, 60, "ERREUR", 0xFFFF, 0xF800, 2);
  tft.drawString(10, 100, line1, 0xFFFF, 0xF800, 1);
  if (line2 && line2[0]) {
    tft.drawString(10, 120, line2, 0xFFFF, 0xF800, 1);
  }
  Serial.printf("[FATAL] %s %s\n", line1, line2 ? line2 : "");
  while (true) {
    delay(1000);
  }
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n\n====================================");
  Serial.println("NES Emulator - portage carte ESP32-S3");
  Serial.printf("Version : %s\n", NES_EMULATOR_VERSION);
  Serial.printf("NES-WiFi : %s\n", NES_WIFI_VERSION);
  Serial.println("====================================\n");

  // Cause du redemarrage precedent (diagnostic : plantage, chien de garde,
  // chute de tension de l'alimentation...).
  {
    const esp_reset_reason_t r = esp_reset_reason();
    const char *why = "autre";
    switch (r) {
      case ESP_RST_POWERON:  why = "mise sous tension"; break;
      case ESP_RST_EXT:      why = "bouton reset"; break;
      case ESP_RST_SW:       why = "redemarrage logiciel (retour au menu)"; break;
      case ESP_RST_PANIC:    why = "PLANTAGE (voir les lignes Guru Meditation / Backtrace juste avant)"; break;
      case ESP_RST_INT_WDT:  why = "PLANTAGE (chien de garde des interruptions)"; break;
      case ESP_RST_TASK_WDT: why = "PLANTAGE (chien de garde des taches)"; break;
      case ESP_RST_WDT:      why = "PLANTAGE (chien de garde)"; break;
      case ESP_RST_BROWNOUT: why = "CHUTE DE TENSION (alimentation trop faible)"; break;
      case ESP_RST_USB:      why = "port USB (televersement, moniteur serie)"; break;
      default: break;
    }
    Serial.printf("[BOOT] Cause du redemarrage : %s (code %d)\n", why, (int)r);
  }

  // A faire tot, avant toute autre chose : detecte si le redemarrage en cours
  // fait suite a un plantage survenu pendant le lancement d'une ROM (voir
  // crash_guard.h) et met a jour sa liste noire en consequence, AVANT que
  // show_menu() (plus bas) ne dessine la liste.
  crash_guard_init();

  setup_controller();
  Serial.println("[1/5] Manette (74HC165) initialisee.");

  // Lien video vers le coprocesseur externe (2e ESP32, sortie composite ->
  // TV). Sans effet tant que ENABLE_VIDEO_LINK vaut 0 dans video_link.h
  // (valeur par defaut) - voir ce fichier avant d'activer.
  video_link_init();
  // Apres un redemarrage de fin de partie (SELECT maintenu), la sortie
  // Televiseur choisie avant la partie est conservee ; apres une mise sous
  // tension, on repart toujours sur l'ecran LCD.
  if (video_link_is_ready() && video_link_recall_tv_mode()) {
    g_display_tv_mode = true;
    Serial.println("[VIDEO-LINK] sortie Televiseur conservee apres la partie precedente.");
  }

  Serial.println("[2/5] Initialisation ecran (TFT_eSPI, ILI9341)...");
  tft.init();
  Serial.printf("     OK - resolution %dx%d\n", DISPLAY_WIDTH, DISPLAY_HEIGHT);
  // SPI_FREQUENCY est une macro de compilation (definie dans le User_Setup.h
  // de la bibliotheque TFT_eSPI, PAS dans ce croquis) - ce print confirme la
  // valeur reellement compilee, pour verifier qu'un changement fait dans ce
  // fichier externe a bien ete pris en compte (recompilation reussie) avant
  // d'aller chercher plus loin.
#ifdef SPI_FREQUENCY
  Serial.printf("     SPI_FREQUENCY compilee : %lu Hz\n", (unsigned long)SPI_FREQUENCY);
#else
  Serial.println("     SPI_FREQUENCY : non definie (TFT_eSPI utilise sa valeur par defaut)");
#endif

  // Retro-eclairage configure APRES tft.init() : TFT_eSPI touche lui-meme cette
  // broche pendant son init(), une configuration PWM faite avant serait
  // ecrasee.
  ledcAttachChannel(LCD_BL_PIN, LCD_BL_PWM_FREQ, LCD_BL_PWM_RES_BITS, LCD_BL_PWM_CHANNEL);
  ledcWrite(LCD_BL_PIN, 204);   // ~80 % (204/255)

  Serial.println("[3/5] Montage de la carte SD (SD_MMC 4 bits)...");
  // setPins() puis begin() en mode 1 bit (second parametre true) : le mode le
  // plus fiable sur cette carte, malgre le brochage 4 bits complet.
  if (!SD_MMC.setPins(SD_SCK, SD_CMD, SD_D0, SD_D1, SD_D2, SD_D3)) {
    show_fatal_error("Configuration des broches SD_MMC impossible", "");
  }

  // Memoire interne (partition FAT de la flash) montee AVANT la carte SD, pour
  // que la console reste utilisable sans carte SD (jeux en memoire interne +
  // portail web). Voir rom_storage.h. La toute premiere fois, la partition est
  // formatee (quelques secondes).
  tft.fillScreen(0x0000);
  tft.drawString(10, 110, "Memoire interne et carte SD...", 0xFFFF, 0x0000, 1);
  rom_storage_flash_begin([]() {
    tft.fillScreen(0x0000);
    tft.drawString(10, 100, "Premiere utilisation de la memoire", 0xFFFF, 0x0000, 1);
    tft.drawString(10, 116, "interne : preparation (jusqu'a 2 min)...", 0xFFFF, 0x0000, 1);
  });

  int sd_retry = 0;
  bool sd_ok = true;
  while (!SD_MMC.begin(ROM_SD_MOUNTPOINT, true)) {   // "/sdcard" (rom_storage.h)
    sd_retry++;
    Serial.printf("     Echec montage SD (essai %d/5)\n", sd_retry);
    if (sd_retry >= 5) {
      // Sans carte SD, on continue si la memoire interne est disponible (jeux
      // qui y sont ranges, portail pour en envoyer).
      if (rom_storage_flash_ok()) {
        Serial.println("     Carte SD absente : on continue avec la memoire interne seule.");
        sd_ok = false;
        break;
      }
      show_fatal_error("Carte SD introuvable",
                        "Verifie qu'elle est inseree et formatee (FAT32)");
    }
    delay(1000);
  }
  rom_storage_set_sd_ok(sd_ok);
  // Restes d'un envoi par le portail interrompu par une coupure.
  rom_storage_cleanup_leftovers();
  rom_storage_log_format();   // FAT16 / FAT32, racine (journal)
  lib_init();                 // Recents et favoris (NVS)
  tft.fillScreen(0x0000);
  // rom_storage_total_bytes() et non SD_MMC.totalBytes(), qui lit le volume
  // FatFs "0:" = la memoire interne quand elle est montee avant.
  if (sd_ok) Serial.printf("     OK - taille totale : %llu Mo\n",
                           (unsigned long long)(rom_storage_total_bytes(ROM_WHERE_SD) / 1024 / 1024));

  // Met en quarantaine la ROM ayant fait planter le demarrage precedent (voir
  // crash_guard.h). DOIT rester APRES le montage du stockage ci-dessus et
  // AVANT show_menu() plus bas (qui relit la liste des jeux).
  crash_guard_process_pending_deletion();

  Serial.println("[4/5] Amplificateur audio + codec ES8311 + tactile...");
  // L'amplificateur (actif a l'etat bas) est active AVANT le codec.
  pinMode(AP_ENABLE, OUTPUT);
  digitalWrite(AP_ENABLE, LOW);

  if (audio_codec_init() != ESP_OK) {
    // Pas bloquant (contrairement a l'ecran/la carte SD) : le jeu reste
    // jouable sans son si le codec ne repond pas - simple message dans
    // les logs, deja donne en detail par audio_codec_init() elle-meme.
    Serial.println("     ATTENTION : codec audio indisponible, le jeu continuera SANS SON.");
  } else {
    Serial.println("     OK (codec)");
  }

  // Tactile FT6336, sur le MEME bus I2C que le codec (voir
  // audio_codec_init() ci-dessus, qui doit avoir tourne en premier -
  // meme si le codec lui-meme a echoue, le bus I2C reste utilisable).
  // Etalonnage a 3 points bloquant si aucun etalonnage n'est deja
  // enregistre - voir touch_controls.cpp.
  touch_controls_init_and_calibrate();

  Serial.println("[5/5] Verification de la PSRAM...");
  Serial.printf("     Tas libre : %u octets\n", esp_get_free_heap_size());
  Serial.printf("     PSRAM libre : %u octets\n", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  Serial.printf("     PSRAM totale : %u octets\n", heap_caps_get_total_size(MALLOC_CAP_SPIRAM));

  if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0) {
    // Ne devrait normalement pas arriver sur cette carte : le schema
    // constructeur indique un ESP32-S3R8 (PSRAM Octal 8 Mo integree).
    // Si ce message s'affiche quand meme, verifier le reglage "PSRAM"
    // dans le menu Outils de l'IDE Arduino ("OPI PSRAM", pas
    // "Disabled").
    show_fatal_error("PSRAM non initialisee",
                      "Verifie le reglage PSRAM du menu Outils");
  }

  // Le Bluetooth ne demarre pas ici (ni dans setup_controller() : trop tot, la
  // pile Bluetooth plantait). Il demarre a la fin du logo, une fois la
  // connexion Wi-Fi terminee (bluetooth_controller_radio_check() dans
  // show_menu(), osd.cpp), et seulement si la console n'a pas ouvert son
  // point d'acces : point d'acces et Bluetooth ne cohabitent pas de facon
  // fiable. Sinon il demarre au debut de la partie, juste apres la coupure du
  // Wi-Fi (plus bas).

  Serial.println("\n====================================");
  Serial.println("Pret - affichage du menu");
  Serial.println("====================================\n");
  Serial.flush();

  int selected = show_menu();
  if (selected < 0) {
    show_fatal_error("Aucun fichier .nes trouve sur la carte SD", "");
  }

  const char *rom_file = get_selected_game();
  Serial.printf("Lancement du jeu : %s\n", rom_file);

  // Coupe le Wi-Fi (box ou point d'acces, portail web compris) avant la
  // partie. Sans effet si ENABLE_WIFI_PORTAL vaut 0 (wifi_boot.h) ou si le
  // Wi-Fi n'a jamais ete actif depuis le demarrage.
  wifi_boot_shutdown_for_game();

  // Le Wi-Fi est coupe, la radio est libre. Demarre le Bluetooth s'il
  // attendait la fin du point d'acces (ou s'il avait rate son demarrage), ou
  // le sort de sa pause : le telephone peut se connecter pendant la partie.
  bluetooth_controller_radio_check(/* game_start= */ true);

  // On quitte les menus pour la partie : en sortie Televiseur, l'ecran LCD est
  // eteint et nes_get_gamepad_state() ignore le tactile (sauf SELECT).
  g_in_menu_screen = false;

  // Une demande "MENU JEUX" du telephone arrivee pendant l'ecran
  // "Lancement..." n'interrompt pas la partie qui demarre.
  bluetooth_controller_take_menu_request();

  // Etiquettes de boutons dans les deux bandes de 32 px de large que le
  // rendu du jeu ne touche jamais (voir touch_controls.cpp) - dessinees
  // une seule fois ici, juste avant de lancer la partie (le menu qui
  // vient de se derouler occupe tout l'ecran, donc les dessiner plus tot
  // aurait ete efface par le menu lui-meme).
  // Via osd_lcd_prepare_game_screen() (osd.cpp), qui ne les dessine pas si la
  // partie va s'afficher en plein ecran (manette du telephone deja connectee,
  // voir LCD_FULLSCREEN_MODE dans hw_config.h).
  osd_lcd_prepare_game_screen();

  // ENABLE_SOUND vaut 1 dans hw_config.h : "-sound" est passe a
  // l'emulateur (voir osd.cpp / osd_main(), qui lit ce parametre pour
  // decider d'activer ou non le rappel audio). "-volume 100" et
  // "-sample 16000" sont deux arguments herites tels quels du croquis
  // d'origine : ni l'un ni l'autre n'est en realite lu par osd_main()
  // (verifie dans osd.cpp - seuls "-sound"/"-nosound" et le nom du
  // fichier ROM le sont), le volume reel est celui explique dans
  // audio_codec.cpp et la frequence reelle est NES_AUDIO_SAMPLE_RATE
  // (hw_config.h) - ces deux arguments restent donc de simples
  // etiquettes informatives sans effet, gardees pour ne pas s'ecarter
  // sans raison du croquis d'origine.
  // A faire juste avant de lancer nofrendo_main() (qui est bloquant et ne rend
  // la main qu'en cas d'arret complet de l'emulation) : marque cette ROM comme
  // "en cours de lancement" pour que crash_guard, au prochain demarrage, sache
  // si un plantage s'est produit pendant CE lancement precis - voir
  // crash_guard.h.
  crash_guard_begin_launch(rom_file);

#if ENABLE_SOUND
  char *argv[] = {
    "nes",
    "-sound",
    "-volume", "100",
    "-sample", "16000",
    (char *)rom_file
  };
  nofrendo_main(7, argv);
#else
  char *argv[] = {
    "nes",
    "-nosound",
    (char *)rom_file
  };
  nofrendo_main(3, argv);
#endif

  // nofrendo_main() ne rend la main que si aucun jeu n'a pu etre charge
  // (fichier illisible, memoire...) : message, puis retour propre au menu,
  // sans mettre le jeu en quarantaine.
  Serial.println("[JEU] Aucun jeu n'a pu etre charge - retour au menu (redemarrage).");
  crash_guard_clear_for_clean_exit();
  tft.fillScreen(0x0000);
  tft.drawString(40, 104, "Jeu impossible a charger", 0xF800, 0x0000, 1);
  tft.drawString(40, 120, "Retour au menu...", 0xFFFF, 0x0000, 1);
  // Le refus (deja confie a la pile Bluetooth par osd_game_load_failed())
  // part vers le telephone avant la deconnexion.
  delay(1500);
  bluetooth_controller_disconnect_before_restart();
  delay(500);
  ESP.restart();
}

void loop() {
  // nofrendo_main() ci-dessus est bloquant (boucle de jeu synchrone) et
  // ne rend la main que si l'emulation s'arrete completement - loop() ne
  // fait donc rien de plus ici, exactement comme dans le croquis
  // d'origine.
  delay(1000);
}
