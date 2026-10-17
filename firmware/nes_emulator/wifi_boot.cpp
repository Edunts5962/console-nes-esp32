// wifi_boot.cpp - connexion Wi-Fi de la console (voir wifi_boot.h).
//
// Ce fichier dessine lui-meme le badge de connexion sur l'ecran du logo :
// wifi_boot_connect() est BLOQUANTE (WiFi.begin() puis attente de
// WiFi.status()), le badge est donc anime dans la boucle d'attente. Au pire
// (box injoignable puis point d'acces), le demarrage est retarde d'environ
// WIFI_STA_TIMEOUT_MS (10 s par defaut) + quelques centaines de ms. Avec
// WIFI_START_AT_BOOT a 0 (defaut), le Wi-Fi n'est allume qu'a la demande,
// depuis Reglages.
#include "wifi_boot.h"
// Identifiants Wi-Fi : wifi_config.h (personnel, ignore par git) ou, a
// defaut, le modele wifi_config.example.h.
#if __has_include("wifi_config.h")
#include "wifi_config.h"
#else
#include "wifi_config.example.h"
#endif
#include "tft_driver.h"
#include "bluetooth_controller.h"   // Pause du Bluetooth pendant la connexion

#if ENABLE_WIFI_PORTAL
#include <WiFi.h>
#include "wifi_portal.h"
#endif

#include <string.h>
#include <stdio.h>
#include <Preferences.h>   // Mode Wi-Fi memorise (NVS "neswifi")
#include <esp_heap_caps.h> // wifi_boot_log_memory()

extern TFTDriver tft;

wifi_boot_status_t g_wifi_boot_status = WIFI_BOOT_DISABLED;
char g_wifi_boot_status_text[32] = "";

// ---- diagnostic (voir wifi_boot.h) ----
void wifi_boot_log_memory(const char *when) {
  const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  Serial.printf("[MEM] %s : memoire interne libre %u octets (plus gros bloc %u, minimum atteint %u), "
                "PSRAM libre %u Ko\n",
                when,
                (unsigned)heap_caps_get_free_size(caps),
                (unsigned)heap_caps_get_largest_free_block(caps),
                (unsigned)heap_caps_get_minimum_free_size(caps),
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

int wifi_boot_ap_clients() {
#if ENABLE_WIFI_PORTAL
  if (g_wifi_boot_status == WIFI_BOOT_AP_FALLBACK) return WiFi.softAPgetStationNum();
#endif
  return 0;
}

#if ENABLE_WIFI_PORTAL
// Journal des evenements Wi-Fi sur le port serie. Appele par la tache
// "arduino_events" du coeur (pile de 4 Ko) : messages courts, rien d'autre.
// s_sta_last_reason : derniere raison de refus de la box, lue a la fin de la
// tentative de connexion (texte "box introuvable", "mot de passe incorrect ?"...).
static volatile uint8_t s_sta_last_reason = 0;

static void wifi_event_log(arduino_event_t *e) {
  switch (e->event_id) {
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED: {
      const uint8_t *m = e->event_info.wifi_ap_staconnected.mac;
      Serial.printf("[WIFI] Point d'acces : appareil relie (%02X:%02X:%02X:%02X:%02X:%02X)\n",
                    m[0], m[1], m[2], m[3], m[4], m[5]);
      break;
    }
    case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED: {
      const esp_ip4_addr_t ip = e->event_info.wifi_ap_staipassigned.ip;
      Serial.printf("[WIFI] Point d'acces : adresse " IPSTR " donnee a l'appareil\n", IP2STR(&ip));
      break;
    }
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED: {
      const uint8_t *m = e->event_info.wifi_ap_stadisconnected.mac;
      const uint16_t r = e->event_info.wifi_ap_stadisconnected.reason;
      Serial.printf("[WIFI] Point d'acces : appareil parti (%02X:%02X:%02X:%02X:%02X:%02X, raison %u %s)\n",
                    m[0], m[1], m[2], m[3], m[4], m[5], (unsigned)r,
                    WiFi.disconnectReasonName((wifi_err_reason_t)r));
      break;
    }
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
      const uint8_t r = e->event_info.wifi_sta_disconnected.reason;
      s_sta_last_reason = r;
      Serial.printf("[WIFI] Box : deconnecte (raison %u %s)\n", (unsigned)r,
                    WiFi.disconnectReasonName((wifi_err_reason_t)r));
      break;
    }
    default:
      break;
  }
}

// Texte court pour un echec de connexion a la box (Reglages, badge du logo :
// ASCII seulement, la police du badge n'a pas d'accents). Indicatif : un
// signal tres faible peut aussi produire les codes "mot de passe".
static const char *sta_fail_text(uint8_t reason) {
  switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
      return "box introuvable";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
      return "box incompatible";
    case WIFI_REASON_AUTH_FAIL:
      return "mot de passe incorrect ?";
    // Poignee de main WPA sans reponse a temps - mauvais mot de passe, mais
    // aussi signal faible ou radio occupee.
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
      return "mot de passe ou signal ?";
    default:
      return "box injoignable";
  }
}
#endif

// ---- mode Wi-Fi (Auto / Box / Point d'acces), voir wifi_boot.h ----
#define NESWIFI_NVS_NAMESPACE "neswifi"
static int s_wifi_mode = -1;   // -1 : pas encore lu en NVS

wifi_mode_pref_t wifi_boot_get_mode() {
  if (s_wifi_mode < 0) {
    Preferences p;
    s_wifi_mode = NESWIFI_MODE_AUTO;
    if (p.begin(NESWIFI_NVS_NAMESPACE, true)) {   // lecture seule
      uint8_t m = p.getUChar("mode", NESWIFI_MODE_AUTO);
      if (m <= NESWIFI_MODE_AP) s_wifi_mode = m;
      p.end();
    }
  }
  return (wifi_mode_pref_t)s_wifi_mode;
}

void wifi_boot_set_mode(wifi_mode_pref_t mode) {
  if ((int)mode < NESWIFI_MODE_AUTO || (int)mode > NESWIFI_MODE_AP) return;
  if ((int)mode == s_wifi_mode) return;
  s_wifi_mode = mode;
  Preferences p;
  if (p.begin(NESWIFI_NVS_NAMESPACE, false)) {
    p.putUChar("mode", (uint8_t)mode);
    p.end();
  }
  static const char *names[] = {"Auto (box puis point d'acces)", "Box uniquement", "Point d'acces uniquement"};
  Serial.printf("[WIFI] Mode memorise : %s\n", names[mode]);
}

// ---- reseau de la box enregistre depuis le portail (NVS) ----
// Cles "ssid" / "pass" de l'espace "neswifi". Absentes : WIFI_SSID /
// WIFI_PASSWORD de wifi_config.h. Gardees en memoire apres la premiere
// lecture.
static bool s_cred_loaded = false;
static char s_nvs_ssid[33] = "";
static char s_nvs_pass[65] = "";

static void cred_load() {
  if (s_cred_loaded) return;
  s_cred_loaded = true;
  Preferences p;
  if (p.begin(NESWIFI_NVS_NAMESPACE, true)) {
    String a = p.getString("ssid", ""), b = p.getString("pass", "");
    snprintf(s_nvs_ssid, sizeof(s_nvs_ssid), "%s", a.c_str());
    snprintf(s_nvs_pass, sizeof(s_nvs_pass), "%s", b.c_str());
    p.end();
  }
}

const char *wifi_boot_sta_ssid() {
  cred_load();
  return s_nvs_ssid[0] ? s_nvs_ssid : WIFI_SSID;
}

static const char *sta_pass() {
  cred_load();
  return s_nvs_ssid[0] ? s_nvs_pass : WIFI_PASSWORD;
}

bool wifi_boot_sta_from_portal() {
  cred_load();
  return s_nvs_ssid[0] != '\0';
}

int wifi_boot_set_credentials(const char *ssid, const char *pass) {
  if (!ssid || !pass) return -1;
  const size_t ls = strlen(ssid), lp = strlen(pass);
  if (ls < 1 || ls > 32) return -1;
  if (lp != 0 && (lp < 8 || lp > 63)) return -2;
  Preferences p;
  if (!p.begin(NESWIFI_NVS_NAMESPACE, false)) return -3;
  const bool ok = p.putString("ssid", ssid) == ls && p.putString("pass", pass) == lp;
  p.end();
  if (!ok) return -3;
  snprintf(s_nvs_ssid, sizeof(s_nvs_ssid), "%s", ssid);
  snprintf(s_nvs_pass, sizeof(s_nvs_pass), "%s", pass);
  s_cred_loaded = true;
  Serial.printf("[WIFI] Reseau de la box enregistre depuis le portail : \"%s\"\n", ssid);
  return 0;
}

void wifi_boot_forget_credentials() {
  Preferences p;
  if (p.begin(NESWIFI_NVS_NAMESPACE, false)) {
    p.remove("ssid");
    p.remove("pass");
    p.end();
  }
  s_nvs_ssid[0] = s_nvs_pass[0] = '\0';
  s_cred_loaded = true;
  Serial.println("[WIFI] Reseau enregistre oublie : retour a celui de wifi_config.h.");
}

#if ENABLE_WIFI_PORTAL
// Bande en bas de l'ecran de demarrage. Le logo (show_magic(), osd.cpp)
// fait 280x240 (turning.h) sur un ecran de 320x240 (tft_driver.h) : cette
// bande deborde volontairement jusqu'a DISPLAY_WIDTH (320) pour couvrir
// aussi la marge noire de droite (280-320, jamais touchee par le logo),
// mais reste assez bas (y=208) pour ne jamais mordre sur le dessin du
// logo lui-meme.
static void draw_wifi_badge(const char *text, uint16_t color) {
  tft.drawFilledRect(0, 208, DISPLAY_WIDTH, 26, 0x0000);
  tft.drawString(10, 216, text, color, 0x0000, 1);
}
#endif

#if ENABLE_WIFI_PORTAL
// Demarrage du point d'acces, commun au repli (mode Auto) et au mode "point
// d'acces uniquement".
static void start_access_point(bool show_progress_badge) {
  if (WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD)) {
    g_wifi_boot_status = WIFI_BOOT_AP_FALLBACK;
    strncpy(g_wifi_boot_status_text, WIFI_AP_SSID,
            sizeof(g_wifi_boot_status_text) - 1);
    g_wifi_boot_status_text[sizeof(g_wifi_boot_status_text) - 1] = '\0';
    Serial.printf("[WIFI] Point d'acces actif - SSID \"%s\", IP %s, canal %d\n",
                  WIFI_AP_SSID, WiFi.softAPIP().toString().c_str(), (int)WiFi.channel());

    // Meme raisonnement que pour le succes Station ci-dessus : demarrage
    // automatique du serveur, le repli Point d'acces est aussi une connexion
    // WiFi valide et fonctionnelle.
    wifi_portal_start();
    wifi_boot_log_memory("point d'acces demarre");

    if (show_progress_badge) {
      char buf[40];
      snprintf(buf, sizeof(buf), "WiFi: PA %s", WIFI_AP_SSID);
      draw_wifi_badge(buf, 0x07FF);  // cyan - repli Point d'acces
    }
  } else {
    // Tres improbable (echec logiciel du softAP lui-meme, pas juste
    // "aucun client ne s'est connecte") - le mot de passe de repli fait
    // moins de 8 caracteres est la cause la plus probable, voir
    // wifi_config.h.
    // La radio est coupee aussi dans ce cas (comme pour "box injoignable") -
    // WIFI_BOOT_FAILED veut donc toujours dire "radio eteinte", ce que suppose
    // wifi_boot_disable().
    WiFi.mode(WIFI_OFF);
    g_wifi_boot_status = WIFI_BOOT_FAILED;
    snprintf(g_wifi_boot_status_text, sizeof(g_wifi_boot_status_text), "point d'acces impossible");
    Serial.println("[WIFI] Echec du Point d'acces egalement - WiFi indisponible.");
    if (show_progress_badge) draw_wifi_badge("WiFi: indisponible", 0xF800);  // rouge - echec complet
  }
}

static void do_wifi_connect_attempt(bool show_progress_badge) {
  g_wifi_boot_status = WIFI_BOOT_CONNECTING;
  const wifi_mode_pref_t mode = wifi_boot_get_mode();

  // Journal des evenements Wi-Fi (une seule inscription).
  static bool s_events_registered = false;
  if (!s_events_registered) {
    s_events_registered = true;
    WiFi.onEvent(wifi_event_log);
  }

  if (mode == NESWIFI_MODE_AP) {
    // Point d'acces choisi dans Reglages - la box n'est pas essayee.
    Serial.println("[WIFI] Mode point d'acces (choisi dans Reglages).");
    if (show_progress_badge) draw_wifi_badge("WiFi: point d'acces...", 0xFFE0);
    if (!WiFi.mode(WIFI_AP)) {   // diagnostic
      // Pilote Wi-Fi pas demarre (esp_wifi_init() refuse, le plus souvent faute de
      // memoire interne) : inutile d'attendre la connexion.
      Serial.println("[WIFI] ERREUR : demarrage du Wi-Fi refuse (memoire interne insuffisante ?).");
      wifi_boot_log_memory("echec du demarrage Wi-Fi");
    }
    start_access_point(show_progress_badge);
    if (show_progress_badge) delay(800);
    return;
  }

  Serial.printf("[WIFI] Connexion a \"%s\" (Station, %lu ms max)%s...\n",
                wifi_boot_sta_ssid(), (unsigned long)WIFI_STA_TIMEOUT_MS,
                wifi_boot_sta_from_portal() ? " - reseau enregistre depuis le portail" : "");

  if (!WiFi.mode(WIFI_STA)) {
    Serial.println("[WIFI] ERREUR : demarrage du Wi-Fi refuse (memoire interne insuffisante ?).");
    wifi_boot_log_memory("echec du demarrage Wi-Fi");
    WiFi.mode(WIFI_OFF);
    g_wifi_boot_status = WIFI_BOOT_FAILED;
    snprintf(g_wifi_boot_status_text, sizeof(g_wifi_boot_status_text), "demarrage impossible");
    if (show_progress_badge) {
      draw_wifi_badge("WiFi: demarrage impossible", 0xF800);
      delay(800);
    }
    return;
  }
  s_sta_last_reason = 0;
  WiFi.begin(wifi_boot_sta_ssid(), sta_pass());   // Reseau enregistre (portail) ou wifi_config.h

  static const char *dots[] = {".", "..", "..."};
  int dot_i = 0;
  unsigned long last_dot = 0;
  unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED &&
         (millis() - start) < (unsigned long)WIFI_STA_TIMEOUT_MS) {
    if (show_progress_badge && millis() - last_dot > 250) {
      char buf[24];
      snprintf(buf, sizeof(buf), "WiFi: connexion%s", dots[dot_i % 3]);
      draw_wifi_badge(buf, 0xFFE0);  // jaune - en cours
      Serial.print(".");
      dot_i++;
      last_dot = millis();
    }
    delay(20);
  }

  if (WiFi.status() == WL_CONNECTED) {
    g_wifi_boot_status = WIFI_BOOT_STATION_OK;
    IPAddress ip = WiFi.localIP();
    snprintf(g_wifi_boot_status_text, sizeof(g_wifi_boot_status_text),
             "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
    Serial.printf("\n[WIFI] Connecte - IP %s\n", g_wifi_boot_status_text);
    wifi_boot_log_memory("Wi-Fi box connecte");

    // Le serveur du portail demarre des que le Wi-Fi est pret. wifi_portal_loop()
    // doit ensuite etre appelee regulierement pour servir les requetes (boucles
    // des menus).
    wifi_portal_start();

    if (show_progress_badge) {
      char buf[40];
      snprintf(buf, sizeof(buf), "WiFi: %s", g_wifi_boot_status_text);
      draw_wifi_badge(buf, 0x07E0);  // vert - Station OK
      delay(800);  // le temps de lire le resultat avant le fondu de sortie
    }
    return;
  }

  // Raison du refus de la box, relevee AVANT WiFi.disconnect() (qui produit
  // son propre evenement de deconnexion).
  const char *why = sta_fail_text(s_sta_last_reason);
  Serial.printf("\n[WIFI] Derniere raison donnee par la box : %u (%s).\n", (unsigned)s_sta_last_reason,
                s_sta_last_reason ? WiFi.disconnectReasonName((wifi_err_reason_t)s_sta_last_reason)
                                  : "aucune - la box n'a pas repondu");

  if (mode == NESWIFI_MODE_BOX) {
    // Box uniquement (choisi dans Reglages) - pas de repli.
    Serial.printf("[WIFI] Echec Station (mode box uniquement) : %s - Wi-Fi coupe.\n", why);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    g_wifi_boot_status = WIFI_BOOT_FAILED;
    snprintf(g_wifi_boot_status_text, sizeof(g_wifi_boot_status_text), "%s", why);
    if (show_progress_badge) {
      char buf[40];
      snprintf(buf, sizeof(buf), "WiFi: %s", why);
      draw_wifi_badge(buf, 0xF800);
      delay(800);
    }
    return;
  }

  // Echec Station - repli en Point d'acces autonome.
  Serial.printf("[WIFI] Echec Station (%s) - bascule en Point d'acces...\n", why);
  if (show_progress_badge) draw_wifi_badge("WiFi: bascule Point d'acces...", 0xFFE0);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);

  start_access_point(show_progress_badge);
  if (show_progress_badge) delay(800);
}
#endif  // ENABLE_WIFI_PORTAL

// Redemarrage demande par le portail (mise a jour du logiciel) : le Wi-Fi est
// rallume au demarrage suivant, pour que la page du portail retrouve la
// console et affiche la nouvelle version. Memoire RTC : gardee a travers
// ESP.restart(), quelconque a la mise sous tension (d'ou le nombre magique).
#include <esp_attr.h>
#define WIFI_AFTER_RESTART_MAGIC 0x57494649u   // "WIFI"
RTC_NOINIT_ATTR static uint32_t s_rtc_wifi_after_restart;
void wifi_boot_keep_on_after_restart() { s_rtc_wifi_after_restart = WIFI_AFTER_RESTART_MAGIC; }

void wifi_boot_connect() {
#if ENABLE_WIFI_PORTAL && WIFI_START_AT_BOOT
  s_rtc_wifi_after_restart = 0;
  do_wifi_connect_attempt(/* show_progress_badge= */ true);
#elif ENABLE_WIFI_PORTAL
  if (s_rtc_wifi_after_restart == WIFI_AFTER_RESTART_MAGIC) {
    s_rtc_wifi_after_restart = 0;
    Serial.println("[WIFI] Demarrage apres une mise a jour par le portail : Wi-Fi rallume.");
    do_wifi_connect_attempt(/* show_progress_badge= */ true);
    return;
  }
  g_wifi_boot_status = WIFI_BOOT_DISABLED;
  g_wifi_boot_status_text[0] = '\0';
  Serial.println("[WIFI] Wi-Fi coupe au demarrage (WIFI_START_AT_BOOT = 0) : Reglages > Wi-Fi pour l'allumer.");
#else
  // ENABLE_WIFI_PORTAL == 0 (wifi_boot.h) : rien a faire, aucun delai
  // ajoute au demarrage.
  g_wifi_boot_status = WIFI_BOOT_DISABLED;
  g_wifi_boot_status_text[0] = '\0';
#endif
}

void wifi_boot_disable() {
#if ENABLE_WIFI_PORTAL
  if (g_wifi_boot_status == WIFI_BOOT_STATION_OK || g_wifi_boot_status == WIFI_BOOT_AP_FALLBACK) {
    Serial.println("[WIFI] Coupure du WiFi.");
    // Arrete proprement le serveur du portail AVANT de couper le WiFi (pendant
    // du demarrage automatique dans do_wifi_connect_attempt() ci-dessus) -
    // server.stop() suppose le WiFi encore actif, l'appeler apres
    // WiFi.mode(WIFI_OFF) serait hasardeux. Remet aussi s_portal_started a
    // false (wifi_portal.cpp) pour que la prochaine connexion redemarre le
    // serveur normalement.
    wifi_portal_stop();
    WiFi.mode(WIFI_OFF);
    g_wifi_boot_status = WIFI_BOOT_DISABLED;
    g_wifi_boot_status_text[0] = '\0';
  }
#endif
}

void wifi_boot_shutdown_for_game() {
  // Devenue un simple alias de wifi_boot_disable() (meme effet exact) : ce
  // nom-ci reste pour la lisibilite au point d'appel (nes_emulator.ino, juste
  // avant nofrendo_main()), voir wifi_boot.h.
  wifi_boot_disable();
}

void wifi_boot_enable() {
#if ENABLE_WIFI_PORTAL
  if (g_wifi_boot_status == WIFI_BOOT_STATION_OK || g_wifi_boot_status == WIFI_BOOT_AP_FALLBACK) {
    return;  // deja actif, rien a faire
  }
  // Le Bluetooth, s'il tourne, est ARRETE avant la connexion (radio et memoire
  // interne rendues) et le reste tant que le Wi-Fi est allume (voir
  // bluetooth_controller.h) : un simple arret de la publicite ne suffit pas.
  bluetooth_controller_stop_for_wifi();
  wifi_boot_log_memory("avant connexion Wi-Fi (Reglages)");
  do_wifi_connect_attempt(/* show_progress_badge= */ false);
  // Echec (Wi-Fi coupe) : le Bluetooth redemarre 2 s plus tard ; Wi-Fi
  // allume : il reste arrete.
  bluetooth_controller_radio_check();
#endif
}
