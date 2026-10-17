// crash_guard.cpp - voir crash_guard.h pour le principe general.
//
// Stockage en NVS via la bibliotheque Preferences (Arduino-ESP32), dans un
// espace de noms dedie ("nescrash").
#include <Arduino.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include "esp_task_wdt.h"
#include "FS.h"
#include "SD_MMC.h"
#include "rom_storage.h"   // Jeux aussi en memoire interne ("flash:/nom.nes")
#include "crash_guard.h"
#include "game_library.h"   // Reprise automatique effacee si elle plante

#define CRASH_GUARD_NAMESPACE  "nescrash"
#define STARTUP_GRACE_MS       4000  // duree de jeu reelle a atteindre avant de considerer le lancement reussi
#define STARTUP_WDT_TIMEOUT_S  8     // duree max sans la moindre image avant redemarrage force
#define MAX_BAD_ROMS           20    // nombre d'entrees max conservees dans la liste noire (en RAM et en NVS)
#define BAD_LIST_MAX_LEN       900   // taille max (octets) de la chaine "badlist" en NVS

static Preferences prefs;
static bool wdt_armed = false;
static bool boot_confirmed = true;  // true tant qu'aucune partie n'est en cours (etat de repos, menu)
static unsigned long launch_started_ms = 0;

// ROM detectee comme fautive par crash_guard_init() (lue depuis la NVS, AVANT
// le montage de la carte SD) et pas encore mise de cote : la mise en
// quarantaine attend crash_guard_process_pending_deletion(), appelee plus
// tard dans setup(). Chemin complet : "/nom.nes" (carte SD) ou
// "flash:/nom.nes" (memoire interne), d'ou ROM_PATH_MAX.
static char pending_delete_rom[ROM_PATH_MAX] = "";

// Liste noire chargee en RAM au demarrage (une seule lecture NVS), pour que
// crash_guard_is_rom_bad() - appelee a chaque redessin du menu - n'aille pas
// relire la flash a chaque image. Taille des noms coherente avec
// game_list.names (osd.cpp), sinon la comparaison strcmp() pourrait echouer.
// Une entree vaut "nom.nes" (carte SD) ou "flash:/nom.nes" (memoire interne).
static char bad_rom_cache[MAX_BAD_ROMS][ROM_PATH_MAX];
static int bad_rom_cache_count = 0;

// ROMs a l'ecran desquelles la console a plante pendant un CHANGEMENT DE JEU
// SANS REDEMARRAGE (manette du telephone) : seulement marquees "!" (cle NVS
// "hotbad"), jamais ecartees - le plantage peut venir du changement lui-meme
// plutot que du jeu. Retirees de cette liste des que le jeu demarre
// normalement (crash_guard_confirm_boot()). Volontairement separee de
// "badlist" : crash_guard_process_pending_deletion() met en quarantaine les
// ROMs de "badlist".
#define MAX_HOT_BAD 10
static char hot_bad_cache[MAX_HOT_BAD][ROM_PATH_MAX];
static int hot_bad_count = 0;
static char launch_rom[ROM_PATH_MAX] = "";   // ROM du lancement en cours (nom sans '/')

// Plantage pendant le chargement d'une reprise automatique : c'est la reprise
// (fichier.sta) qui est effacee, le jeu reste dans la liste.
static char pending_drop_state[ROM_PATH_MAX] = "";
// Message pour l'ecran des menus (une fois), "" : aucun.
static char s_notice[176] = "";

static void display_name(const char *path, char *out, size_t n) {
  const char *p;
  rom_storage_split(path, &p);
  if (p[0] == '/') p++;
  snprintf(out, n, "%s", p);
  const size_t l = strlen(out);
  if (l > 4 && strcasecmp(out + l - 4, ".nes") == 0) out[l - 4] = '\0';
}

// Le texte reste dans s_notice (rendu une seule fois : s_notice_taken).
static bool s_notice_taken = false;
const char *crash_guard_take_notice() {
  if (!s_notice[0] || s_notice_taken) return NULL;
  s_notice_taken = true;
  return s_notice;
}

static const char *strip_leading_slash(const char *name) {
  return (name[0] == '/') ? (name + 1) : name;
}

// ---- liste "hotbad" (voir plus haut) ----
static void hot_bad_load() {
  hot_bad_count = 0;
  String list = prefs.getString("hotbad", "");
  int start = 0;
  while (start < (int)list.length() && hot_bad_count < MAX_HOT_BAD) {
    int nl = list.indexOf('\n', start);
    String entry = (nl == -1) ? list.substring(start) : list.substring(start, nl);
    if (entry.length() > 0) {
      strncpy(hot_bad_cache[hot_bad_count], entry.c_str(), ROM_PATH_MAX - 1);
      hot_bad_cache[hot_bad_count][ROM_PATH_MAX - 1] = '\0';
      hot_bad_count++;
    }
    if (nl == -1) break;
    start = nl + 1;
  }
}

static void hot_bad_save() {
  String list = "";
  for (int i = 0; i < hot_bad_count; i++) list += String(hot_bad_cache[i]) + "\n";
  prefs.putString("hotbad", list);
}

static int hot_bad_find(const char *name) {
  for (int i = 0; i < hot_bad_count; i++) {
    if (strcmp(hot_bad_cache[i], name) == 0) return i;
  }
  return -1;
}

static void hot_bad_add(const char *name) {
  if (name[0] == '\0' || hot_bad_find(name) >= 0) return;
  if (hot_bad_count == MAX_HOT_BAD) {   // liste pleine : la plus ancienne saute
    for (int i = 1; i < hot_bad_count; i++) strcpy(hot_bad_cache[i - 1], hot_bad_cache[i]);
    hot_bad_count--;
  }
  strncpy(hot_bad_cache[hot_bad_count], name, ROM_PATH_MAX - 1);
  hot_bad_cache[hot_bad_count][ROM_PATH_MAX - 1] = '\0';
  hot_bad_count++;
  hot_bad_save();
}

static void hot_bad_remove(const char *name) {
  int i = hot_bad_find(name);
  if (i < 0) return;
  for (int j = i + 1; j < hot_bad_count; j++) strcpy(hot_bad_cache[j - 1], hot_bad_cache[j]);
  hot_bad_count--;
  hot_bad_save();
  Serial.printf("[CRASH-GUARD] '%s' a demarre normalement : marque '!' retiree.\n", name);
}

static bool cache_contains(const char *name) {
  for (int i = 0; i < bad_rom_cache_count; i++) {
    if (strcmp(bad_rom_cache[i], name) == 0) return true;
  }
  return false;
}

static void load_bad_list_into_cache() {
  bad_rom_cache_count = 0;
  String list = prefs.getString("badlist", "");
  int start = 0;
  while (start < (int)list.length() && bad_rom_cache_count < MAX_BAD_ROMS) {
    int nl = list.indexOf('\n', start);
    String entry = (nl == -1) ? list.substring(start) : list.substring(start, nl);
    if (entry.length() > 0) {
      strncpy(bad_rom_cache[bad_rom_cache_count], entry.c_str(), ROM_PATH_MAX - 1);
      bad_rom_cache[bad_rom_cache_count][ROM_PATH_MAX - 1] = '\0';
      bad_rom_cache_count++;
    }
    if (nl == -1) break;
    start = nl + 1;
  }
}

static void add_to_bad_list(const char *rom_filename) {
  const char *name = strip_leading_slash(rom_filename);
  if (name[0] == '\0' || cache_contains(name)) {
    return;  // nom vide ou deja present, rien a faire
  }

  String list = prefs.getString("badlist", "");
  String addition = String(name) + "\n";

  // Si l'ajout depasserait la taille max reservee, on retire l'entree la
  // plus ancienne (premiere ligne) avant d'ajouter la nouvelle - evite une
  // croissance sans fin de cette chaine en NVS au fil des essais successifs.
  while (list.length() + addition.length() > BAD_LIST_MAX_LEN && list.indexOf('\n') != -1) {
    int nl = list.indexOf('\n');
    list = list.substring(nl + 1);
  }

  list += addition;
  prefs.putString("badlist", list);
  load_bad_list_into_cache();

  Serial.printf("[CRASH-GUARD] '%s' ajoute a la liste noire (%d ROM(s) au total).\n",
                name, bad_rom_cache_count);
}

void crash_guard_init() {
  prefs.begin(CRASH_GUARD_NAMESPACE, false);

  load_bad_list_into_cache();
  hot_bad_load();
  Serial.printf("[CRASH-GUARD] %d ROM(s) deja marquee(s) comme defaillante(s) en memoire.\n",
                bad_rom_cache_count + hot_bad_count);

  bool was_launching = prefs.getBool("launching", false);
  bool was_hot = prefs.getBool("hot", false);
  const bool was_resume = prefs.getBool("resume", false);
  if (was_launching) {
    String bad_rom = prefs.getString("cur_rom", "");
    if (bad_rom.length() > 0 && was_resume) {
      // Plantage juste apres le chargement d'une reprise automatique : la
      // reprise est en cause plus probablement que le jeu (qui avait tourne
      // pour la creer) - elle sera effacee, le jeu reste.
      Serial.printf("[CRASH-GUARD] Redemarrage pendant la reprise de '%s' -> reprise effacee, jeu garde.\n", bad_rom.c_str());
      strncpy(pending_drop_state, bad_rom.c_str(), sizeof(pending_drop_state) - 1);
      pending_drop_state[sizeof(pending_drop_state) - 1] = '\0';
    } else if (bad_rom.length() > 0 && was_hot) {
      // Plantage pendant un changement de jeu sans redemarrage - ROM seulement
      // marquee "!" (voir hot_bad_cache).
      Serial.printf("[CRASH-GUARD] Redemarrage pendant un changement de jeu (telephone) vers '%s' -> jeu marque '!' (pas supprime).\n",
                    bad_rom.c_str());
      hot_bad_add(strip_leading_slash(bad_rom.c_str()));
    } else if (bad_rom.length() > 0) {
      // Le stockage n'est pas encore monte : on memorise seulement le chemin ;
      // crash_guard_process_pending_deletion() fera la mise en quarantaine.
      Serial.printf("[CRASH-GUARD] Redemarrage survenu pendant le lancement de '%s' -> suppression prevue (carte SD pas encore montee).\n",
                    bad_rom.c_str());
      strncpy(pending_delete_rom, bad_rom.c_str(), sizeof(pending_delete_rom) - 1);
      pending_delete_rom[sizeof(pending_delete_rom) - 1] = '\0';
    } else {
      Serial.println("[CRASH-GUARD] Redemarrage inattendu detecte (nom de ROM inconnu, rien a supprimer).");
    }
    prefs.putBool("launching", false);
  }
  if (was_hot) prefs.putBool("hot", false);
  if (was_resume) prefs.putBool("resume", false);

  wdt_armed = false;
  boot_confirmed = true;
}

// Voir crash_guard.h pour le principe complet.
void crash_guard_process_pending_deletion() {
  // --- 1) ROM fautive detectee a ce demarrage (voir crash_guard_init() ci-dessus) ---
  // Reprise automatique en cause : effacee
  if (pending_drop_state[0] != '\0') {
    const char *p;
    const int where = rom_storage_split(pending_drop_state, &p);
    lib_save_remove(p[0] == '/' ? p + 1 : p, where, "sta");
    char nm[64];
    display_name(pending_drop_state, nm, sizeof(nm));
    snprintf(s_notice, sizeof(s_notice), "La reprise de \xC2\xAB %s \xC2\xBB a fait planter la console : elle est effac\xC3\xA9" "e, le jeu reste.", nm);
    pending_drop_state[0] = '\0';
  }

  if (pending_delete_rom[0] != '\0') {
    bool ok = rom_storage_quarantine(pending_delete_rom);
    if (ok) {
      Serial.printf("[CRASH-GUARD] '%s' mis de cote (dossier quarantaine).\n", pending_delete_rom);
      char nm[64];
      display_name(pending_delete_rom, nm, sizeof(nm));
      snprintf(s_notice, sizeof(s_notice), "\xC2\xAB %s \xC2\xBB a plant\xC3\xA9 au d\xC3\xA9marrage : jeu mis de c\xC3\xB4t\xC3\xA9 (R\xC3\xA9glages > Jeux \xC3\xA9" "cart\xC3\xA9s).", nm);
    } else {
      // Echec (carte protegee en ecriture, systeme de fichiers en erreur...) : la
      // ROM est seulement marquee "!" dans la liste.
      Serial.printf("[CRASH-GUARD] echec de la suppression de '%s' - ROM marquee '!' dans le menu a la place.\n",
                    pending_delete_rom);
      add_to_bad_list(pending_delete_rom);
    }
    pending_delete_rom[0] = '\0';
  }

  // --- 2) ROMs encore marquees "!" lors de demarrages precedents ---
  // Mises en quarantaine a leur tour (le stockage est monte ici) et retirees de
  // la liste noire en cas de succes ; sinon elles restent marquees.
  if (bad_rom_cache_count > 0) {
    String kept_list = "";
    int removed_count = 0;
    for (int i = 0; i < bad_rom_cache_count; i++) {
      // "flash:/nom.nes" (memoire interne) ou "nom.nes" (carte SD, sans '/').
      String full_path = strncmp(bad_rom_cache[i], ROM_FLASH_PREFIX, ROM_FLASH_PREFIX_LEN) == 0
                           ? String(bad_rom_cache[i]) : "/" + String(bad_rom_cache[i]);
      if (rom_storage_quarantine(full_path.c_str())) {   // mis de cote
        Serial.printf("[CRASH-GUARD] (menage) '%s' mis de cote.\n", bad_rom_cache[i]);
        removed_count++;
        continue;  // ne pas la garder dans la liste noire
      }
      // Fichier deja absent OU deplacement impossible : l'entree est gardee
      // (inoffensif si le fichier n'existe plus : il n'est pas liste).
      kept_list += String(bad_rom_cache[i]) + "\n";
    }
    if (removed_count > 0) {
      prefs.putString("badlist", kept_list);
      load_bad_list_into_cache();
      Serial.printf("[CRASH-GUARD] menage termine : %d ancienne(s) ROM(s) marquee(s) supprimee(s), %d restante(s) en liste noire.\n",
                    removed_count, bad_rom_cache_count);
    }
  }
}

// Partie commune aux deux lancements (au demarrage, ou changement de jeu sans
// redemarrage). "hot" est ecrit AVANT "launching" : apres un plantage,
// crash_guard_init() lit toujours un "hot" a jour.
static void begin_launch_common(const char *rom_path, bool hot) {
  prefs.putBool("hot", hot);
  prefs.putString("cur_rom", rom_path);
  prefs.putBool("launching", true);
  strncpy(launch_rom, strip_leading_slash(rom_path), sizeof(launch_rom) - 1);
  launch_rom[sizeof(launch_rom) - 1] = '\0';

  boot_confirmed = false;
  launch_started_ms = millis();

  // Changement de jeu dans les premieres secondes du jeu precedent (pas encore
  // confirme) : la tache est deja surveillee. Un second esp_task_wdt_add()
  // rendrait ESP_ERR_INVALID_ARG ("deja abonnee") : on garde simplement
  // l'abonnement en cours.
  if (wdt_armed) {
    esp_task_wdt_reset();
    Serial.printf("[CRASH-GUARD] Lancement de '%s'%s - filet de securite toujours arme.\n",
                  rom_path, hot ? " (sans redemarrage)" : "");
    return;
  }

  // Filet de securite pour un blocage TOTAL (aucune image jamais affichee) -
  // voir crash_guard.h.
  //
  // ESP_ERR_INVALID_STATE sur l'un ou l'autre des deux appels ci-dessous
  // signifie simplement "deja initialise"/"deja abonne" - le framework
  // Arduino-ESP32 peut deja avoir mis en place un chien de garde par
  // defaut avant meme setup(). Dans ce cas, on continue normalement : notre
  // propre nourrissage (esp_task_wdt_reset() dans crash_guard_confirm_boot())
  // couvre les deux situations indifferemment.
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = (uint32_t)STARTUP_WDT_TIMEOUT_S * 1000,
    .idle_core_mask = 0,   // ne surveille pas les taches "idle" du systeme, seulement la notre (ajoutee juste apres)
    .trigger_panic = true
  };
  esp_err_t wdt_init_err = esp_task_wdt_init(&wdt_config);
  if (wdt_init_err != ESP_OK && wdt_init_err != ESP_ERR_INVALID_STATE) {
    Serial.printf("[CRASH-GUARD] esp_task_wdt_init() a echoue (%d) - filet de securite desactive pour ce lancement.\n",
                  (int)wdt_init_err);
    wdt_armed = false;
    return;
  }

  esp_err_t wdt_add_err = esp_task_wdt_add(NULL);
  // ESP_ERR_INVALID_ARG = tache deja abonnee (ESP-IDF 5.x) : le filet est en
  // place, on le considere comme arme.
  if (wdt_add_err != ESP_OK && wdt_add_err != ESP_ERR_INVALID_STATE && wdt_add_err != ESP_ERR_INVALID_ARG) {
    Serial.printf("[CRASH-GUARD] esp_task_wdt_add() a echoue (%d) - filet de securite desactive pour ce lancement.\n",
                  (int)wdt_add_err);
    wdt_armed = false;
    return;
  }

  wdt_armed = true;
  Serial.printf("[CRASH-GUARD] Lancement de '%s'%s - filet de securite arme (%ds).\n",
                rom_path, hot ? " (sans redemarrage)" : "", STARTUP_WDT_TIMEOUT_S);
}

void crash_guard_begin_launch(const char *rom_path) {
  begin_launch_common(rom_path, false);
}

void crash_guard_begin_launch_hot(const char *rom_path) {
  begin_launch_common(rom_path, true);
}

void crash_guard_confirm_boot() {
  if (wdt_armed) {
    esp_task_wdt_reset();  // nourrit le chien de garde tant que des images arrivent
  }

  if (boot_confirmed) return;
  if (millis() - launch_started_ms < STARTUP_GRACE_MS) return;

  boot_confirmed = true;
  prefs.putBool("launching", false);
  prefs.putBool("resume", false);

  if (wdt_armed) {
    esp_task_wdt_delete(NULL);
    wdt_armed = false;
  }

  Serial.println("[CRASH-GUARD] Demarrage confirme (quelques secondes de jeu ecoulees) - ROM consideree comme saine.");
  hot_bad_remove(launch_rom);   // Marque "!" d'un plantage a chaud retiree
}

// Partie en pause (liste des jeux ouverte sur le telephone). Nourrit le chien
// de garde sans confirmer le lancement : seul le temps de jeu compte pour la
// confirmation (crash_guard_extend_grace() a la reprise).
void crash_guard_feed() {
  if (wdt_armed) esp_task_wdt_reset();
}

void crash_guard_extend_grace(unsigned long paused_ms) {
  if (!boot_confirmed) launch_started_ms += paused_ms;
}

// La reprise automatique va etre chargee (voir crash_guard.h)
void crash_guard_note_resume() {
  if (!boot_confirmed) prefs.putBool("resume", true);
}

// Jeu remis dans la liste (Jeux ecartes) : il n'est plus marque "!".
void crash_guard_forget(const char *rom_filename) {
  const char *name = strip_leading_slash(rom_filename);
  if (hot_bad_find(name) >= 0) hot_bad_remove(name);
  if (cache_contains(name)) {
    String kept = "";
    for (int i = 0; i < bad_rom_cache_count; i++)
      if (strcmp(bad_rom_cache[i], name) != 0) kept += String(bad_rom_cache[i]) + "\n";
    prefs.putString("badlist", kept);
    load_bad_list_into_cache();
  }
}

void crash_guard_clear_for_clean_exit() {
  prefs.putBool("launching", false);
  prefs.putBool("resume", false);
  if (wdt_armed) {
    esp_task_wdt_delete(NULL);
    wdt_armed = false;
  }
}

bool crash_guard_is_rom_bad(const char *rom_filename) {
  const char *name = strip_leading_slash(rom_filename);
  return cache_contains(name) || hot_bad_find(name) >= 0;   // + "hotbad"
}
