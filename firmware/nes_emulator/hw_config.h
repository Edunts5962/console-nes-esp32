// hw_config.h - broches et options materielles de la console.
//
// Carte principale : LCDWIKI ES3C28P (ESP32-S3R8 : 2 coeurs LX7 a 240 MHz,
// 512 Ko de SRAM, 8 Mo de PSRAM OPI, 16 Mo de flash), ecran ILI9341 2,8"
// 320x240 en SPI, tactile capacitif FT6336 (I2C), codec audio ES8311 (I2C +
// I2S) avec amplificateur, lecteur microSD en SD_MMC.
//
// Le depot d'origine ("Esp32-S3-nes-emulator-by-DSN", derdacavga) visait une
// autre carte (SD en SPI, ecran ST7789, 8 boutons sur 8 GPIO, DAC I2S
// MAX98357A). Le coeur nofrendo (CPU 6502, PPU, APU, mappers) ne touche a
// aucune broche : tout ce qui depend du materiel est regroupe ici, dans
// osd.cpp et dans les fichiers propres a ce portage.
//
// Son : le codec ES8311 doit etre CONFIGURE par I2C (frequence
// d'echantillonnage, volume materiel, horloge MCLK) avant que l'envoi I2S ait
// le moindre effet - voir audio_codec.cpp. Le flux audio lui-meme, genere en
// temps reel par nofrendo, part par le pilote I2S bas niveau d'ESP-IDF
// (i2s_write() dans osd.cpp).
#ifndef HW_CONFIG_H
#define HW_CONFIG_H

// Version du logiciel, affichee au demarrage (port serie), dans les menus
// (Reglages > A propos) et par le portail web (mise a jour OTA).
#define NES_EMULATOR_VERSION "v2.20.1-BETA-TV (2026-10-11)"
// Version courte, pour l'affichage a l'ecran (la version complete deborde).
#define NES_EMULATOR_VERSION_SHORT "v2.20.1-BETA-TV"

// Version du portail web (Wi-Fi), affichee au demarrage et dans les menus.
#define NES_WIFI_VERSION "v0.2-BETA-PORTAIL (2026-09-24)"
#define NES_WIFI_VERSION_SHORT "v0.2-BETA-PORTAIL"

#define ENABLE_SOUND 1

// Frequence d'echantillonnage utilisee PARTOUT pour le son (codec ES8311,
// audio_codec.cpp, ET flux I2S genere par nofrendo, osd.cpp) : une seule
// definition pour que les deux ne puissent pas diverger. Valeur du depot
// d'origine : plus haute, elle ne rendrait pas le son de l'APU NES meilleur
// et couterait du temps processeur.
#define NES_AUDIO_SAMPLE_RATE 16000

/* -------------------- I2C (codec ES8311 et tactile FT6336) -------------------- */
#define AUDIO_I2C_SDA   16
#define AUDIO_I2C_SCL   15
#define AUDIO_I2C_SPEED 400000

/* -------------------- I2S vers le codec ES8311 -------------------- */
#define I2S_BCK 5
#define I2S_WS  7
#define I2S_DO  8   // sortie de donnees I2S vers le codec
#define I2S_MCK 4   // horloge maitre (MCLK) - indispensable pour l'ES8311

/* -------------------- Amplificateur de puissance --------------------
 * Active a l'etat BAS.
 */
#define AP_ENABLE 1

/* -------------------- Retro-eclairage LCD (PWM) --------------------
 * TFT_eSPI n'allume PAS l'ecran tout seul sur cette carte : sans ce PWM
 * (demarre dans setup(), nes_emulator.ino), l'ecran reste noir. La
 * luminosite se regle dans Reglages (memorisee en NVS).
 */
#define LCD_BL_PIN           45
#define LCD_BL_PWM_FREQ      2000
#define LCD_BL_PWM_RES_BITS  8
#define LCD_BL_PWM_CHANNEL   0

/* -------------------- Tactile FT6336 --------------------
 * Dans les menus LVGL, le doigt agit partout sur l'ecran. Pendant une
 * partie, USE_TOUCH_CONTROLS active des zones tactiles invisibles dans les
 * deux bandes de 32 px a gauche et a droite de l'image du jeu (256x240 au
 * centre) : croix a gauche, A / B / START / SELECT a droite. Ces bandes ne
 * sont jamais ecrites par le rendu du jeu, leurs etiquettes restent donc
 * visibles toute la partie.
 */
#define TOUCH_RST 18
#define USE_TOUCH_CONTROLS 1

/* HAS_PHYSICAL_CONTROLLER : 1 = manette filaire branchee (manette NES a
 * CD4021 de la carte porteuse, ou montage a 74HC165 - voir PAD_PROTOCOL plus
 * bas). A 0 (defaut), les broches de la manette ne sont ni configurees ni
 * lues : une entree flottante pourrait produire des appuis fantomes.
 */
#define HAS_PHYSICAL_CONTROLLER 0

/* ENABLE_BLE_GAMEPAD : manette virtuelle du telephone en Bluetooth basse
 * consommation (page manette8.html, bluetooth_controller.cpp). 1 = active
 * (defaut) : son etat s'ajoute (OU) a celui de la manette filaire et du
 * tactile, et son bouton "MENU JEUX" ramene a la liste des jeux. 0 = la pile
 * Bluetooth n'est ni compilee ni demarree (programme plus petit d'environ
 * 230 Ko, memoire interne liberee).
 */
#define ENABLE_BLE_GAMEPAD 1

/* LCD_FULLSCREEN_MODE : image du jeu en plein ecran sur le LCD (320x240) au
 * lieu de 256x240 entre les deux bandes tactiles (voir lcd_fullscreen.h).
 *   0 = jamais ;
 *   1 = quand la manette du telephone est connectee (defaut). Liaison
 *       perdue en cours de partie : retour a l'affichage normal, bandes
 *       tactiles redessinees, apres LCD_FULLSCREEN_EXIT_DELAY_MS ;
 *   2 = toujours.
 * En plein ecran, le tactile ne garde que la zone SELECT (coin bas-droit,
 * invisible) : la maintenir 3 s ramene au menu, comme en mode Televiseur.
 * Sans effet en mode Televiseur (ecran LCD eteint pendant la partie).
 * Cout : 25 % de pixels en plus a envoyer a l'ecran a chaque image, donc un
 * peu moins d'images affichees par seconde ; la vitesse du jeu ne change pas
 * (l'emulateur saute des images pour garder le rythme).
 */
#define LCD_FULLSCREEN_MODE 1

/* Lignes retirees en haut ET en bas en plein ecran : 8 (defaut) = 224 lignes
 * etirees sur 240, proportions d'une NES sur un televiseur (ces lignes
 * etaient cachees par le bord des televiseurs) ; 0 = les 240 lignes, image
 * un peu plus large que sur un televiseur. De 0 a 16. */
#define LCD_FULLSCREEN_CROP_LINES 8

/* Mode 1 : delai avant le retour a l'affichage normal apres la perte de la
 * liaison avec le telephone. Evite un aller-retour pour une coupure breve
 * (la page du telephone se reconnecte seule). */
#define LCD_FULLSCREEN_EXIT_DELAY_MS 5000

/* LCD_GAME_COLOR_FIX : ordre des octets des couleurs de l'image du jeu
 * envoyee a l'ecran LCD. 1 (defaut) = palette normale, meme convention que
 * les menus LVGL et le logo (voir tft_driver.h, swap_bytes). 0 = octets
 * inverses (couleurs fausses sur cet ecran). Sans effet sur la sortie
 * television (palette separee).
 */
#define LCD_GAME_COLOR_FIX 1

/* USE_LVGL_UI : 1 = menus LVGL (ui_lvgl.cpp, defaut). 0 = ancienne interface
 * dessinee a la main (osd.cpp), sans LVGL a l'execution ; la bibliotheque
 * reste necessaire a la compilation (les polices ui_font_*.c l'incluent).
 */
#define USE_LVGL_UI 1

/* g_display_tv_mode (definie dans osd.cpp) : sortie de l'image du jeu -
 * false = ecran LCD de la console ; true = image et son envoyes au
 * coprocesseur video pour la television, ecran LCD eteint pendant la partie
 * (voir vid_flush() dans osd.cpp). Toujours false si ENABLE_VIDEO_LINK vaut 0
 * (video_link.h). Pas sauvegardee en flash : chaque mise sous tension repart
 * sur l'ecran LCD, pour ne jamais rester bloque sur un ecran noir si la
 * carte video n'est pas branchee. Seul le redemarrage logiciel de FIN DE
 * PARTIE conserve le choix Televiseur (memoire RTC, voir
 * video_link_recall_tv_mode()).
 */
extern bool g_display_tv_mode;

/* g_in_menu_screen (definie dans osd.cpp) : true dans les ecrans de menu,
 * false une fois la partie lancee (juste avant nofrendo_main(), voir
 * nes_emulator.ino). En mode Television, le tactile n'est ignore que pendant
 * la partie (l'ecran LCD est alors eteint). Voir nes_get_gamepad_state()
 * (controller.cpp).
 */
extern bool g_in_menu_screen;

/* g_lcd_fullscreen_active (definie dans osd.cpp) : true pendant une partie
 * affichee en plein ecran sur le LCD (voir LCD_FULLSCREEN_MODE plus haut),
 * false sinon (menus, affichage normal, mode Televiseur). Comme en mode
 * Televiseur, le tactile ne garde alors que la zone SELECT (controller.cpp).
 * Mise a jour a chaque image par vid_flush() (osd.cpp).
 */
extern bool g_lcd_fullscreen_active;

/* g_video_pal_mode : norme video envoyee a la carte video TV - false = NTSC,
 * true = PAL (defaut). Reglable dans l'onglet Video des menus (qui propose
 * aussi PAL 60, voir video_link.h) et memorisee en NVS ("nesui"/"pal").
 * Pas de SECAM : la carte video ne produit que PAL et NTSC, acceptes par les
 * televiseurs et convertisseurs recents. Sans effet si ENABLE_VIDEO_LINK
 * vaut 0.
 */
extern bool g_video_pal_mode;

/* -------------------- Carte microSD (SD_MMC) --------------------
 * Brochage complet du lecteur (4 bits) ; la carte est utilisee en mode
 * 1 bit (voir SD_MMC.begin() dans nes_emulator.ino).
 */
#define SD_SCK  38
#define SD_CMD  40
#define SD_D0   39
#define SD_D1   41
#define SD_D2   48
#define SD_D3   47

/* -------------------- Ecran TFT --------------------
 * Broches de l'ecran definies dans la configuration de TFT_eSPI
 * (User_Setup.h, voir config/ dans le depot) : aucune n'est repetee ici.
 */

/* -------------------- Manette filaire : broches --------------------
 * La carte n'a que 6 GPIO libres, sur deux connecteurs :
 *   - "Expanded IO" (P3, 4 broches) : GPIO21, GPIO14, GPIO3, GPIO2 ;
 *   - "UART" (P2) : GPIO44 (RXD0) et GPIO43 (TXD0), reutilisables tant que
 *     "USB CDC On Boot" reste active dans l'IDE Arduino (le port serie de
 *     debogage passe alors par l'USB).
 * Elles sont toutes utilisees :
 *   - manette filaire        : GPIO21 (LATCH/PL), GPIO14 (CLOCK), GPIO44 (DATA)
 *   - liaison carte video    : GPIO2 (SCLK), GPIO3 (CS), GPIO43 (MOSI)
 *     (voir VIDEO_LINK_PIN_* dans video_link.h).
 * GPIO1 (amplificateur, AP_ENABLE) n'est sur aucun de ces connecteurs.
 */
#define HC165_PL   21   // LATCH (manette NES, actif haut) ou Shift/Load (74HC165, actif bas) - "Expanded IO", IO21
#define HC165_CLK  14   // CLOCK - connecteur "Expanded IO", broche IO14
#define HC165_Q7   44   // DATA (manette NES) ou QH (74HC165) - connecteur "UART", RXD0 (GPIO44)

/* -------------------- Protocole de la manette --------------------
 * PAD_PROTOCOL_NES (defaut) : manette "manette_nes" du projet KiCad
 *   (registre CD4021BE, prise DB9, 3,3 V) ou toute manette NES d'origine /
 *   compatible ALIMENTEE EN 3,3 V. Memes trois broches que ci-dessus :
 *     GPIO21 = LATCH (impulsion HAUTE de 12 us : les 8 boutons sont charges),
 *     GPIO14 = CLOCK (chaque front montant passe au bouton suivant),
 *     GPIO44 = DATA  (0 = appuye), dans l'ordre fixe de la NES :
 *     A, B, Select, Start, Haut, Bas, Gauche, Droite.
 *   Sur la carte porteuse, ces signaux passent par les cavaliers JP1-JP3
 *   (position 1-2) et des resistances de 1 kOhm ; DATA a une resistance de
 *   rappel de 47 kOhm : sans manette branchee, aucun bouton n'est vu appuye.
 * PAD_PROTOCOL_HC165 : montage a 74HC165 (8 boutons-poussoirs, PL actif BAS,
 *   ordre HC165_BIT_* ci-dessous).
 * Dans les deux cas, HAS_PHYSICAL_CONTROLLER (plus haut) doit valoir 1.
 */
#define PAD_PROTOCOL_HC165 0
#define PAD_PROTOCOL_NES   1
#define PAD_PROTOCOL       PAD_PROTOCOL_NES
#define NES_PAD_LATCH_US   12   // duree de l'impulsion LATCH (valeur de la console NES)
#define NES_PAD_HALF_US    6    // demi-periode de CLOCK : ~80 kHz, sur pour un CD4021B a 3,3 V

/* -------------------- Montage 74HC165 : bits des boutons --------------------
 * Chaque bouton-poussoir relie une entree D0..D7 du 74HC165 a la masse, avec
 * une resistance de tirage vers 3,3 V sur cette entree : bouton appuye =
 * entree a 0 (logique lue dans controller.cpp). Ordre D0..D7 : UP, DOWN,
 * LEFT, RIGHT, A, B, START, SELECT.
 */
#define HC165_BIT_UP      0
#define HC165_BIT_DOWN    1
#define HC165_BIT_LEFT    2
#define HC165_BIT_RIGHT   3
#define HC165_BIT_A       4
#define HC165_BIT_B       5
#define HC165_BIT_START   6
#define HC165_BIT_SELECT  7

#endif
