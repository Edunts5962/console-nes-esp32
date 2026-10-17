// rom_storage.cpp - jeux sur carte SD et en memoire interne (voir
// rom_storage.h).
#include <Arduino.h>
#include "FS.h"
#include "SD_MMC.h"
#include "FFat.h"
#include "esp_vfs_fat.h"
#include "rom_storage.h"
#include "esp_partition.h"
#include "esp_flash.h"
#include "ff.h"              // Type de FAT, racine
#include "diskio_sdmmc.h"    // Numero de lecteur FatFs de la carte SD

static bool s_flash_ok = false;
static bool s_sd_ok = false;

// Diagnostic : si la memoire interne est indisponible, raison exacte,
// partitions REELLEMENT presentes et taille de la flash, affiches dans
// Reglages > A propos, le portail web et sur le port serie.
static char s_flash_reason[48] = "";
static char s_flash_detail[200] = "";
static char s_flash_label[17] = FFAT_PARTITION_LABEL;   // partition FAT utilisee

static void fmt_size(uint32_t bytes, char *out, size_t n) {
  if (bytes >= 1024UL * 1024UL) {
    unsigned long tenths = (unsigned long)(((uint64_t)bytes * 10ULL + 524288ULL) / 1048576ULL);
    snprintf(out, n, "%lu,%lu Mo", tenths / 10UL, tenths % 10UL);
  } else {
    snprintf(out, n, "%lu Ko", (unsigned long)(bytes / 1024UL));
  }
}

// "Partitions : app0 3,0 Mo, app1 3,0 Mo, spiffs 896 Ko (SPIFFS). Flash : 16 Mo (IDE : 16 Mo)."
// (les petites partitions systeme nvs/otadata/coredump sont omises)
static void build_flash_detail() {
  size_t k = 0;
  k += snprintf(s_flash_detail + k, sizeof(s_flash_detail) - k, "Partitions :");
  Serial.println("     Partitions de la flash :");
  esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
  bool first = true;
  while (it) {
    const esp_partition_t *p = esp_partition_get(it);
    char sz[16];
    fmt_size(p->size, sz, sizeof(sz));
    Serial.printf("       %-10s type %u sous-type 0x%02x  0x%06lx  %s\n", p->label, (unsigned)p->type,
                  (unsigned)p->subtype, (unsigned long)p->address, sz);
    bool small_sys = p->type == ESP_PARTITION_TYPE_DATA &&
                     (p->subtype == ESP_PARTITION_SUBTYPE_DATA_NVS || p->subtype == ESP_PARTITION_SUBTYPE_DATA_OTA ||
                      p->subtype == ESP_PARTITION_SUBTYPE_DATA_COREDUMP || p->subtype == ESP_PARTITION_SUBTYPE_DATA_PHY);
    if (!small_sys && k < sizeof(s_flash_detail)) {
      const char *kind = p->type == ESP_PARTITION_TYPE_APP ? ""
                       : p->subtype == ESP_PARTITION_SUBTYPE_DATA_FAT ? " (FAT)"
                       : p->subtype == ESP_PARTITION_SUBTYPE_DATA_SPIFFS ? " (SPIFFS)" : " (donnees)";
      k += snprintf(s_flash_detail + k, sizeof(s_flash_detail) - k, "%s %s %s%s", first ? "" : ",", p->label, sz, kind);
      first = false;
    }
    it = esp_partition_next(it);   // libere l'iterateur a la fin
  }
  uint32_t hdr = 0, phys = 0;
  esp_flash_get_size(esp_flash_default_chip, &hdr);
  esp_flash_get_physical_size(esp_flash_default_chip, &phys);
  Serial.printf("     Flash : %lu Mo (reglage IDE \"Flash Size\" : %lu Mo)\n",
                (unsigned long)(phys >> 20), (unsigned long)(hdr >> 20));
  if (k < sizeof(s_flash_detail))
    snprintf(s_flash_detail + k, sizeof(s_flash_detail) - k, ". Flash : %lu Mo (IDE : %lu Mo).",
             (unsigned long)(phys >> 20), (unsigned long)(hdr >> 20));
}

bool rom_storage_flash_begin(void (*first_format_cb)()) {
  if (s_flash_ok) return true;
  uint32_t t0 = millis();
  build_flash_detail();
  // Partition FAT : celle nommee "ffat" (schemas standard de l'IDE), sinon
  // la premiere partition de sous-type FAT, quel que soit son nom (table
  // de partitions personnalisee).
  const esp_partition_t *fat = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, FFAT_PARTITION_LABEL);
  if (!fat) fat = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, NULL);
  if (!fat) {
    // Sans partition FAT, rien a faire (et surtout rien a formater).
    s_flash_ok = false;
    snprintf(s_flash_reason, sizeof(s_flash_reason), "aucune partition FAT");
  } else {
    snprintf(s_flash_label, sizeof(s_flash_label), "%s", fat->label);
    // 1er essai SANS formatage ; s'il echoue, c'est la toute premiere
    // utilisation (partition vierge ou d'un autre usage) : on previent
    // (le formatage peut durer jusqu'a ~2 minutes : la partition entiere
    // est effacee) puis on formate.
    s_flash_ok = FFat.begin(false, "/ffat", 10, s_flash_label);
    if (!s_flash_ok) {
      Serial.println("     Memoire interne : premiere utilisation, formatage (jusqu'a ~2 min)...");
      if (first_format_cb) first_format_cb();
      s_flash_ok = FFat.begin(true, "/ffat", 10, s_flash_label);
    }
    if (!s_flash_ok) snprintf(s_flash_reason, sizeof(s_flash_reason), "montage impossible (%s)", s_flash_label);
  }
  if (s_flash_ok) {
    s_flash_reason[0] = '\0';
    Serial.printf("     Memoire interne (partition \"%s\") : %u Ko libres sur %u Ko (%lu ms)\n", s_flash_label,
                  (unsigned)(FFat.freeBytes() / 1024), (unsigned)(FFat.totalBytes() / 1024),
                  (unsigned long)(millis() - t0));
  } else {
    Serial.printf("     Memoire interne indisponible : %s\n", s_flash_reason);
    if (!fat) Serial.println("     (Outils > Partition Scheme > \"16M Flash (3MB APP/9.9MB FATFS)\" pour l'activer)");
  }
  return s_flash_ok;
}

const char *rom_storage_flash_reason() { return s_flash_reason; }
const char *rom_storage_flash_detail() { return s_flash_detail; }

bool rom_storage_flash_ok() { return s_flash_ok; }
bool rom_storage_sd_ok() { return s_sd_ok; }
void rom_storage_set_sd_ok(bool ok) { s_sd_ok = ok; }

bool rom_storage_available(int where) {
  return where == ROM_WHERE_FLASH ? s_flash_ok : s_sd_ok;
}

fs::FS *rom_storage_fs(int where) {
  if (where == ROM_WHERE_FLASH) return s_flash_ok ? (fs::FS *)&FFat : NULL;
  return s_sd_ok ? (fs::FS *)&SD_MMC : NULL;
}

// Espace d'un emplacement. ATTENTION : SD_MMC.totalBytes()/usedBytes() de
// la bibliotheque Arduino lisent TOUJOURS le lecteur FatFs "0:" - qui est
// la memoire interne depuis qu'elle est montee avant la carte SD. On
// passe donc par esp_vfs_fat_info(), qui interroge le bon volume d'apres
// son point de montage.
static bool fat_info(int where, uint64_t *total, uint64_t *free_b) {
  *total = 0; *free_b = 0;
  if (!rom_storage_available(where)) return false;
  return esp_vfs_fat_info(where == ROM_WHERE_FLASH ? "/ffat" : ROM_SD_MOUNTPOINT, total, free_b) == ESP_OK;
}

uint64_t rom_storage_total_bytes(int where) {
  uint64_t t, f;
  fat_info(where, &t, &f);
  return t;
}

uint64_t rom_storage_free_bytes(int where) {
  uint64_t t, f;
  fat_info(where, &t, &f);
  return f;
}

uint64_t rom_storage_used_bytes(int where) {
  uint64_t t, f;
  fat_info(where, &t, &f);
  return t > f ? t - f : 0;
}

bool rom_storage_make_path(int where, const char *name, char *out, size_t out_len) {
  if (name[0] == '/') name++;
  int n = snprintf(out, out_len, "%s/%s", where == ROM_WHERE_FLASH ? ROM_FLASH_PREFIX : "", name);
  return n > 0 && (size_t)n < out_len;   // false : chemin tronque, a ne pas utiliser
}

int rom_storage_split(const char *path, const char **fs_path) {
  if (strncmp(path, ROM_FLASH_PREFIX, ROM_FLASH_PREFIX_LEN) == 0) {
    *fs_path = path + ROM_FLASH_PREFIX_LEN;
    return ROM_WHERE_FLASH;
  }
  *fs_path = path;
  return ROM_WHERE_SD;
}

fs::File rom_storage_open_read(const char *path) {
  const char *p;
  int where = rom_storage_split(path, &p);
  fs::FS *fs = rom_storage_fs(where);
  if (!fs) return fs::File();
  return fs->open(p, FILE_READ);
}

bool rom_storage_remove(const char *path) {
  const char *p;
  int where = rom_storage_split(path, &p);
  fs::FS *fs = rom_storage_fs(where);
  if (!fs || !fs->exists(p)) return false;
  return fs->remove(p);
}

// Marge gardee libre a destination (grappes FAT partiellement remplies,
// repertoire) : 16 Ko.
#define ROM_SPACE_MARGIN (16 * 1024)

int rom_storage_move(const char *name, int from, int to) {
  fs::FS *src = rom_storage_fs(from), *dst = rom_storage_fs(to);
  if (!src || !dst || from == to) return ROM_MOVE_ERR_UNAVAIL;
  char path[ROM_PATH_MAX];
  int pl = snprintf(path, sizeof(path), "/%s", name[0] == '/' ? name + 1 : name);
  if (pl <= 0 || (size_t)pl >= sizeof(path)) return ROM_MOVE_ERR_NOFILE;   // nom trop long
  if (!src->exists(path)) return ROM_MOVE_ERR_NOFILE;
  if (dst->exists(path)) return ROM_MOVE_ERR_EXISTS;
  File in = src->open(path, FILE_READ);
  if (!in) return ROM_MOVE_ERR_NOFILE;
  size_t total = in.size();
  if ((uint64_t)total + ROM_SPACE_MARGIN > rom_storage_free_bytes(to)) { in.close(); return ROM_MOVE_ERR_SPACE; }
  // Racine pleine a destination (FAT12/16) ?
  const int need = rom_storage_name_entries(path);
  int used = 0, cap = 0;
  const bool small_root = rom_storage_root_entries(to, &used, &cap);
  if (small_root && cap - used < need) { in.close(); return ROM_MOVE_ERR_ROOT; }
  File out = dst->open(path, FILE_WRITE);
  if (!out) { in.close(); return (small_root && cap - used < need + 8) ? ROM_MOVE_ERR_ROOT : ROM_MOVE_ERR_IO; }
  const size_t BUF = 4096;
  uint8_t *buf = (uint8_t *)malloc(BUF);   // RAM interne (tampon DMA de la carte SD)
  if (!buf) { in.close(); out.close(); dst->remove(path); return ROM_MOVE_ERR_IO; }
  size_t done = 0;
  bool ok = true;
  uint32_t t0 = millis();
  while (done < total) {
    size_t n = in.read(buf, BUF);
    if (n == 0 || out.write(buf, n) != n) { ok = false; break; }
    done += n;
    yield();
  }
  free(buf);
  in.close();
  out.close();
  if (ok) {
    File chk = dst->open(path, FILE_READ);
    ok = chk && (size_t)chk.size() == total;
    if (chk) chk.close();
  }
  if (!ok) {
    dst->remove(path);
    Serial.printf("[STOCKAGE] Deplacement de %s : ECHEC (copie annulee)\n", path);
    return ROM_MOVE_ERR_IO;
  }
  if (!src->remove(path)) {
    // L'original n'a pas pu etre supprime : on retire la copie pour ne pas
    // laisser le jeu en double, et on signale l'echec.
    dst->remove(path);
    Serial.printf("[STOCKAGE] Deplacement de %s : ECHEC (original non supprime)\n", path);
    return ROM_MOVE_ERR_IO;
  }
  Serial.printf("[STOCKAGE] %s deplace vers %s (%u octets, %lu ms)\n", path,
                to == ROM_WHERE_FLASH ? "la memoire interne" : "la carte SD",
                (unsigned)total, (unsigned long)(millis() - t0));
  return ROM_MOVE_OK;
}

// ---------------------------------------------------------------------
// Maintenance
// ---------------------------------------------------------------------
static_assert(sizeof(ROM_UPLOAD_TMP_SUFFIX) == sizeof(ROM_UPLOAD_OLD_SUFFIX),
              "les deux suffixes doivent avoir la meme longueur (rom_storage_cleanup_leftovers)");

static bool is_game_name(const String &n) { return n.endsWith(".nes") || n.endsWith(".NES"); }

// Restes d'un envoi par le portail (wifi_portal.cpp) : "nom.nes~tmp" (envoi en
// cours ou interrompu) et "nom.nes~old". Suffixes volontairement inhabituels :
// aucun autre fichier de la carte (musique, etc.) ne peut etre pris pour l'un
// d'eux.
static bool is_upload_leftover(const String &n) {
  return n.endsWith(".nes" ROM_UPLOAD_TMP_SUFFIX) || n.endsWith(".NES" ROM_UPLOAD_TMP_SUFFIX) ||
         n.endsWith(".nes" ROM_UPLOAD_OLD_SUFFIX) || n.endsWith(".NES" ROM_UPLOAD_OLD_SUFFIX) ||
         n.equalsIgnoreCase(ROM_UPLOAD_TMP_NAME + 1);   // "NESUPLD.TMP"
}

// Releve (sans rien modifier) les fichiers de la racine qui verifient
// keep(), en chemins "/nom" ; 64 au plus par appel.
static int list_root(fs::FS *fs, bool (*keep)(const String &), char (*out)[ROM_PATH_MAX], int max) {
  int n = 0;
  File root = fs->open("/");
  if (!root) return 0;
  File e = root.openNextFile();
  while (e && n < max) {
    String nm = e.name();
    if (!e.isDirectory() && keep(nm)) {
      int l = snprintf(out[n], ROM_PATH_MAX, "/%s", nm.c_str());
      if (l > 0 && l < ROM_PATH_MAX) n++;   // nom trop long : ignore
    }
    e.close();
    e = root.openNextFile();
  }
  if (e) e.close();
  root.close();
  return n;
}

static char s_names[64][ROM_PATH_MAX];

int rom_storage_delete_all_games(int where) {
  fs::FS *fs = rom_storage_fs(where);
  if (!fs) return -1;
  // On releve d'abord les noms, puis on supprime : supprimer pendant le
  // parcours du repertoire peut sauter des entrees. Par paquets de 64.
  int total = 0;
  while (true) {
    int n = list_root(fs, is_game_name, s_names, 64);
    if (n == 0) break;
    int removed = 0;
    for (int i = 0; i < n; i++) if (fs->remove(s_names[i])) removed++;
    total += removed;
    if (removed == 0) break;          // plus rien de supprimable : on arrete
  }
  // Et les restes d'envois interrompus (jamais comptes comme des jeux).
  int n = list_root(fs, is_upload_leftover, s_names, 64);
  for (int i = 0; i < n; i++) fs->remove(s_names[i]);
  Serial.printf("[STOCKAGE] %d jeu(x) supprime(s) (%s)\n", total,
                where == ROM_WHERE_FLASH ? "memoire interne" : "carte SD");
  return total;
}

void rom_storage_cleanup_leftovers() {
  for (int where = 0; where < 2; where++) {
    fs::FS *fs = rom_storage_fs(where);
    if (!fs) continue;
    int n = list_root(fs, is_upload_leftover, s_names, 64);
    for (int i = 0; i < n; i++) {
      String p = s_names[i];
      String fin = p.substring(0, p.length() - strlen(ROM_UPLOAD_TMP_SUFFIX));   // meme longueur pour ~old
      if (p.endsWith(ROM_UPLOAD_OLD_SUFFIX) && !fs->exists(fin)) {
        // Coupure pendant un remplacement : la version mise de cote (~old) est la
        // seule copie complete, on la remet en place.
        bool ok = fs->rename(p, fin);
        Serial.printf("[STOCKAGE] Restauration de %s : %s\n", fin.c_str(), ok ? "OK" : "ECHEC");
      } else {
        fs->remove(p);
        Serial.printf("[STOCKAGE] Reste d'envoi supprime : %s\n", p.c_str());
      }
    }
  }
}

bool rom_storage_flash_erase() {
  if (!s_flash_ok) return false;
  Serial.println("[STOCKAGE] Effacement de la memoire interne...");
  FFat.end();
  s_flash_ok = false;
  bool fmt = FFat.format(FFAT_WIPE_QUICK, s_flash_label);
  bool mounted = FFat.begin(true, "/ffat", 10, s_flash_label);
  s_flash_ok = mounted;
  Serial.printf("[STOCKAGE] Memoire interne : formatage %s, remontage %s\n",
                fmt ? "OK" : "ECHEC", mounted ? "OK" : "ECHEC");
  return fmt && mounted;
}

// Acces au descripteur de carte (membre protege de fs::SDMMCFS) : le
// pointeur-sur-membre est forme depuis une classe derivee, ce que le C++
// autorise ; aucune instance de cette classe n'est jamais creee.
namespace {
struct SdCardAccess : fs::SDMMCFS {
  static sdmmc_card_t *fs::SDMMCFS::*member() { return &SdCardAccess::_card; }
};
}

bool rom_storage_sd_format() {
  if (!s_sd_ok) return false;
  sdmmc_card_t *card = SD_MMC.*(SdCardAccess::member());
  if (!card) return false;
  Serial.println("[STOCKAGE] Formatage de la carte SD...");
  uint32_t t0 = millis();
  esp_vfs_fat_mount_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.format_if_mount_failed = false;
  cfg.max_files = 5;                    // meme valeur que SD_MMC.begin() par defaut
  // Grappes : 32 Ko (sinon 512 octets : tables FAT enormes), MAIS au moins
  // 70 000 grappes : en dessous de 65 526, FatFs choisit le FAT16, dont la
  // racine ne tient que 512 noms (~150 jeux) - le cas de toute carte de 2 Go
  // ou moins avec des grappes de 32 Ko. Au-dela, FatFs passe de lui-meme au
  // FAT32.
  const uint64_t cap = (uint64_t)card->csd.capacity * (uint64_t)card->csd.sector_size;
  size_t au = 32 * 1024;
  while (au > (size_t)card->csd.sector_size && cap / au < 70000ULL) au /= 2;
  cfg.allocation_unit_size = au;
  Serial.printf("[STOCKAGE] Carte de %llu Mo, grappes de %u octets\n", (unsigned long long)(cap >> 20), (unsigned)au);
  esp_err_t err = esp_vfs_fat_sdcard_format_cfg(ROM_SD_MOUNTPOINT, card, &cfg);
  // En cas d'echec, ESP-IDF peut avoir libere la carte (remontage rate) :
  // on ne la touche plus jusqu'au prochain demarrage.
  if (err != ESP_OK) s_sd_ok = false;
  Serial.printf("[STOCKAGE] Formatage de la carte SD : %s (%lu ms)\n",
                err == ESP_OK ? "OK" : esp_err_to_name(err), (unsigned long)(millis() - t0));
  if (err == ESP_OK) rom_storage_log_format();
  return err == ESP_OK;
}

// ---------------------------------------------------------------------
// Type de FAT et racine des volumes FAT12/FAT16 (rom_storage.h)
// ---------------------------------------------------------------------
// Volume FatFs d'un emplacement : numero de lecteur ("N:") et objet FATFS.
// Carte SD : lecteur enregistre pour sa carte. Memoire interne : l'autre
// lecteur monte (FFat ne donne pas acces au sien).
static FATFS *drive_fs(int d) {
  char drv[3] = {(char)('0' + d), ':', 0};
  DWORD n = 0;
  FATFS *fs = NULL;
  return (f_getfree(drv, &n, &fs) == FR_OK) ? fs : NULL;
}

static int sd_drive() {
  if (!s_sd_ok) return -1;
  sdmmc_card_t *card = SD_MMC.*(SdCardAccess::member());
  if (!card) return -1;
  const BYTE d = ff_diskio_get_pdrv_card(card);
  return (d == 0xFF || d >= FF_VOLUMES) ? -1 : (int)d;
}

static FATFS *volume_of(int where, int *drive) {
  if (!rom_storage_available(where)) return NULL;
  const int sd = sd_drive();
  if (where == ROM_WHERE_SD) {
    if (sd < 0) return NULL;
    if (drive) *drive = sd;
    return drive_fs(sd);
  }
  for (int d = 0; d < FF_VOLUMES; d++) {
    if (d == sd) continue;
    FATFS *fs = drive_fs(d);
    if (fs) { if (drive) *drive = d; return fs; }
  }
  return NULL;
}

int rom_storage_fat_type(int where) {
  FATFS *fs = volume_of(where, NULL);
  if (!fs) return 0;
  switch (fs->fs_type) {
    case FS_FAT12: return 12;
    case FS_FAT16: return 16;
    case FS_FAT32: return 32;
    default: return 0;
  }
}

// Longueur en UTF-16 (unites des noms longs FAT) d'un nom UTF-8.
static int utf16_units(const char *s) {
  int n = 0;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++)
    if ((*p & 0xC0) != 0x80) n += (*p >= 0xF0) ? 2 : 1;
  return n;
}

// Nom court 8.3 en majuscules (une seule entree, sans nom long).
static bool is_short_name(const char *n) {
  int base = 0, ext = -1;
  for (const char *p = n; *p; p++) {
    const unsigned char c = (unsigned char)*p;
    if (c == '.') { if (ext >= 0 || base == 0) return false; ext = 0; continue; }
    if (c >= 0x80 || c <= ' ' || (c >= 'a' && c <= 'z') || strchr("\"*+,/:;<=>?[\\]|", c)) return false;
    if (ext >= 0) { if (++ext > 3) return false; }
    else if (++base > 8) return false;
  }
  return base > 0 && ext != 0;
}

int rom_storage_name_entries(const char *name) {
  if (name[0] == '/') name++;
  if (is_short_name(name)) return 1;
  return (utf16_units(name) + 12) / 13 + 1;
}

bool rom_storage_root_entries(int where, int *used, int *total) {
  int d = -1;
  FATFS *fs = volume_of(where, &d);
  if (!fs || (fs->fs_type != FS_FAT12 && fs->fs_type != FS_FAT16) || d < 0) return false;
  char path[4] = {(char)('0' + d), ':', '/', 0};
  FF_DIR dir;
  if (f_opendir(&dir, path) != FR_OK) return false;
  static FILINFO fi;   // ~300 octets : hors de la pile
  int u = 1;           // etiquette de volume
  while (f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
    const bool lfn = fi.altname[0] && strcasecmp(fi.fname, fi.altname) != 0;
    u += lfn ? (utf16_units(fi.fname) + 12) / 13 + 1 : 1;
  }
  f_closedir(&dir);
  if (used) *used = u;
  if (total) *total = fs->n_rootdir;
  return true;
}

void rom_storage_log_format() {
  for (int where = 0; where < 2; where++) {
    if (!rom_storage_available(where)) continue;
    const char *nm = where == ROM_WHERE_FLASH ? "Memoire interne" : "Carte SD";
    const int t = rom_storage_fat_type(where);
    int used = 0, total = 0;
    if (rom_storage_root_entries(where, &used, &total)) {
      Serial.printf("[STOCKAGE] %s : FAT%d, racine %d/%d entrees (~%d jeu(x) de plus au plus)\n", nm, t, used, total,
                    total > used ? (total - used) / 4 : 0);
      if (where == ROM_WHERE_SD)
        Serial.println("[STOCKAGE] /!\\ Carte SD en FAT16 : 512 noms a la racine, ~150 jeux. Formater en FAT32 "
                       "(portail : Maintenance > Formater, ou sur un PC) pour en mettre plus.");
    } else {
      Serial.printf("[STOCKAGE] %s : FAT%d\n", nm, t);
    }
  }
}

// ==================== jeux ecartes (voir rom_storage.h) ====================
static bool quarantine_path(const char *name, char *out, size_t n) {
  const char *b = name[0] == '/' ? name + 1 : name;
  const int w = snprintf(out, n, ROM_QUARANTINE_DIR "/%s", b);
  return w > 0 && (size_t)w < n;
}

bool rom_storage_quarantine(const char *path) {
  const char *p;
  const int where = rom_storage_split(path, &p);
  fs::FS *fs = rom_storage_fs(where);
  if (!fs || !fs->exists(p)) return false;
  char q[ROM_PATH_MAX + 16];
  if (!quarantine_path(p, q, sizeof(q))) return false;
  if (!fs->exists(ROM_QUARANTINE_DIR)) fs->mkdir(ROM_QUARANTINE_DIR);
  if (fs->exists(q)) fs->remove(q);   // ancienne copie ecartee du meme jeu
  const bool ok = fs->rename(p, q);
  Serial.printf("[ROM] %s %s %s\n", path, ok ? "range dans" : "IMPOSSIBLE a ranger dans", q);
  return ok;
}

int rom_storage_quarantine_list(void (*cb)(const char *name, int where, uint32_t size, void *ctx), void *ctx) {
  int n = 0;
  for (int where = 0; where < 2; where++) {
    fs::FS *fs = rom_storage_fs(where);
    if (!fs || !fs->exists(ROM_QUARANTINE_DIR)) continue;
    File dir = fs->open(ROM_QUARANTINE_DIR);
    if (!dir || !dir.isDirectory()) continue;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      if (!f.isDirectory()) {
        const char *nm = f.name();
        const char *slash = strrchr(nm, '/');
        if (slash) nm = slash + 1;
        const size_t l = strlen(nm);
        if (l > 4 && strcasecmp(nm + l - 4, ".nes") == 0) {
          if (cb) cb(nm, where, (uint32_t)f.size(), ctx);
          n++;
        }
      }
      f.close();
    }
    dir.close();
  }
  return n;
}

int rom_storage_quarantine_restore(int where, const char *name) {
  fs::FS *fs = rom_storage_fs(where);
  if (!fs) return ROM_MOVE_ERR_UNAVAIL;
  char q[ROM_PATH_MAX + 16], r[ROM_PATH_MAX];
  if (!quarantine_path(name, q, sizeof(q))) return ROM_MOVE_ERR_NOFILE;
  const int rl = snprintf(r, sizeof(r), "/%s", name[0] == '/' ? name + 1 : name);
  if (rl <= 0 || (size_t)rl >= sizeof(r)) return ROM_MOVE_ERR_NOFILE;
  if (!fs->exists(q)) return ROM_MOVE_ERR_NOFILE;
  if (fs->exists(r)) return ROM_MOVE_ERR_EXISTS;
  int used = 0, cap = 0;
  if (rom_storage_root_entries(where, &used, &cap) && cap - used < rom_storage_name_entries(r)) return ROM_MOVE_ERR_ROOT;
  if (!fs->rename(q, r)) return ROM_MOVE_ERR_IO;
  Serial.printf("[ROM] %s remis dans la liste des jeux\n", r);
  return ROM_MOVE_OK;
}

bool rom_storage_quarantine_remove(int where, const char *name) {
  fs::FS *fs = rom_storage_fs(where);
  char q[ROM_PATH_MAX + 16];
  if (!fs || !quarantine_path(name, q, sizeof(q)) || !fs->exists(q)) return false;
  return fs->remove(q);
}
