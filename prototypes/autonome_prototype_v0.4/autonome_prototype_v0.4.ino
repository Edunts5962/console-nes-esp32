// Prototype de l'EMULATEUR MINIMALISTE (carte autonome : un seul ESP32
// "classique", sortie TV composite, carte microSD, manette). Il valide le
// materiel pas a pas SANS encore executer de ROM.
//
// ALIGNEMENT SUR LA CARTE PORTEUSE KICAD (console_nes) :
//   - MANETTE : protocole NES (manette KiCad "manette_nes" a CD4021BE, ou
//     manette NES alimentee en 3,3 V) : LATCH GPIO32 (actif haut), CLOCK
//     GPIO33, DATA GPIO27 - cavaliers JP1 a JP3 de la carte porteuse en
//     position 2-3. Ordre : A, B, Select, Start, Haut, Bas, Gauche, Droite.
//   - SON : GPIO26 est reserve au son (filtre R10/C8, C9, RV1 et prises RCA
//     blanche/rouge de la carte porteuse), comme sur la carte video. Ce
//     prototype n'emet pas encore de son.
//
// CE QUE CE PROTOTYPE VALIDE :
//   - Video composite couleur (GPIO25) : scene animee (cercle qui rebondit,
//     ligne qui defile) + 8 carres-temoins, un par bouton de la manette.
//   - Carte microSD en SPI (module du connecteur J12) : liste les .nes de la
//     racine puis charge entierement le premier en RAM (en-tete iNES verifie).
//   - Manette : lue a chaque image si HAS_PHYSICAL_CONTROLLER vaut 1 ; sinon
//     mode demo (un bouton simule toutes les ~600 ms).
//   - Portail Wi-Fi (Station puis repli Point d'acces) : norme PAL/NTSC,
//     liste des ROMs, etat de la manette.
//
// CARTE A CHOISIR DANS ARDUINO IDE : "ESP32 Dev Module" (coeur esp32 3.x).
// BIBLIOTHEQUE : "LovyanGFX" (1.2.x), comme la carte video.
//
// BROCHAGE (carte porteuse KiCad, U3 = ESP32-DevKitC V4) :
//   GPIO25 -> RCA jaune (direct)          GPIO26 -> son (reserve)
//   microSD J12 : SCK=18 MISO=19 MOSI=23 CS=5 (R12 : rappel de CS au 3,3 V)
//   Manette DB9 J8 via JP1-JP3 en 2-3 : LATCH=32 CLOCK=33 DATA=27
//
// Prototype : compile avec le coeur Arduino-ESP32 3.3 et LovyanGFX 1.2, a
// valider pas a pas (moniteur serie pour la SD et la manette, navigateur
// pour le portail, puis la television).

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <math.h>
#include <LovyanGFX.hpp>

#if !defined(CONFIG_IDF_TARGET_ESP32)
#error "Emulateur minimaliste : choisir 'ESP32 Dev Module' (ESP32 classique, WROOM-32/32E)."
#endif
#if !defined(ESP_ARDUINO_VERSION_MAJOR) || (ESP_ARDUINO_VERSION_MAJOR < 3)
#error "Emulateur minimaliste : coeur Arduino-ESP32 3.x requis."
#endif

// -------------------- Video --------------------
// USE_PAL devient une variable (g_use_pal) au lieu d'un #define fige, pour
// pouvoir etre changee a chaud depuis le portail WiFi ci-dessous (voir
// handleSetNorme()). PAL par defaut au demarrage (bon choix pour une TV en
// France, meme raisonnement que la carte principale).
static bool g_use_pal = true;
#define FRAME_WIDTH  256
#define FRAME_HEIGHT 240
#define PIN_VIDEO    25    // DAC 1 -> RCA jaune (direct)
#define PIN_AUDIO    26    // reserve au son (pas encore utilise ici)
#define VIDEO_OUTPUT_LEVEL 128   // amplitude : 128 par defaut ; 160-200 si l'image est sombre

// Meme reglage que la carte video : 288 echantillons par ligne, image de
// 256 pixels centree (16 colonnes noires de chaque cote).
#define TV_MEMORY_WIDTH 288
#define TV_OFFSET_X ((TV_MEMORY_WIDTH - FRAME_WIDTH) / 2)

class TvOut : public lgfx::LGFX_Device {
 public:
  lgfx::Panel_CVBS panel;
  TvOut() {
    auto cfg = panel.config();
    cfg.memory_width = TV_MEMORY_WIDTH;
    cfg.memory_height = FRAME_HEIGHT;
    cfg.panel_width = FRAME_WIDTH;
    cfg.panel_height = FRAME_HEIGHT;
    cfg.offset_x = TV_OFFSET_X;
    cfg.offset_y = 0;
    panel.config(cfg);
    auto det = panel.config_detail();
    det.signal_type = lgfx::Panel_CVBS::config_detail_t::signal_type_t::PAL;
    det.pin_dac = PIN_VIDEO;
    det.use_psram = 0;
    det.output_level = VIDEO_OUTPUT_LEVEL;
    panel.config_detail(det);
    setPanel(&panel);
  }
};

static TvOut tv;
static bool g_video_ok = false;

// Couleurs (RGB888 en uint32_t : LovyanGFX convertit vers le RGB332 de l'image)
static const uint32_t COL_BG     = 0x101830u;
static const uint32_t COL_LINE   = 0x20C0FFu;
static const uint32_t COL_BALL   = 0xFFB020u;
static const uint32_t COL_KEY_ON = 0xFFFFFFu;
static const uint32_t COL_KEY_OFF= 0x303848u;

static void video_apply_standard(bool pal) {
  tv.panel.setResolution(FRAME_WIDTH, FRAME_HEIGHT,
                         pal ? lgfx::Panel_CVBS::config_detail_t::signal_type_t::PAL
                             : lgfx::Panel_CVBS::config_detail_t::signal_type_t::NTSC,
                         TV_MEMORY_WIDTH, pal ? 288 : 240, TV_OFFSET_X, pal ? 24 : 0);
}

// -------------------- Portail WiFi (parametrage) --------------------
// Meme mecanisme hybride que wifi_boot.cpp/wifi_portal.cpp de la carte
// principale (Station d'abord, repli Point d'acces si echec) - SSID de
// secours volontairement different ("NES-AUTONOME" vs "NES-WiFi") pour ne
// jamais confondre les deux cartes au moment de s'y connecter en WiFi.
#include <WiFi.h>
#include <WebServer.h>
#define WIFI_SSID         "MonReseauWiFi"   // a remplacer par le nom de votre box
#define WIFI_PASSWORD     "MonMotDePasse"   // et son mot de passe
#define WIFI_AP_SSID      "NES-AUTONOME"
#define WIFI_AP_PASSWORD  "autonome123"
#define WIFI_STA_TIMEOUT_MS 10000

static WebServer server(80);
static bool g_ap_mode = false;   // true = repli Point d'acces (Station a echoue)
static String g_rom_list_html = "(aucune ROM listee pour l'instant)";

// -------------------- Carte SD (SPI simple, PAS SD_MMC) --------------------
#define SD_PIN_SCK  18
#define SD_PIN_MISO 19
#define SD_PIN_MOSI 23
#define SD_PIN_CS   5
static int rom_count = 0;
static String first_rom_path = "";   // Chemin de la premiere ROM trouvee, pour le test de chargement ci-dessous

// Format iNES (magic + compte de banques PRG/CHR), meme convention que
// ROM_INES_MAGIC dans nes_rom.c (carte principale) - repris ici juste pour la
// verification, pas pour charger reellement la ROM dans l'emulateur.
#define ROM_INES_MAGIC "NES\x1A"

// -------------------- Manette physique (protocole NES) --------------------
// Meme protocole que controller.cpp de la console complete (PAD_PROTOCOL_NES) :
// LATCH a 1 pendant 12 us, puis 8 lectures de DATA, chacune suivie d'un
// front montant de CLOCK. Ordre : A, B, Select, Start, Haut, Bas, Gauche,
// Droite. DATA a 0 = appuye. Brochage : JP1-JP3 de la carte porteuse en 2-3.
//
// MODE DEMO : HAS_PHYSICAL_CONTROLLER a 0 simule un bouton appuye toutes les
// ~600 ms (utile sans manette). Repasser a 1 une fois la manette branchee :
// sans manette, la resistance de rappel (47 kOhm sur la carte porteuse +
// rappel interne) fait lire "aucun bouton appuye".
#define HAS_PHYSICAL_CONTROLLER 0

#define PAD_LATCH 32   // -> JP1 (2-3) -> R6 -> DB9 broche 3
#define PAD_CLOCK 33   // -> JP2 (2-3) -> R7 -> DB9 broche 4
#define PAD_DATA  27   // <- JP3 (2-3) <- R8 <- DB9 broche 2
#define PAD_LATCH_US 12
#define PAD_HALF_US   6

static const char *PAD_NAME[8] = {
  "A", "B", "SELECT", "START", "HAUT", "BAS", "GAUCHE", "DROITE"
};

static uint8_t pad_prev = 0;   // masque des boutons appuyes (bit i = PAD_NAME[i])

#if HAS_PHYSICAL_CONTROLLER
static void controller_init() {
  pinMode(PAD_LATCH, OUTPUT);
  pinMode(PAD_CLOCK, OUTPUT);
  pinMode(PAD_DATA, INPUT_PULLUP);
  digitalWrite(PAD_LATCH, LOW);
  digitalWrite(PAD_CLOCK, LOW);
}

// Renvoie le masque des boutons APPUYES (bit 0 = A ... bit 7 = Droite).
static uint8_t controller_read() {
  digitalWrite(PAD_LATCH, HIGH);
  delayMicroseconds(PAD_LATCH_US);
  digitalWrite(PAD_LATCH, LOW);
  delayMicroseconds(PAD_HALF_US);
  uint8_t pressed = 0;
  for (int i = 0; i < 8; i++) {
    if (digitalRead(PAD_DATA) == LOW) pressed |= (uint8_t)(1 << i);
    digitalWrite(PAD_CLOCK, HIGH);
    delayMicroseconds(PAD_HALF_US);
    digitalWrite(PAD_CLOCK, LOW);
    delayMicroseconds(PAD_HALF_US);
  }
  return pressed;
}
#else
// Mode demo : un bouton different "appuye" toutes les ~600 ms.
static uint8_t controller_read() {
  static int demo_index = 0;
  static int demo_counter = 0;
  demo_counter++;
  if (demo_counter >= 15) {   // ~600 ms a 40 ms/image (voir loop())
    demo_counter = 0;
    demo_index = (demo_index + 1) % 8;
  }
  return (uint8_t)(1 << demo_index);
}
#endif

// -------------------- Video : scene demo (cercle + ligne qui bougent) --------------------
// Un cercle qui rebondit et une ligne qui defile : d'un coup d'oeil, on voit
// si le signal est stable (formes nettes, pas de dechirure). Dessin direct
// dans l'image affichee : on efface seulement les anciennes positions (pas
// d'ecran entier repeint a chaque image, donc pas de clignotement).
static int s_line_y = 0;
static int s_cx = FRAME_WIDTH / 2, s_cy = FRAME_HEIGHT / 2;
static int s_vx = 3, s_vy = 2;
static const int BALL_R = 18;
static const int KEYS_Y = 4, KEY_SQ = 16, KEY_GAP = 4;

static void draw_background() {
  tv.fillScreen(COL_BG);
}

static void draw_demo_scene() {
  tv.startWrite();
  // efface la ligne et le cercle precedents
  tv.drawFastHLine(0, s_line_y, FRAME_WIDTH, COL_BG);
  tv.fillCircle(s_cx, s_cy, BALL_R, COL_BG);
  // nouvelles positions
  s_line_y = (s_line_y + 1) % FRAME_HEIGHT;
  s_cx += s_vx;
  s_cy += s_vy;
  if (s_cx - BALL_R < 0 || s_cx + BALL_R >= FRAME_WIDTH)  s_vx = -s_vx;
  if (s_cy - BALL_R < 0 || s_cy + BALL_R >= FRAME_HEIGHT) s_vy = -s_vy;
  tv.drawFastHLine(0, s_line_y, FRAME_WIDTH, COL_LINE);
  tv.fillCircle(s_cx, s_cy, BALL_R, COL_BALL);
  tv.endWrite();
}

// 8 carres en haut a gauche, un par bouton (A, B, Select, Start, Haut, Bas,
// Gauche, Droite, de gauche a droite) : blancs quand le bouton est appuye.
static void draw_controller_state(uint8_t pressed) {
  tv.startWrite();
  for (int b = 0; b < 8; b++) {
    int x0 = 4 + b * (KEY_SQ + KEY_GAP);
    tv.fillRect(x0, KEYS_Y, KEY_SQ, KEY_SQ, (pressed & (1 << b)) ? COL_KEY_ON : COL_KEY_OFF);
  }
  tv.endWrite();
}

// -------------------- Portail WiFi : connexion + pages --------------------
// Meme logique hybride que wifi_boot.cpp (carte principale) : essaie la
// Station en premier, bascule en Point d'acces si ca echoue avant
// WIFI_STA_TIMEOUT_MS. Non teste sur CETTE carte (jamais essaye avec ce
// SSID de secours ni ce brochage), mais reprend un mecanisme deja
// fonctionnel ailleurs dans le projet.
static void wifi_connect_with_ap_fallback() {
  Serial.printf("[WIFI] tentative de connexion a \"%s\"...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_STA_TIMEOUT_MS) {
    delay(250);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    g_ap_mode = false;
    Serial.printf("\n[WIFI] connecte - portail sur http://%s/\n", WiFi.localIP().toString().c_str());
  } else {
    g_ap_mode = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD);
    Serial.printf("\n[WIFI] echec Station - point d'acces \"%s\" cree, portail sur http://%s/\n",
                  WIFI_AP_SSID, WiFi.softAPIP().toString().c_str());
  }
}

// Page unique : statut, norme video reglable, liste des ROMs, etat de la
// manette (reelle ou demo). Pas encore de bouton "lancer" - la ROM n'est pas
// executee dans ce prototype.
static void handleRoot() {
  String demo_note = HAS_PHYSICAL_CONTROLLER
    ? "manette NES (DB9, cavaliers JP1-JP3 en 2-3)"
    : "mode demo (pas de manette branchee) - un bouton simule toutes les 600 ms";

  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<title>NES autonome - AUTONOME-v0.4</title></head><body>"
    "<h1>Prototype carte autonome - AUTONOME-v0.4</h1>"
    "<p><b>WiFi :</b> " + String(g_ap_mode ? "point d'acces (repli)" : "connecte a la box") + "</p>"
    "<p><b>Norme video actuelle :</b> " + String(g_use_pal ? "PAL" : "NTSC") +
    " - <a href='/norme?v=pal'>passer en PAL</a> / <a href='/norme?v=ntsc'>passer en NTSC</a></p>"
    "<p><b>Manette :</b> " + demo_note + "</p>"
    "<p><b>ROMs trouvees sur la SD (" + String(rom_count) + ") :</b></p>"
    "<ul>" + g_rom_list_html + "</ul>"
    "<p><em>Pas encore de bouton pour lancer un jeu - l'emulation NES n'est pas encore portee sur cette variante (voir la feuille de route, Phase 3).</em></p>"
    "</body></html>";

  server.send(200, "text/html; charset=utf-8", html);
}

// Change la norme a chaud : LovyanGFX sait changer de resolution et de norme
// en fonctionnement (meme appel que la carte video, setResolution()).
static void handleSetNorme() {
  if (server.hasArg("v")) {
    String v = server.arg("v");
    bool new_use_pal = (v == "pal");
    if (new_use_pal != g_use_pal) {
      g_use_pal = new_use_pal;
      Serial.printf("[PORTAIL] changement de norme demande -> %s\n", g_use_pal ? "PAL" : "NTSC");
      if (g_video_ok) {
        video_apply_standard(g_use_pal);
        draw_background();
      }
    }
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== Prototype AUTONOME-v0.4 : video + SD + manette + portail WiFi ===");

  // ---- Video (LovyanGFX, couleur) ----
  video_apply_standard(g_use_pal);
  tv.setColorDepth(lgfx::color_depth_t::rgb332_1Byte);
  g_video_ok = tv.init();
  if (!g_video_ok) {
    Serial.println("[VIDEO] ECHEC d'initialisation de la sortie video (memoire ?).");
  } else {
    draw_background();
    Serial.printf("[VIDEO] pret - %dx%d couleur sur GPIO%d, norme %s\n", FRAME_WIDTH, FRAME_HEIGHT,
                  PIN_VIDEO, g_use_pal ? "PAL" : "NTSC");
  }

  // ---- Carte SD (SPI simple) ----
  SPI.begin(SD_PIN_SCK, SD_PIN_MISO, SD_PIN_MOSI, SD_PIN_CS);
  if (!SD.begin(SD_PIN_CS)) {
    Serial.println("[SD] ECHEC SD.begin() - verifier le cablage (SCK=18 MISO=19 MOSI=23 CS=5) et que la carte est bien formatee en FAT32.");
  } else {
    Serial.println("[SD] carte detectee - liste des ROMs (.nes) a la racine :");
    g_rom_list_html = "";
    File root = SD.open("/");
    if (root) {
      File entry = root.openNextFile();
      while (entry) {
        String name = entry.name();
        if (!entry.isDirectory() && name.endsWith(".nes")) {
          rom_count++;
          Serial.printf("  - %s (%lu octets)\n", entry.name(), (unsigned long)entry.size());
          g_rom_list_html += "<li>" + name + " (" + String((unsigned long)entry.size()) + " octets)</li>";
          if (first_rom_path.length() == 0) {
            first_rom_path = String("/") + name;   // premiere ROM trouvee, utilisee pour le test de chargement
          }
        }
        entry.close();
        entry = root.openNextFile();
      }
      root.close();
    }
    Serial.printf("[SD] %d ROM(s) .nes trouvee(s).\n", rom_count);
    if (g_rom_list_html.length() == 0) {
      g_rom_list_html = "<li><em>aucune ROM .nes trouvee</em></li>";
    }

    // ---- Test de CHARGEMENT (pas d'execution) ----
    // Ouvre la premiere ROM trouvee, lit son en-tete iNES pour verifier que
    // c'est bien un fichier NES valide (memes 4 octets magiques que nes_rom.c
    // cote carte principale), puis lit le fichier ENTIER dans un buffer pour
    // valider que la lecture SD complete fonctionne sans erreur - le buffer
    // est ensuite libere.
    if (first_rom_path.length() > 0) {
      File rom = SD.open(first_rom_path);
      if (!rom) {
        Serial.printf("[ROM] ECHEC ouverture de %s\n", first_rom_path.c_str());
      } else {
        uint8_t header[16];
        size_t got = rom.read(header, sizeof(header));
        if (got != sizeof(header) || memcmp(header, ROM_INES_MAGIC, 4) != 0) {
          Serial.printf("[ROM] %s : en-tete iNES invalide (pas un fichier .nes standard) - chargement complet ignore.\n", first_rom_path.c_str());
        } else {
          int prg_banks = header[4];
          int chr_banks = header[5];
          Serial.printf("[ROM] %s : en-tete iNES valide - %d banque(s) PRG (16 Ko), %d banque(s) CHR (8 Ko)\n",
                        first_rom_path.c_str(), prg_banks, chr_banks);

          size_t rom_size = rom.size();
          uint8_t *rom_buf = (uint8_t *)malloc(rom_size);
          if (!rom_buf) {
            Serial.printf("[ROM] echec malloc(%u octets) - ROM trop grosse pour la RAM disponible (pas de PSRAM sur ce module ?).\n", (unsigned)rom_size);
          } else {
            rom.seek(0);
            size_t read_total = rom.read(rom_buf, rom_size);
            if (read_total == rom_size) {
              Serial.printf("[ROM] chargement complet reussi : %u/%u octets lus en RAM.\n", (unsigned)read_total, (unsigned)rom_size);
            } else {
              Serial.printf("[ROM] lecture incomplete : %u/%u octets seulement.\n", (unsigned)read_total, (unsigned)rom_size);
            }
            free(rom_buf);   // pas d'emulation dans ce prototype
          }
        }
        rom.close();
      }
    } else {
      Serial.println("[ROM] aucune ROM .nes trouvee sur la carte - test de chargement ignore.");
    }
  }

  // ---- Manette NES ----
#if HAS_PHYSICAL_CONTROLLER
  controller_init();
  Serial.println("[MANETTE] manette NES prete (LATCH=32 CLOCK=33 DATA=27, JP1-JP3 en 2-3).");
#else
  Serial.println("[MANETTE] mode demo actif (HAS_PHYSICAL_CONTROLLER=0) - boutons simules automatiquement, aucun cablage requis.");
#endif

  // ---- Portail WiFi ----
  wifi_connect_with_ap_fallback();
  server.on("/", handleRoot);
  server.on("/norme", handleSetNorme);
  server.begin();
  Serial.println("[PORTAIL] serveur web demarre sur le port 80.");

  if (g_video_ok) draw_demo_scene();
}

void loop() {
  server.handleClient();

  if (!g_video_ok) {
    delay(1000);
    return;
  }

  draw_demo_scene();

  uint8_t pressed = controller_read();
  draw_controller_state(pressed);

  // Log uniquement sur changement d'etat, pour ne pas noyer le moniteur
  // serie (la lecture se fait a chaque image, ~25 fois/s).
  if (pressed != pad_prev) {
    for (int b = 0; b < 8; b++) {
      bool was_pressed = pad_prev & (1 << b);
      bool is_pressed  = pressed & (1 << b);
      if (was_pressed != is_pressed) {
        Serial.printf("[MANETTE] %s %s\n", PAD_NAME[b], is_pressed ? "appuye" : "relache");
      }
    }
    pad_prev = pressed;
  }

  delay(40);   // ~25 images/s
}
