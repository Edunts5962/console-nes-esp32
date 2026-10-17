// games_link.cpp - liste des jeux et changement de jeu depuis la manette du
// telephone. Format des messages : voir games_link.h.
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <stdio.h>
#include "games_link.h"
#include "bluetooth_controller.h"
#include "osd_menu_api.h"

// ---------------------------------------------------------------------------
// Evenements en attente d'envoi (petite file : le plus ancien est perdu si
// elle deborde). Gardes seulement quand la page est la pour les recevoir.
// ---------------------------------------------------------------------------
#define EV_MAX 6
#define EV_LEN 13   // ETAT de 12 octets (8 avant) ;
static uint8_t s_ev[EV_MAX][EV_LEN];
static uint8_t s_ev_len[EV_MAX];
static int s_ev_head = 0;
static int s_ev_count = 0;

static void ev_push(const uint8_t *m, uint8_t len) {
  if (!bluetooth_controller_games_ready() || len > EV_LEN) return;
  if (s_ev_count == EV_MAX) {   // file pleine : le plus ancien saute
    s_ev_head = (s_ev_head + 1) % EV_MAX;
    s_ev_count--;
  }
  const int slot = (s_ev_head + s_ev_count) % EV_MAX;
  memcpy(s_ev[slot], m, len);
  s_ev_len[slot] = len;
  s_ev_count++;
}

// Vrai si tout est parti.
static bool ev_flush() {
  while (s_ev_count > 0) {
    if (!bluetooth_controller_send(s_ev[s_ev_head], s_ev_len[s_ev_head])) return false;
    s_ev_head = (s_ev_head + 1) % EV_MAX;
    s_ev_count--;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Envoi de la liste : DEBUT, morceaux, FIN (CRC). Texte construit a la
// demande en PSRAM, libere des la fin de l'envoi.
// Deux listes - la PAGE affichee (messages 0x02-0x04, 50 jeux au plus) ou le
// CATALOGUE complet (0x06-0x08, 1000 jeux au plus : longueur et positions sur
// 3 octets, nombre sur 2).
// ---------------------------------------------------------------------------
#define TX_IDLE  0
#define TX_BEGIN 1
#define TX_DATA  2
#define TX_END   3
static struct {
  uint8_t *buf;
  uint32_t len;
  uint32_t off;
  uint16_t crc;
  uint8_t gen;
  uint16_t count;
  uint8_t phase;
  bool all;         // Catalogue complet
  bool unchanged;   // Le telephone a deja ce catalogue (rien a renvoyer)
  uint32_t last_ok; // dernier message parti (millis)
} s_tx = {NULL, 0, 0, 0, 0, 0, TX_IDLE, false, false, 0};

// Plus rien ne part depuis ce delai : envoi abandonne (la page redemande).
static const uint32_t TX_STALL_MS = 3000;
// Messages au plus par appel de games_link_poll().
static const int TX_BURST = 6;

static uint16_t crc16_ccitt(const uint8_t *d, size_t n) {
  uint16_t crc = 0xFFFF;
  while (n--) {
    crc ^= (uint16_t)(*d++) << 8;
    for (int i = 0; i < 8; i++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

static void tx_abort() {
  if (s_tx.buf) free(s_tx.buf);   // heap_caps_malloc() se libere avec free()
  s_tx.buf = NULL;
  s_tx.phase = TX_IDLE;
}

// all : catalogue complet, sinon page affichee. have : le telephone a deja le
// catalogue have_gen / have_crc.
static void tx_start(bool all, bool have, uint8_t have_gen, uint16_t have_crc) {
  tx_abort();
  int n = all ? osd_catalog_count() : osd_games_count();
  if (n < 0) n = 0;
  if (!all && n > MAX_GAMES) n = MAX_GAMES;
  if (all && n > MAX_GAMES_TOTAL) n = MAX_GAMES_TOTAL;
  // Par jeu : drapeaux (2) + tab + Ko (7) + tab + nom (63) + fin de ligne.
  const size_t cap = (size_t)n * 76 + 1;
  uint8_t *b = (uint8_t *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!b) b = (uint8_t *)malloc(cap);
  if (!b) {
    Serial.println("[TEL] Liste des jeux : pas de memoire pour la preparer.");
    return;
  }
  size_t len = 0;
  for (int i = 0; i < n; i++) {
    const uint32_t size = all ? osd_catalog_size(i) : osd_game_size(i);
    const int where = all ? osd_catalog_where(i) : osd_game_where(i);
    const bool bad = all ? osd_catalog_is_bad(i) : osd_game_is_bad(i);
    const char *name = all ? osd_catalog_filename(i) : osd_game_filename(i);
    const unsigned long kb = (unsigned long)((size + 1023) / 1024);
    const int w = snprintf((char *)b + len, cap - len, "%c%s\t%lu\t%s\n",
                           where == OSD_WHERE_FLASH ? 'F' : 'S', bad ? "!" : "", kb, name);
    if (w < 0 || (size_t)w >= cap - len) { n = i; break; }
    len += (size_t)w;
  }
  s_tx.buf = b;
  s_tx.len = (uint32_t)len;
  s_tx.off = 0;
  s_tx.crc = crc16_ccitt(b, len);
  s_tx.gen = all ? osd_catalog_generation() : osd_games_generation();
  s_tx.count = (uint16_t)n;
  s_tx.all = all;
  s_tx.unchanged = all && have && have_gen == s_tx.gen && have_crc == s_tx.crc;
  s_tx.phase = TX_BEGIN;
  s_tx.last_ok = millis();
  if (s_tx.unchanged)
    Serial.printf("[TEL] Liste complete demandee : %d jeu(x), deja sur le telephone (rien a renvoyer).\n", n);
  else
    Serial.printf("[TEL] Liste %s demandee : %d jeu(x), %u octets.\n", all ? "complete" : "de la page", n,
                  (unsigned)len);
}

static void tx_step() {
  if (s_tx.phase == TX_IDLE) return;
  if ((uint32_t)(millis() - s_tx.last_ok) > TX_STALL_MS) {
    Serial.println("[TEL] Liste des jeux : envoi bloque, abandonne.");
    tx_abort();
    return;
  }
  uint8_t m[256];
  for (int k = 0; k < TX_BURST && s_tx.phase != TX_IDLE; k++) {
    size_t ml = 0;
    size_t chunk = 0;
    if (s_tx.phase == TX_BEGIN) {
      if (s_tx.all) {
        m[0] = 0x06; m[1] = s_tx.gen;
        m[2] = (uint8_t)(s_tx.count & 0xFF); m[3] = (uint8_t)(s_tx.count >> 8);
        m[4] = (uint8_t)(s_tx.len & 0xFF); m[5] = (uint8_t)((s_tx.len >> 8) & 0xFF);
        m[6] = (uint8_t)((s_tx.len >> 16) & 0xFF);
        m[7] = s_tx.unchanged ? 0x01 : 0x00;
        ml = 8;
      } else {
        m[0] = 0x02; m[1] = s_tx.gen; m[2] = (uint8_t)s_tx.count;
        m[3] = (uint8_t)(s_tx.len & 0xFF); m[4] = (uint8_t)((s_tx.len >> 8) & 0xFF);
        ml = 5;
      }
    } else if (s_tx.phase == TX_DATA) {
      const size_t head = s_tx.all ? 5 : 4;
      size_t room = bluetooth_controller_max_message();
      if (room > sizeof(m)) room = sizeof(m);
      room -= head;
      chunk = s_tx.len - s_tx.off;
      if (chunk > room) chunk = room;
      m[1] = s_tx.gen;
      m[2] = (uint8_t)(s_tx.off & 0xFF); m[3] = (uint8_t)((s_tx.off >> 8) & 0xFF);
      if (s_tx.all) {
        m[0] = 0x07;
        m[4] = (uint8_t)((s_tx.off >> 16) & 0xFF);
      } else {
        m[0] = 0x03;
      }
      memcpy(m + head, s_tx.buf + s_tx.off, chunk);
      ml = head + chunk;
    } else {
      m[0] = s_tx.all ? 0x08 : 0x04; m[1] = s_tx.gen;
      m[2] = (uint8_t)(s_tx.crc & 0xFF); m[3] = (uint8_t)(s_tx.crc >> 8);
      ml = 4;
    }
    if (!bluetooth_controller_send(m, ml)) return;   // tampons pleins : la suite au prochain appel
    s_tx.last_ok = millis();
    if (s_tx.phase == TX_BEGIN) {
      s_tx.phase = (s_tx.len && !s_tx.unchanged) ? TX_DATA : TX_END;
    } else if (s_tx.phase == TX_DATA) {
      s_tx.off += (uint32_t)chunk;
      if (s_tx.off >= s_tx.len) s_tx.phase = TX_END;
    } else {
      tx_abort();   // termine
    }
  }
}

// ---------------------------------------------------------------------------
// Bibliotheque (favoris, recents, reprise) - message 0x09, voir games_link.h.
// Envoyee a une page qui a demande la liste complete, apres celle-ci, puis a
// chaque changement (favori ajoute, jeu lance...).
// ---------------------------------------------------------------------------
static bool s_lib_want = false;   // la page a demande la liste complete
static uint32_t s_lib_sent = 0;       // empreinte envoyee (0 : rien)
static int s_lib_phase = -1;          // -1 : rien en cours ; 0 favoris, 1 recents, 2 reprise
static int s_lib_off = 0;
static int16_t s_lib_fav[64], s_lib_rec[16];
static int s_lib_nfav = 0, s_lib_nrec = 0;
static int s_lib_resume = -1;
static bool s_lib_resume_state = false;
static bool s_launch_resume = false;  // dernier LANCEMENT : reprise demandee

bool games_link_launch_resume() { return s_launch_resume; }

static uint32_t lib_signature() {
  return (osd_library_generation() * 2654435761u) ^ ((uint32_t)osd_catalog_generation() << 1) ^ 1u;
}

static void lib_prepare() {
  s_lib_nfav = 0;
  for (int ci = 0; ci < osd_catalog_count() && s_lib_nfav < 64; ci++)
    if (osd_catalog_is_fav(ci)) s_lib_fav[s_lib_nfav++] = (int16_t)ci;
  s_lib_nrec = 0;
  for (int i = 0; i < osd_recent_count() && s_lib_nrec < 16; i++) {
    const int ci = osd_recent_catalog(i);
    if (ci >= 0) s_lib_rec[s_lib_nrec++] = (int16_t)ci;
  }
  s_lib_resume = osd_resume_catalog(&s_lib_resume_state);
  s_lib_phase = 0;
  s_lib_off = 0;
}

// Vrai quand plus rien n'est a envoyer.
static bool lib_step() {
  if (s_lib_phase < 0) {
    if (!s_lib_want || lib_signature() == s_lib_sent) return true;
    s_lib_sent = lib_signature();
    lib_prepare();
  }
  uint8_t m[128];
  size_t room = bluetooth_controller_max_message();
  if (room > sizeof(m)) room = sizeof(m);
  const uint8_t gen = osd_catalog_generation();
  while (s_lib_phase >= 0) {
    size_t ml;
    if (s_lib_phase < 2) {
      const int16_t *v = s_lib_phase == 0 ? s_lib_fav : s_lib_rec;
      const int n = s_lib_phase == 0 ? s_lib_nfav : s_lib_nrec;
      m[0] = 0x09; m[1] = gen; m[2] = (uint8_t)s_lib_phase; m[3] = (uint8_t)n; m[4] = (uint8_t)s_lib_off;
      ml = 5;
      int k = s_lib_off;
      while (k < n && ml + 2 <= room) {
        m[ml++] = (uint8_t)(v[k] & 0xFF);
        m[ml++] = (uint8_t)((uint16_t)v[k] >> 8);
        k++;
      }
      if (!bluetooth_controller_send(m, ml)) return false;
      s_lib_off = k;
      if (s_lib_off >= n) { s_lib_phase++; s_lib_off = 0; }
    } else {
      const uint16_t r = s_lib_resume < 0 ? 0xFFFF : (uint16_t)s_lib_resume;
      m[0] = 0x09; m[1] = gen; m[2] = 2; m[3] = s_lib_resume_state ? 0x01 : 0x00;
      m[4] = (uint8_t)(r & 0xFF); m[5] = (uint8_t)(r >> 8);
      if (!bluetooth_controller_send(m, 6)) return false;
      s_lib_phase = -1;
      Serial.printf("[TEL] Bibliotheque envoyee : %d favori(s), %d recent(s).\n", s_lib_nfav, s_lib_nrec);
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Etat signale au telephone (a chaque changement, et avant chaque liste).
// ---------------------------------------------------------------------------
static int s_last_state = -1;
static int s_last_current = -2;
static int s_last_gen = -1;
static int s_last_count = -1;
static int s_last_flags = -1;
static int s_last_catcur = -2;
static int s_last_catgen = -1;
static int s_last_total = -1;
static int s_last_volume = -1;

int games_link_poll(int state, int current, int *index) {
  if (!bluetooth_controller_games_ready()) {
    // Page absente : rien a envoyer ; l'etat sera renvoye a son retour.
    if (s_tx.phase != TX_IDLE) tx_abort();
    s_ev_count = 0;
    s_last_state = -1;
    s_lib_want = false;   // La bibliotheque repartira avec la prochaine liste
    s_lib_sent = 0;
    s_lib_phase = -1;
  } else {
    const int gen = osd_games_generation();
    const int count = osd_games_count();
    const int flags = osd_tv_list_available() ? 0x01 : 0x00;
    // Jeu en cours (numero du catalogue), numero et taille du catalogue
    const int catcur = (state == GAMES_LINK_STATE_MENU) ? -1 : osd_games_running_catalog();
    const int catgen = osd_catalog_generation();
    const int total = osd_catalog_count();
    const int volume = osd_get_volume();
    if (state != s_last_state || current != s_last_current || gen != s_last_gen || count != s_last_count ||
        flags != s_last_flags || catcur != s_last_catcur || catgen != s_last_catgen || total != s_last_total ||
        volume != s_last_volume) {
      const uint16_t cur = current < 0 ? 0xFFFF : (uint16_t)current;
      const uint16_t cc = catcur < 0 ? 0xFFFF : (uint16_t)catcur;
      const uint8_t m[13] = {0x01, (uint8_t)state, (uint8_t)(cur & 0xFF), (uint8_t)(cur >> 8),
                             (uint8_t)gen, (uint8_t)(count > 255 ? 255 : count), (uint8_t)flags,
                             (uint8_t)(cc & 0xFF), (uint8_t)(cc >> 8), (uint8_t)catgen,
                             (uint8_t)(total & 0xFF), (uint8_t)((total >> 8) & 0xFF),
                             (uint8_t)(volume < 0 ? 0 : volume > 200 ? 200 : volume)};
      ev_push(m, sizeof(m));
      s_last_state = state;
      s_last_current = current;
      s_last_gen = gen;
      s_last_count = count;
      s_last_flags = flags;
      s_last_catcur = catcur;
      s_last_catgen = catgen;
      s_last_total = total;
      s_last_volume = volume;
    }
    // la liste passe apres les evenements, la bibliotheque apres la liste
    if (ev_flush()) {
      tx_step();
      if (s_tx.phase == TX_IDLE) lib_step();
    }
  }

  // Demandes du telephone, dans l'ordre d'arrivee.
  for (int k = 0; k < 4; k++) {
    uint8_t gen = 0;
    uint16_t idx = 0;
    const int r = bluetooth_controller_take_games_request(&gen, &idx);
    if (r == BLE_GAMES_REQ_NONE) break;
    if (r == BLE_GAMES_REQ_LIST) {
      s_last_state = -1;   // ETAT renvoye juste avant la liste
      tx_start(false, false, 0, 0);
      continue;
    }
    if (r == BLE_GAMES_REQ_LIST_ALL || r == BLE_GAMES_REQ_LIST_ALL_HAVE) {
      s_last_state = -1;
      tx_start(true, r == BLE_GAMES_REQ_LIST_ALL_HAVE, gen, idx);
      s_lib_want = true;    // Bibliotheque renvoyee apres la liste
      s_lib_sent = 0;
      s_lib_phase = -1;
      continue;
    }
    if (r == BLE_GAMES_REQ_FAV) {   // Favori (bascule)
      if (gen != osd_catalog_generation() || (int)idx >= osd_catalog_count()) {
        Serial.println("[TEL] Favori ignore : liste perimee.");
        continue;
      }
      const bool was = osd_catalog_is_fav((int)idx);
      if (!was && osd_fav_full()) Serial.println("[TEL] Favoris pleins.");
      else Serial.printf("[TEL] Favori %s : %s\n", osd_catalog_toggle_fav((int)idx) ? "ajoute" : "retire",
                         osd_catalog_filename((int)idx));
      s_lib_sent = 0;   // renvoyee meme si rien n'a change (favoris pleins)
      continue;
    }
    if (r == BLE_GAMES_REQ_VOLUME) {
      osd_volume_remote((int)idx);
      continue;
    }
    if (r == BLE_GAMES_REQ_PAUSE) return GAMES_LINK_PAUSE;
    if (r == BLE_GAMES_REQ_RESUME) return GAMES_LINK_RESUME;
    if (r == BLE_GAMES_REQ_TVLIST) return GAMES_LINK_TVLIST;
    if (r == BLE_GAMES_REQ_LAUNCH) {
      if (gen != (uint8_t)osd_games_generation()) {
        Serial.println("[TEL] Lancement refuse : liste perimee (relue depuis).");
        games_link_report_refused(GAMES_LINK_REFUSE_STALE, 0);
        continue;
      }
      if ((int)idx >= osd_games_count()) {
        Serial.printf("[TEL] Lancement refuse : jeu n° %u inconnu.\n", (unsigned)idx);
        games_link_report_refused(GAMES_LINK_REFUSE_UNKNOWN, 0);
        continue;
      }
      if (index) *index = (int)idx;
      s_launch_resume = false;
      return GAMES_LINK_LAUNCH;
    }
    if (r == BLE_GAMES_REQ_LAUNCH_ALL || r == BLE_GAMES_REQ_RESUME_ALL) {   // Numero du catalogue
      if (gen != osd_catalog_generation()) {
        Serial.println("[TEL] Lancement refuse : liste perimee (relue depuis).");
        games_link_report_refused(GAMES_LINK_REFUSE_STALE, 0);
        continue;
      }
      // La page qui contient ce jeu devient la page affichee (meme index
      // que si on l'avait choisi a l'ecran).
      const int pidx = ((int)idx < osd_catalog_count()) ? osd_games_show_catalog((int)idx) : -1;
      if (pidx < 0) {
        Serial.printf("[TEL] Lancement refuse : jeu n° %u inconnu.\n", (unsigned)idx);
        games_link_report_refused(GAMES_LINK_REFUSE_UNKNOWN, 0);
        continue;
      }
      if (index) *index = pidx;
      s_launch_resume = (r == BLE_GAMES_REQ_RESUME_ALL) && osd_catalog_has_resume((int)idx);
      return GAMES_LINK_LAUNCH;
    }
  }
  return GAMES_LINK_NONE;
}

// Evenement envoye tout de suite (sans attendre le prochain
// games_link_poll()) : le chargement d'un jeu ou un redemarrage suivent
// souvent juste apres, et le telephone doit l'apprendre avant.
static void report_event(uint8_t code, uint8_t a, uint8_t b, int cat) {
  // + numero du catalogue (0xFFFF : aucun) - 4 octets avant.
  const uint16_t c = cat < 0 ? 0xFFFF : (uint16_t)cat;
  const uint8_t m[6] = {0x05, code, a, b, (uint8_t)(c & 0xFF), (uint8_t)(c >> 8)};
  ev_push(m, sizeof(m));
  if (bluetooth_controller_games_ready()) ev_flush();
}

void games_link_report_loading(int index) {
  report_event(1, (uint8_t)(index & 0xFF), (uint8_t)((index >> 8) & 0xFF), osd_games_page_to_catalog(index));
}

void games_link_report_started(int index) {
  report_event(2, (uint8_t)(index & 0xFF), (uint8_t)((index >> 8) & 0xFF), osd_games_running_catalog());
}

void games_link_report_refused(int reason, int detail) {
  report_event(3, (uint8_t)reason, (uint8_t)(detail < 0 ? 0 : detail), -1);
}
