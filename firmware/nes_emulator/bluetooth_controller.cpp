// bluetooth_controller.cpp - manette virtuelle du telephone en Bluetooth
// basse consommation (serveur GATT, voir bluetooth_controller.h).
//
// Le telephone (page manette8.html, Web Bluetooth) ecrit l'etat de ses
// boutons et ses commandes sur FFF1 ; la console repond par des
// notifications sur FFF2 (games_link.h). La radio est partagee avec le
// Wi-Fi : le Bluetooth est arrete tant que le Wi-Fi est allume
// (bluetooth_controller_radio_check()).
#include "bluetooth_controller.h"
#include "hw_config.h"
#include "wifi_boot.h"   // g_wifi_boot_status (partage de la radio)

#if ENABLE_BLE_GAMEPAD

// Bibliotheque BLE integree au core Arduino-ESP32 (BLEDevice.h...), et NON
// la bibliotheque externe NimBLE-Arduino : avec celle-ci, la carte
// redemarrait tres tot, avant meme le premier message Serial.
// A savoir : a partir du core 3.3, cette bibliotheque integree s'appuie
// elle-meme sur la pile NimBLE d'ESP-IDF pour l'ESP32-S3 (Bluedroid avec
// les cores plus anciens). L'API et le comportement restent identiques.
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#if defined(CONFIG_NIMBLE_ENABLED)
#include <host/ble_hs.h>   // Notifications FFF2 envoyees directement a NimBLE
#endif
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

static const char *BLE_DEVICE_NAME = "NES-ESP32S3";
static const char *BLE_SERVICE_UUID = "0000fff0-0000-1000-8000-00805f9b34fb";
static const char *BLE_CHARACTERISTIC_UUID = "0000fff1-0000-1000-8000-00805f9b34fb";
// Reponses de la console au telephone (liste des jeux, evenements), en
// notifications. Voir docs/protocole_bluetooth.md.
static const char *BLE_GAMES_CHARACTERISTIC_UUID = "0000fff2-0000-1000-8000-00805f9b34fb";

// La page renvoie l'etat des touches tenues toutes les 100 ms (SEND_DELAY de
// manette8.html) et envoie 0x00 a chaque relachement. Sans trame d'etat
// pendant ce delai, on relache tous les boutons (page figee, telephone en
// veille...).
static const uint32_t BLE_INPUT_TIMEOUT_MS = 500;

// Trames de plus d'un octet (voir bluetooth_controller.h)
static const uint8_t BLE_FRAME_SPEED   = 0xFE;  // [0xFE, n]
static const uint8_t BLE_FRAME_COMMAND = 0xFD;  // [0xFD, code, 0x00]
static const uint8_t BLE_CMD_MENU      = 0x01;
// Liste des jeux et changement de jeu (voir bluetooth_controller.h)
static const uint8_t BLE_CMD_LIST      = 0x03;   // [0xFD, 0x03, 0x00]
static const uint8_t BLE_CMD_LAUNCH    = 0x04;   // [0xFD, 0x04, gen, index lo, index hi, 0x00]
static const uint8_t BLE_CMD_PAUSE     = 0x05;   // [0xFD, 0x05, 1 = pause | 0 = reprise, 0x00]
static const uint8_t BLE_CMD_TVLIST    = 0x06;   // [0xFD, 0x06, 0x00] : liste des jeux sur la television
// Liste COMPLETE (catalogue) et lancement par numero du catalogue
static const uint8_t BLE_CMD_LIST_ALL   = 0x07;  // [0xFD, 0x07, 0x00] ou [0xFD, 0x07, gen, crc lo, crc hi, 0x00]
static const uint8_t BLE_CMD_LAUNCH_ALL = 0x08;  // [0xFD, 0x08, gen, numero lo, numero hi, 0x00]
static const uint8_t BLE_CMD_FAV        = 0x09;  // [0xFD, 0x09, gen, numero lo, numero hi, 0x00] : favori (bascule)
static const uint8_t BLE_CMD_VOLUME     = 0x0A;  // [0xFD, 0x0A, volume 0..200, 0x00]
static const uint8_t BLE_CMD_RESUME_ALL = 0x0B;  // [0xFD, 0x0B, gen, numero lo, numero hi, 0x00] : reprendre ce jeu

static volatile uint8_t  g_ble_state = 0;
static volatile uint32_t g_ble_last_rx = 0;
static volatile bool     g_ble_connected = false;
static volatile bool     g_ble_menu_request = false;
static bool g_ble_initialized = false;
static BLEServer *g_ble_server = nullptr;

// Liste des jeux : caracteristique FFF2 (notifications), abonnement de la page
// (rappel onSubscribe, tache de la pile Bluetooth) et file des demandes du
// telephone (ecrite par onWrite(), lue par la tache Arduino).
struct games_request_t {
  uint8_t kind;      // BLE_GAMES_REQ_*
  uint8_t gen;
  uint16_t index;
};
static BLECharacteristic *g_games_char = nullptr;
static volatile bool g_games_subscribed = false;
static volatile uint16_t g_games_conn = 0;
static QueueHandle_t g_games_queue = nullptr;

static void games_queue_push(uint8_t kind, uint8_t gen, uint16_t index) {
  if (!g_games_queue) return;
  games_request_t r = {kind, gen, index};
  xQueueSend(g_games_queue, &r, 0);   // file pleine : demande perdue (la page la refait)
}

// Partage de la radio avec le Wi-Fi (voir bluetooth_controller_radio_check()) :
//  - s_adv_hold : arret du Bluetooth en cours : les rappels de connexion
//    (tache de la pile Bluetooth, d'ou volatile) refusent un telephone et ne
//    relancent pas la publicite ;
//  - s_deferred : le Wi-Fi a deja fait attendre (ou arrete) le Bluetooth ;
//  - s_free_since : depuis quand le Wi-Fi est coupe, apres une attente due
//    au Wi-Fi (0 = pas en cours).
static volatile bool s_adv_hold = false;
static bool s_deferred = false;
static uint32_t s_free_since = 0;
// s_init_failed : le dernier demarrage a echoue (nouvel essai au debut de la
// partie seulement) ; s_retry_done : ce nouvel essai a eu lieu ; s_stops :
// arrets pour le Wi-Fi depuis la mise sous tension (journal).
static bool s_init_failed = false;
static bool s_retry_done = false;
static uint16_t s_stops = 0;

// Apres une attente due au Wi-Fi, le Bluetooth ne demarre dans les menus que
// si le Wi-Fi reste coupe ce temps-la (voir
// bluetooth_controller_radio_check()) : un changement de mode coupe le Wi-Fi
// puis le rallume 0,3 s plus tard.
static const uint32_t BLE_START_AFTER_WIFI_OFF_MS = 2000;

class NesServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *server) override {
    g_ble_connected = true;
    g_ble_state = 0;
    g_ble_last_rx = millis();
    Serial.println("[BLE] Smartphone connecte");
    // Connexion arrivee pendant l'arret du Bluetooth (Wi-Fi allume au meme
    // moment) : refusee tout de suite.
    if (s_adv_hold && server) {
      Serial.println("[BLE] Arret du Bluetooth en cours (Wi-Fi) : connexion refusee.");
      server->disconnect(server->getConnId());
    }
  }

  void onDisconnect(BLEServer *server) override {
    g_ble_connected = false;
    g_ble_state = 0;
    g_ble_last_rx = 0;
    Serial.println("[BLE] Smartphone deconnecte");
    g_games_subscribed = false;   // La page se reabonne a la reconnexion

    // Relance la publicite pour permettre une nouvelle connexion - sauf
    // pendant l'arret du Bluetooth pour le Wi-Fi.
    if (server && !s_adv_hold) {
      delay(10);
      if (!s_adv_hold) server->startAdvertising();   // Relu apres le delai
    }
  }
};

// Abonnement de la page aux notifications FFF2.
class NesGamesCallbacks : public BLECharacteristicCallbacks {
#if defined(CONFIG_NIMBLE_ENABLED)
  void onSubscribe(BLECharacteristic *characteristic, ble_gap_conn_desc *desc, uint16_t subValue) override {
    (void)characteristic;
    if (desc) g_games_conn = desc->conn_handle;
    g_games_subscribed = (subValue != 0);
  }
#endif
};

class NesCommandCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) override {
    if (!characteristic) return;

    String value = characteristic->getValue();
    const size_t len = value.length();
    if (len == 0) return;
    const uint8_t first = static_cast<uint8_t>(value[0]);

    // [0xFE, n] : curseur "vitesse" de la page (envoye a la connexion et a
    // chaque deplacement du curseur). L'emulateur n'en a pas l'usage.
    if (len >= 2 && first == BLE_FRAME_SPEED) {
      return;
    }

    // [0xFD, code, 0x00] : commande systeme. Pas de Serial ici (rappel de la
    // pile BLE, a garder court) : le message est ecrit par celui qui traite la
    // demande (osd.cpp ou ui_lvgl.cpp). Code 0x02 : controle de liaison toutes
    // les secondes, rien a faire - l'accuse de reception envoye par la pile
    // BLE allume le voyant vert de la page.
    if (len >= 2 && first == BLE_FRAME_COMMAND) {
      const uint8_t code = static_cast<uint8_t>(value[1]);
      if (code == BLE_CMD_MENU) {
        g_ble_state = 0;             // relache tout avant de quitter
        g_ble_menu_request = true;
      } else if (code == BLE_CMD_LIST) {
        games_queue_push(BLE_GAMES_REQ_LIST, 0, 0);
      } else if (code == BLE_CMD_LAUNCH && len >= 5) {
        g_ble_state = 0;
        games_queue_push(BLE_GAMES_REQ_LAUNCH, static_cast<uint8_t>(value[2]),
                         static_cast<uint16_t>(static_cast<uint8_t>(value[3]) |
                                               (static_cast<uint8_t>(value[4]) << 8)));
      } else if (code == BLE_CMD_PAUSE && len >= 3) {
        g_ble_state = 0;
        games_queue_push(static_cast<uint8_t>(value[2]) ? BLE_GAMES_REQ_PAUSE : BLE_GAMES_REQ_RESUME, 0, 0);
      } else if (code == BLE_CMD_TVLIST) {
        g_ble_state = 0;
        games_queue_push(BLE_GAMES_REQ_TVLIST, 0, 0);
      } else if (code == BLE_CMD_LIST_ALL) {
        if (len >= 5) {   // le telephone a deja une liste : numero + CRC
          games_queue_push(BLE_GAMES_REQ_LIST_ALL_HAVE, static_cast<uint8_t>(value[2]),
                           static_cast<uint16_t>(static_cast<uint8_t>(value[3]) |
                                                 (static_cast<uint8_t>(value[4]) << 8)));
        } else {
          games_queue_push(BLE_GAMES_REQ_LIST_ALL, 0, 0);
        }
      } else if ((code == BLE_CMD_LAUNCH_ALL || code == BLE_CMD_RESUME_ALL || code == BLE_CMD_FAV) && len >= 5) {
        if (code != BLE_CMD_FAV) g_ble_state = 0;
        games_queue_push(code == BLE_CMD_FAV ? BLE_GAMES_REQ_FAV : code == BLE_CMD_RESUME_ALL ? BLE_GAMES_REQ_RESUME_ALL
                                                                                                : BLE_GAMES_REQ_LAUNCH_ALL,
                         static_cast<uint8_t>(value[2]),
                         static_cast<uint16_t>(static_cast<uint8_t>(value[3]) |
                                               (static_cast<uint8_t>(value[4]) << 8)));
      } else if (code == BLE_CMD_VOLUME && len >= 3) {
        games_queue_push(BLE_GAMES_REQ_VOLUME, 0, static_cast<uint8_t>(value[2]));
      }
      return;
    }

    // Etat de la manette : un octet = etat complet de la manette NES. Si
    // plusieurs octets arrivent ensemble (autre format), on garde le dernier.
    g_ble_state = static_cast<uint8_t>(value[len - 1]);
    g_ble_last_rx = millis();
  }
};

// Rappels en objets statiques : le Bluetooth peut s'arreter et redemarrer
// (Wi-Fi allume puis coupe) ; un "new" a chaque demarrage perdrait un peu de
// memoire interne a chaque fois.
static NesServerCallbacks s_server_cb;
static NesCommandCallbacks s_command_cb;
static NesGamesCallbacks s_games_cb;

void bluetooth_controller_init() {
  if (g_ble_initialized) {
    Serial.println("[BLE] Deja initialise.");
    return;
  }

  Serial.println("[BLE] Initialisation du serveur GATT...");
  Serial.flush();

  // Initialisation BLE unique et explicite.
  // Resultat verifie - sans pile Bluetooth, la creation du serveur ci-dessous
  // planterait.
  if (!BLEDevice::init(BLE_DEVICE_NAME)) {
    Serial.println("[BLE] ERREUR : la pile Bluetooth n'a pas demarre (memoire interne insuffisante ?).");
    wifi_boot_log_memory("echec du demarrage Bluetooth");
    return;
  }

  BLEServer *server = BLEDevice::createServer();
  if (!server) {
    Serial.println("[BLE] ERREUR : creation du serveur impossible.");
    return;
  }

  server->setCallbacks(&s_server_cb);

  BLEService *service = server->createService(BLE_SERVICE_UUID);
  if (!service) {
    Serial.println("[BLE] ERREUR : creation du service impossible.");
    return;
  }

  BLECharacteristic *characteristic = service->createCharacteristic(
      BLE_CHARACTERISTIC_UUID,
      BLECharacteristic::PROPERTY_WRITE |
      BLECharacteristic::PROPERTY_WRITE_NR);

  if (!characteristic) {
    Serial.println("[BLE] ERREUR : creation de la caracteristique impossible.");
    return;
  }

  characteristic->setCallbacks(&s_command_cb);

  // Reponses de la console (liste des jeux, evenements). Sans elle (creation
  // impossible), la manette marche, sans liste.
  g_games_char = service->createCharacteristic(BLE_GAMES_CHARACTERISTIC_UUID,
                                               BLECharacteristic::PROPERTY_NOTIFY);
  if (g_games_char) {
    g_games_char->setCallbacks(&s_games_cb);
  } else {
    Serial.println("[BLE] Caracteristique FFF2 impossible : pas de liste des jeux sur le telephone.");
  }
  if (!g_games_queue) g_games_queue = xQueueCreate(8, sizeof(games_request_t));

  service->start();

  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(BLE_SERVICE_UUID);
  advertising->setScanResponse(true);
  // Intervalle de connexion prefere annonce au telephone : 0x06 x 1,25 ms
  // = 7,5 ms mini, 0x12 x 1,25 ms = 22,5 ms maxi (l'exemple Arduino d'origine
  // appelait deux fois setMinPreferred(), la 2e valeur ecrasant la 1re).
  advertising->setMinPreferred(0x06);
  advertising->setMaxPreferred(0x12);

  // Etat remis a zero AVANT la publicite (un telephone qui se connecterait
  // aussitot ne serait pas compte deconnecte).
  g_ble_state = 0;
  g_ble_last_rx = 0;
  g_ble_connected = false;
  g_ble_menu_request = false;
  g_ble_server = server;
  g_ble_initialized = true;

  BLEDevice::startAdvertising();

  Serial.println("[BLE] Serveur GATT pret");
  Serial.printf("[BLE] Nom : %s\n", BLE_DEVICE_NAME);
  Serial.printf("[BLE] Service : %s\n", BLE_SERVICE_UUID);
  Serial.printf("[BLE] Commandes : %s\n", BLE_CHARACTERISTIC_UUID);
  Serial.println("[BLE] Write / Write Without Response");
  if (g_games_char) Serial.printf("[BLE] Liste des jeux : %s (notifications)\n", BLE_GAMES_CHARACTERISTIC_UUID);
  Serial.println("[BLE] En attente du smartphone...");
  wifi_boot_log_memory("Bluetooth demarre");   // Memoire interne restante
}

uint8_t bluetooth_controller_get_state() {
  if (!g_ble_connected) return 0;

  uint32_t last = g_ble_last_rx;
  if (last == 0 || (uint32_t)(millis() - last) > BLE_INPUT_TIMEOUT_MS) {
    return 0;
  }

  return g_ble_state;
}

bool bluetooth_controller_is_connected() {
  return g_ble_connected;
}

bool bluetooth_controller_take_menu_request() {
  if (!g_ble_menu_request) return false;
  g_ble_menu_request = false;
  return true;
}

void bluetooth_controller_disconnect_before_restart() {
  if (!g_ble_initialized || !g_ble_server || !g_ble_connected) return;
  Serial.println("[BLE] Deconnexion du smartphone avant redemarrage");
  g_ble_server->disconnect(g_ble_server->getConnId());
  // Laisse partir la demande de deconnexion (0,3 s au plus) : le telephone
  // la recoit tout de suite au lieu d'attendre plusieurs secondes la perte
  // de liaison.
  for (int i = 0; i < 30 && g_ble_connected; i++) delay(10);
}

// ---- liste des jeux et changement de jeu (voir bluetooth_controller.h) ----
int bluetooth_controller_take_games_request(uint8_t *gen, uint16_t *index) {
  if (!g_games_queue) return BLE_GAMES_REQ_NONE;
  games_request_t r;
  if (xQueueReceive(g_games_queue, &r, 0) != pdTRUE) return BLE_GAMES_REQ_NONE;
  if (gen) *gen = r.gen;
  if (index) *index = r.index;
  return r.kind;
}

void bluetooth_controller_flush_games_launches() {
  if (!g_games_queue) return;
  // Un tour de file : chaque demande est reprise puis remise au bout, sauf
  // les lancements. Une demande arrivee pendant ce tour est simplement
  // traitee plus tard (elle est derriere celles deja vues).
  UBaseType_t n = uxQueueMessagesWaiting(g_games_queue);
  while (n--) {
    games_request_t r;
    if (xQueueReceive(g_games_queue, &r, 0) != pdTRUE) break;
    if (r.kind != BLE_GAMES_REQ_LAUNCH && r.kind != BLE_GAMES_REQ_LAUNCH_ALL && r.kind != BLE_GAMES_REQ_RESUME_ALL)
      xQueueSend(g_games_queue, &r, 0);
  }
}

bool bluetooth_controller_games_ready() {
  return g_ble_initialized && g_games_char && g_ble_connected && g_games_subscribed;
}

size_t bluetooth_controller_max_message() {
#if defined(CONFIG_NIMBLE_ENABLED)
  if (bluetooth_controller_games_ready()) {
    uint16_t mtu = ble_att_mtu(g_games_conn);
    if (mtu > 23) return (size_t)(mtu - 3);
  }
#endif
  return 20;
}

bool bluetooth_controller_send(const uint8_t *data, size_t len) {
  if (!bluetooth_controller_games_ready() || !data || len == 0) return false;
  if (len > bluetooth_controller_max_message()) return false;
#if defined(CONFIG_NIMBLE_ENABLED)
  // Appel direct a NimBLE (sans la copie String de BLECharacteristic::
  // notify()) : echoue sans rien bloquer quand ses tampons sont pleins -
  // l'appelant renvoie le meme message un peu plus tard.
  struct os_mbuf *om = ble_hs_mbuf_from_flat(data, (uint16_t)len);
  if (!om) return false;
  return ble_gatts_notify_custom(g_games_conn, g_games_char->getHandle(), om) == 0;   // om toujours consomme
#else
  g_games_char->setValue(const_cast<uint8_t *>(data), len);
  g_games_char->notify();
  return true;
#endif
}

// Avant l'arret : publicite arretee et telephone deconnecte, en attendant la
// fin de la deconnexion (au plus wait_ms) - la page recoit la deconnexion tout
// de suite et tente de se reconnecter.
static void ble_enforce_hold(uint32_t wait_ms) {
  if (!BLEDevice::getInitialized()) return;   // pile pas demarree : rien a arreter
#if defined(CONFIG_NIMBLE_ENABLED)
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  if (adv && adv->isAdvertising()) BLEDevice::stopAdvertising();
#else
  BLEDevice::stopAdvertising();
#endif
  if (g_ble_connected && g_ble_server) {
    Serial.println("[BLE] Telephone deconnecte (Wi-Fi).");
    g_ble_server->disconnect(g_ble_server->getConnId());
    // g_ble_connected repasse a faux dans onDisconnect().
    const uint32_t t0 = millis();
    while (g_ble_connected && (uint32_t)(millis() - t0) < wait_ms) delay(10);
    if (g_ble_connected) Serial.println("[BLE] Telephone toujours relie : arret du Bluetooth quand meme.");
  }
}

// Arret complet du Bluetooth pour le Wi-Fi : publicite arretee, telephone
// deconnecte, puis pile NimBLE et controleur arretes
// (BLEDevice::deinit(false) : memoire interne rendue, redemarrage possible -
// "true" l'interdirait). bluetooth_controller_init() le redemarre a la coupure
// du Wi-Fi.
static void ble_stop_for_wifi() {
  // Pile demarree, meme si le serveur n'a pas pu etre cree (demarrage a
  // moitie rate) : sa memoire est rendue aussi.
  if (!g_ble_initialized && !BLEDevice::getInitialized()) return;
  s_stops++;
  Serial.printf("[BLE] Wi-Fi allume : Bluetooth ARRETE (radio et memoire interne pour le Wi-Fi, "
                "arret %u). Il redemarre a la coupure du Wi-Fi ou au lancement d'un jeu.\n",
                (unsigned)s_stops);
  wifi_boot_log_memory("avant l'arret du Bluetooth");
  s_adv_hold = true;           // onDisconnect() ne relance pas la publicite
  ble_enforce_hold(600);       // publicite arretee, telephone deconnecte
  // Plus aucun envoi vers le telephone a partir d'ici.
  g_ble_initialized = false;
  g_games_subscribed = false;
  g_ble_server = nullptr;      // detruit par BLEDevice::deinit()
  g_games_char = nullptr;      // idem (service et caracteristiques)
  if (g_games_queue) xQueueReset(g_games_queue);   // demandes du telephone perimees
  BLEDevice::deinit(false);
  g_ble_connected = false;
  g_ble_state = 0;
  g_ble_last_rx = 0;
  g_ble_menu_request = false;
  s_adv_hold = false;
  s_deferred = true;           // redemarrage BLE_START_AFTER_WIFI_OFF_MS apres la coupure
  s_free_since = 0;
  wifi_boot_log_memory("Bluetooth arrete");
}

// Voir bluetooth_controller.h. Appelee depuis la tache Arduino (fin du logo,
// boucles des menus, debut de partie, wifi_boot_enable()), jamais depuis un
// rappel de la pile Bluetooth.
//
// Le Bluetooth garde 40 a 50 Ko de memoire interne meme en pause (il en reste
// environ 75 Ko libres une fois demarre) : la connexion a la box (securite
// WPA, TCP de la page du portail) n'a alors plus de quoi tourner et la carte
// peut redemarrer a l'allumage du Wi-Fi. Le Bluetooth est donc completement
// ARRETE des que le Wi-Fi est allume, et redemarre a sa coupure ou au
// lancement d'un jeu.
void bluetooth_controller_radio_check(bool game_start) {
  // Wi-Fi allume : box connectee, point d'acces actif, ou tentative pas
  // encore terminee.
  const bool wifi_on = (g_wifi_boot_status == WIFI_BOOT_AP_FALLBACK ||
                        g_wifi_boot_status == WIFI_BOOT_STATION_OK ||
                        g_wifi_boot_status == WIFI_BOOT_CONNECTING);

  if (g_ble_initialized || BLEDevice::getInitialized()) {
    if (wifi_on) ble_stop_for_wifi();
    return;
  }

  // Bluetooth arrete ou pas encore demarre.
  if (wifi_on) {
    s_free_since = 0;
    if (!s_deferred) {
      s_deferred = true;
      Serial.println("[BLE] Demarrage differe : le Wi-Fi est allume. Le Bluetooth demarrera a sa "
                     "coupure (Reglages ou debut de partie).");
    }
    s_retry_done = false;   // la memoire aura change : un echec pourra etre retente
    return;
  }
  if (s_init_failed) {
    // Echec deja signale : un seul nouvel essai, au debut de la partie
    // (Wi-Fi coupe, sa memoire rendue), si la pile n'a pas demarre.
    if (!game_start || s_retry_done || BLEDevice::getInitialized()) return;
    s_retry_done = true;
    Serial.println("[BLE] Nouvel essai de demarrage, Wi-Fi coupe.");
  } else if (s_deferred && !game_start) {
    // Le Wi-Fi a fait attendre (ou arrete) le Bluetooth et vient d'etre
    // coupe depuis les Reglages : demarrage s'il reste coupe
    // BLE_START_AFTER_WIFI_OFF_MS (changement de mode : coupe puis rallume).
    if (s_free_since == 0) {
      s_free_since = millis() | 1;
      return;
    }
    if ((uint32_t)(millis() - s_free_since) < BLE_START_AFTER_WIFI_OFF_MS) return;
  }
  if (s_stops) Serial.println("[BLE] Wi-Fi coupe : redemarrage du Bluetooth (la page du telephone "
                              "se reconnecte toute seule).");
  bluetooth_controller_init();
  s_init_failed = !g_ble_initialized;
  s_free_since = 0;
}

// Voir bluetooth_controller.h.
void bluetooth_controller_stop_for_wifi() {
  ble_stop_for_wifi();
}

bool bluetooth_controller_waiting_for_wifi() {
  // Arrete ou pas encore demarre (et pas d'echec) : il attend la coupure du
  // Wi-Fi ; ou en cours d'arret.
  return (!g_ble_initialized && !s_init_failed) || s_adv_hold;
}

#else  // ENABLE_BLE_GAMEPAD == 0 : pile BLE ni compilee ni demarree

void bluetooth_controller_init() {
  Serial.println("[BLE] Manette du telephone desactivee (ENABLE_BLE_GAMEPAD = 0).");
}
uint8_t bluetooth_controller_get_state() { return 0; }
bool bluetooth_controller_is_connected() { return false; }
bool bluetooth_controller_take_menu_request() { return false; }
void bluetooth_controller_disconnect_before_restart() {}
int bluetooth_controller_take_games_request(uint8_t *, uint16_t *) { return BLE_GAMES_REQ_NONE; }
void bluetooth_controller_flush_games_launches() {}
bool bluetooth_controller_games_ready() { return false; }
size_t bluetooth_controller_max_message() { return 20; }
bool bluetooth_controller_send(const uint8_t *, size_t) { return false; }
void bluetooth_controller_radio_check(bool) {
  // Le message ci-dessus est ecrit une fois, a la fin du logo.
  static bool said = false;
  if (!said) {
    said = true;
    bluetooth_controller_init();
  }
}
bool bluetooth_controller_waiting_for_wifi() { return false; }
void bluetooth_controller_stop_for_wifi() {}

#endif  // ENABLE_BLE_GAMEPAD
