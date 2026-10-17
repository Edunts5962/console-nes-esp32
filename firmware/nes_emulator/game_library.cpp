// game_library.cpp - voir game_library.h.
#include <Arduino.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include "FS.h"
#include "game_library.h"
#include "rom_storage.h"

#define LIB_NVS "nesbib"
#define SAVE_DIR "/saves"
#define SAVE_TMP "/saves/NESSAVE.TMP"

static Preferences s_prefs;
static bool s_ok = false;
static uint32_t s_gen = 1;

// Noms en PSRAM (4,6 Ko : la memoire interne est comptee)
typedef char LibName[64];
static LibName *s_rec_name = NULL;
static uint8_t s_rec_where[LIB_MAX_RECENTS];
static int s_rec_n = 0;
static LibName *s_fav = NULL;
static int s_fav_n = 0;
static LibName s_none[1] = {""};

static bool lib_alloc() {
  if (s_rec_name && s_fav) return true;
  s_rec_name = (LibName *)heap_caps_calloc(LIB_MAX_RECENTS, sizeof(LibName), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  s_fav = (LibName *)heap_caps_calloc(LIB_MAX_FAVORITES, sizeof(LibName), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_rec_name) s_rec_name = (LibName *)calloc(LIB_MAX_RECENTS, sizeof(LibName));
  if (!s_fav) s_fav = (LibName *)calloc(LIB_MAX_FAVORITES, sizeof(LibName));
  return s_rec_name && s_fav;
}

// ---- NVS : "recents" = "w<TAB>nom\n..." ; "favs" = "nom\n..." ----
static void save_recents() {
  if (!s_ok) return;
  String s;
  for (int i = 0; i < s_rec_n; i++) {
    s += (char)('0' + s_rec_where[i]);
    s += '\t';
    s += s_rec_name[i];
    s += '\n';
  }
  s_prefs.putString("recents", s);
}

static void save_favs() {
  if (!s_ok) return;
  String s;
  for (int i = 0; i < s_fav_n; i++) {
    s += s_fav[i];
    s += '\n';
  }
  s_prefs.putString("favs", s);
}

void lib_init() {
  if (s_ok) return;
  if (!lib_alloc()) {
    Serial.println("[BIBLIO] Pas de memoire pour les recents et favoris.");
    return;
  }
  s_ok = s_prefs.begin(LIB_NVS, false);
  if (!s_ok) {
    Serial.println("[BIBLIO] NVS indisponible : recents et favoris non gardes.");
    return;
  }
  String r = s_prefs.getString("recents", "");
  int start = 0;
  s_rec_n = 0;
  while (start < (int)r.length() && s_rec_n < LIB_MAX_RECENTS) {
    int nl = r.indexOf('\n', start);
    if (nl < 0) nl = r.length();
    String line = r.substring(start, nl);
    start = nl + 1;
    if (line.length() < 3 || line[1] != '\t') continue;
    snprintf(s_rec_name[s_rec_n], 64, "%s", line.c_str() + 2);
    s_rec_where[s_rec_n] = (uint8_t)(line[0] == '1' ? ROM_WHERE_FLASH : ROM_WHERE_SD);
    s_rec_n++;
  }
  String f = s_prefs.getString("favs", "");
  start = 0;
  s_fav_n = 0;
  while (start < (int)f.length() && s_fav_n < LIB_MAX_FAVORITES) {
    int nl = f.indexOf('\n', start);
    if (nl < 0) nl = f.length();
    String line = f.substring(start, nl);
    start = nl + 1;
    if (!line.length()) continue;
    snprintf(s_fav[s_fav_n++], 64, "%s", line.c_str());
  }
  Serial.printf("[BIBLIO] %d jeu(x) recent(s), %d favori(s)\n", s_rec_n, s_fav_n);
}

// ---- Recents ----
void lib_recent_remove(const char *name) {
  if (!s_rec_name) return;
  for (int i = 0; i < s_rec_n; i++) {
    if (strcmp(s_rec_name[i], name) == 0) {
      for (int k = i; k < s_rec_n - 1; k++) {
        memcpy(s_rec_name[k], s_rec_name[k + 1], 64);
        s_rec_where[k] = s_rec_where[k + 1];
      }
      s_rec_n--;
      s_gen++;
      save_recents();
      return;
    }
  }
}

void lib_recent_push(const char *name, int where) {
  if (!name || !name[0] || !s_rec_name) return;
  if (s_rec_n > 0 && strcmp(s_rec_name[0], name) == 0 && s_rec_where[0] == where) return;   // deja en tete
  int found = -1;
  for (int i = 0; i < s_rec_n; i++)
    if (strcmp(s_rec_name[i], name) == 0) { found = i; break; }
  int last = (found >= 0) ? found : (s_rec_n < LIB_MAX_RECENTS ? s_rec_n : LIB_MAX_RECENTS - 1);
  for (int k = last; k > 0; k--) {
    memcpy(s_rec_name[k], s_rec_name[k - 1], 64);
    s_rec_where[k] = s_rec_where[k - 1];
  }
  snprintf(s_rec_name[0], 64, "%s", name);
  s_rec_where[0] = (uint8_t)where;
  if (found < 0 && s_rec_n < LIB_MAX_RECENTS) s_rec_n++;
  s_gen++;
  save_recents();
}

int lib_recent_count() { return s_rec_n; }
const char *lib_recent_name(int i) { return (s_rec_name && i >= 0 && i < s_rec_n) ? s_rec_name[i] : s_none[0]; }
int lib_recent_where(int i) { return (i >= 0 && i < s_rec_n) ? s_rec_where[i] : ROM_WHERE_SD; }

// ---- Favoris ----
static int fav_index(const char *name) {
  if (!s_fav) return -1;
  for (int i = 0; i < s_fav_n; i++)
    if (strcmp(s_fav[i], name) == 0) return i;
  return -1;
}

bool lib_fav_is(const char *name) { return name && fav_index(name) >= 0; }
bool lib_fav_full() { return s_fav_n >= LIB_MAX_FAVORITES; }
int lib_fav_count() { return s_fav_n; }
const char *lib_fav_name(int i) { return (s_fav && i >= 0 && i < s_fav_n) ? s_fav[i] : s_none[0]; }
uint32_t lib_generation() { return s_gen; }

bool lib_fav_toggle(const char *name) {
  if (!name || !name[0] || !s_fav) return false;
  const int i = fav_index(name);
  if (i >= 0) {
    for (int k = i; k < s_fav_n - 1; k++) memcpy(s_fav[k], s_fav[k + 1], 64);
    s_fav_n--;
    s_gen++;
    save_favs();
    Serial.printf("[BIBLIO] Favori retire : %s\n", name);
    return false;
  }
  if (s_fav_n >= LIB_MAX_FAVORITES) {
    Serial.println("[BIBLIO] Favoris pleins.");
    return false;
  }
  snprintf(s_fav[s_fav_n++], 64, "%s", name);
  s_gen++;
  save_favs();
  Serial.printf("[BIBLIO] Favori ajoute : %s\n", name);
  return true;
}

// ---- Fichiers de sauvegarde ----
static bool save_path(const char *game_name, const char *ext, char *out, size_t n) {
  const char *base = game_name[0] == '/' ? game_name + 1 : game_name;
  size_t len = strlen(base);
  if (len > 4 && strcasecmp(base + len - 4, ".nes") == 0) len -= 4;
  const int w = snprintf(out, n, SAVE_DIR "/%.*s.%s", (int)len, base, ext);
  return w > 0 && (size_t)w < n;
}

int lib_save_find(const char *game_name, int game_where, const char *ext) {
  char p[ROM_PATH_MAX + 16];
  if (!save_path(game_name, ext, p, sizeof(p))) return -1;
  const int order[2] = {game_where, game_where == ROM_WHERE_SD ? ROM_WHERE_FLASH : ROM_WHERE_SD};
  for (int k = 0; k < 2; k++) {
    fs::FS *fs = rom_storage_fs(order[k]);
    if (fs && fs->exists(p)) return order[k];
  }
  return -1;
}

long lib_save_size(const char *game_name, int game_where, const char *ext) {
  const int w = lib_save_find(game_name, game_where, ext);
  if (w < 0) return -1;
  char p[ROM_PATH_MAX + 16];
  save_path(game_name, ext, p, sizeof(p));
  File f = rom_storage_fs(w)->open(p, FILE_READ);
  if (!f) return -1;
  const long n = (long)f.size();
  f.close();
  return n;
}

long lib_save_read(const char *game_name, int game_where, const char *ext, uint8_t *buf, size_t max) {
  const int w = lib_save_find(game_name, game_where, ext);
  if (w < 0) return -1;
  char p[ROM_PATH_MAX + 16];
  save_path(game_name, ext, p, sizeof(p));
  File f = rom_storage_fs(w)->open(p, FILE_READ);
  if (!f) return -1;
  size_t got = 0;
  while (got < max) {
    const size_t n = f.read(buf + got, max - got);
    if (n == 0) break;
    got += n;
  }
  f.close();
  return (long)got;
}

bool lib_save_write(const char *game_name, int game_where, const char *ext, const uint8_t *buf, size_t len) {
  fs::FS *fs = rom_storage_fs(game_where);
  if (!fs) {   // emplacement du jeu absent : l'autre
    game_where = game_where == ROM_WHERE_SD ? ROM_WHERE_FLASH : ROM_WHERE_SD;
    fs = rom_storage_fs(game_where);
  }
  if (!fs) return false;
  char p[ROM_PATH_MAX + 16];
  if (!save_path(game_name, ext, p, sizeof(p))) return false;
  if (!fs->exists(SAVE_DIR)) fs->mkdir(SAVE_DIR);
  fs->remove(SAVE_TMP);
  File f = fs->open(SAVE_TMP, FILE_WRITE);
  if (!f) {
    Serial.printf("[SAUVEGARDE] Impossible d'ecrire %s\n", SAVE_TMP);
    return false;
  }
  size_t done = 0;
  while (done < len) {
    const size_t n = f.write(buf + done, len - done);
    if (n == 0) break;
    done += n;
  }
  f.close();
  if (done != len) {
    fs->remove(SAVE_TMP);
    Serial.printf("[SAUVEGARDE] Ecriture incomplete de %s (%u/%u octets - plein ?)\n", p, (unsigned)done, (unsigned)len);
    return false;
  }
  fs->remove(p);
  if (!fs->rename(SAVE_TMP, p)) {
    Serial.printf("[SAUVEGARDE] Renommage en %s impossible\n", p);
    return false;
  }
  // copie eventuelle sur l'autre emplacement (jeu deplace) : perimee
  fs::FS *other = rom_storage_fs(game_where == ROM_WHERE_SD ? ROM_WHERE_FLASH : ROM_WHERE_SD);
  if (other && other->exists(p)) other->remove(p);
  return true;
}

bool lib_save_remove(const char *game_name, int game_where, const char *ext) {
  bool any = false;
  char p[ROM_PATH_MAX + 16];
  if (!save_path(game_name, ext, p, sizeof(p))) return false;
  for (int w = 0; w < 2; w++) {
    fs::FS *fs = rom_storage_fs(w);
    if (fs && fs->exists(p)) any |= fs->remove(p);
  }
  (void)game_where;
  return any;
}
