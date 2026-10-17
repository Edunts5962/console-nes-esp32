// wifi_boot.h - Wi-Fi de la console : connexion a la box (mode Station),
// avec repli automatique en point d'acces "NES-WiFi" (identifiants dans
// wifi_config.h), badge de statut sur l'ecran du logo, allumage / coupure a
// la demande depuis Reglages, mode de connexion memorise.
//
// Le portail web (wifi_portal.h) utilise l'etat defini ici
// (g_wifi_boot_status) pour savoir s'il peut demarrer.
//
// ENABLE_WIFI_PORTAL a 0 : aucun Wi-Fi (ni WiFi.begin(), ni portail).
#ifndef WIFI_BOOT_H
#define WIFI_BOOT_H

#define ENABLE_WIFI_PORTAL 1

// WIFI_START_AT_BOOT :
//   0 (defaut) : le Wi-Fi reste COUPE a chaque demarrage - Reglages > Wi-Fi
//       l'allume quand il sert (portail, envoi de jeux) ; le logo ne dure
//       pas le temps d'une connexion, et le Bluetooth demarre tout de suite ;
//   1 : connexion pendant le logo (mode memorise dans Reglages : box, point
//       d'acces ou automatique).
// Le Wi-Fi s'allume a tout moment depuis Reglages ; le Bluetooth est alors
// arrete tant que le Wi-Fi est allume, voir wifi_boot_enable() et
// bluetooth_controller_stop_for_wifi().
#define WIFI_START_AT_BOOT 0

// Etats possibles, mis a jour par wifi_boot_connect() - lus par osd.cpp
// (rien pour l'instant au-dela du badge de demarrage) et plus tard par
// wifi_portal.cpp.
enum wifi_boot_status_t {
  WIFI_BOOT_DISABLED = 0,   // ENABLE_WIFI_PORTAL == 0, jamais tente
  WIFI_BOOT_CONNECTING,     // tentative Station en cours
  WIFI_BOOT_STATION_OK,     // connecte a la box, IP obtenue
  WIFI_BOOT_AP_FALLBACK,    // repli en Point d'acces reussi
  WIFI_BOOT_FAILED          // ni Station ni Point d'acces n'ont fonctionne
};

extern wifi_boot_status_t g_wifi_boot_status;

// IP obtenue en mode Station (ex. "192.168.1.42"), ou SSID du point
// d'acces cree en repli (WIFI_AP_SSID) - chaine vide si
// WIFI_BOOT_DISABLED ou WIFI_BOOT_FAILED. C'est ce texte que le badge de
// l'ecran de demarrage affiche.
extern char g_wifi_boot_status_text[32];

// A appeler une seule fois au demarrage, PENDANT l'ecran de logo
// (show_magic() dans osd.cpp - c'est elle qui dessine son propre badge
// de progression/resultat directement sur l'ecran pendant l'appel, voir
// wifi_boot.cpp). Bloquante jusqu'a WIFI_STA_TIMEOUT_MS
// (wifi_config.h) + une seconde environ dans le pire cas (echec Station
// PUIS tentative de Point d'acces) - assume et documente dans
// wifi_boot.cpp, PAS un bug.
void wifi_boot_connect();

// A appeler juste avant de lancer une partie (nes_emulator.ino, juste avant
// nofrendo_main()) - coupe completement le WiFi (WiFi.mode(WIFI_OFF), Station
// ET Point d'acces) pour liberer le CPU/RAM pendant l'emulation. Sans effet si
// ENABLE_WIFI_PORTAL vaut 0 (rien n'a ete allume au demarrage dans ce cas).
void wifi_boot_shutdown_for_game();

// Allume / coupe le Wi-Fi pendant que la console tourne (Reglages).
// wifi_boot_disable() a le meme effet que wifi_boot_shutdown_for_game()
// (radio coupee, box ET point d'acces). wifi_boot_enable() refait la meme
// tentative qu'au demarrage (box puis repli point d'acces selon le mode),
// sans le badge de l'ecran de logo : BLOQUANTE jusqu'a WIFI_STA_TIMEOUT_MS +
// ~800 ms dans le pire cas, l'appelant doit prevenir l'utilisateur AVANT
// l'appel.
void wifi_boot_enable();

// Mode de connexion, choisi dans Reglages, memorise en NVS (espace "neswifi",
// cle "mode") et applique au demarrage comme a chaque activation :
//  - NESWIFI_MODE_AUTO : la box d'abord (WIFI_SSID, WIFI_STA_TIMEOUT_MS),
//    puis le point d'acces NES-WiFi si elle ne repond pas (valeur par
//    defaut) ;
//  - NESWIFI_MODE_BOX : la box uniquement ; si elle ne repond pas, le Wi-Fi
//    reste coupe (etat WIFI_BOOT_FAILED, texte "box injoignable") ;
//  - NESWIFI_MODE_AP : le point d'acces NES-WiFi directement, sans essayer
//    la box (demarrage plus rapide ; etat WIFI_BOOT_AP_FALLBACK, qui
//    signifie "point d'acces actif", par choix ou par repli).
// wifi_boot_set_mode() ne fait que memoriser : c'est a l'appelant de
// relancer le Wi-Fi s'il est actif (ui_lvgl.cpp le fait).
// Prefixe NESWIFI_ obligatoire : les noms WIFI_MODE_AP, WIFI_MODE_STA...
// sont deja ceux de l'enumeration wifi_mode_t d'ESP-IDF (utilisee par
// WiFi.mode(WIFI_AP)) ; les reprendre ici empeche la compilation.
enum wifi_mode_pref_t {
  NESWIFI_MODE_AUTO = 0,
  NESWIFI_MODE_BOX = 1,
  NESWIFI_MODE_AP = 2
};
wifi_mode_pref_t wifi_boot_get_mode();
void wifi_boot_set_mode(wifi_mode_pref_t mode);
void wifi_boot_disable();

//  - wifi_boot_ap_clients() : nombre d'appareils (telephone, PC...) relies au
//    point d'acces NES-WiFi en ce moment ; 0 hors point d'acces. Affiche dans
//    l'onglet Portail : un appareil qui n'y apparait pas n'est pas sur le
//    reseau de la console.
//  - wifi_boot_log_memory() : ligne "[MEM]" sur le port serie (memoire
//    interne libre, plus gros bloc, minimum atteint depuis le demarrage,
//    PSRAM libre). Le Wi-Fi et le Bluetooth prennent leurs tampons dans la
//    memoire interne, pas dans la PSRAM.
// Les evenements du point d'acces (appareil relie, adresse donnee, appareil
// parti) et la raison d'un echec de connexion a la box sont aussi ecrits sur
// le port serie, prefixe "[WIFI]".
int wifi_boot_ap_clients();

// Reseau de la box sans recompiler : enregistre depuis le portail web (NVS
// "neswifi"), prioritaire sur WIFI_SSID / WIFI_PASSWORD de wifi_config.h, qui
// restent la valeur par defaut.
const char *wifi_boot_sta_ssid();        // reseau utilise (enregistre ou wifi_config.h)
bool wifi_boot_sta_from_portal();        // true : enregistre depuis le portail
// 0 : enregistre ; -1 : nom invalide (1 a 32 octets) ; -2 : mot de passe
// invalide (vide ou 8 a 63 caracteres) ; -3 : ecriture impossible.
int wifi_boot_set_credentials(const char *ssid, const char *pass);
void wifi_boot_forget_credentials();     // retour a wifi_config.h
// Le Wi-Fi sera rallume au prochain demarrage (apres une mise a jour par le
// portail ; une seule fois).
void wifi_boot_keep_on_after_restart();
void wifi_boot_log_memory(const char *when);

#endif
