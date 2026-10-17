// bluetooth_controller.h - manette virtuelle du telephone (Bluetooth LE).
#pragma once
#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

// Manette virtuelle du telephone (page manette8.html, Web Bluetooth).
// Nom BLE : NES-ESP32S3
// Service : 0000fff0-0000-1000-8000-00805f9b34fb
// Caracteristique : 0000fff1-0000-1000-8000-00805f9b34fb
//                   (Write / Write Without Response)
//
// Trames acceptees sur FFF1 (detail dans docs/protocole_bluetooth.md) :
//   1 octet            etat complet de la manette (masque ci-dessous)
//   0xFE, n            curseur "vitesse" de la page (1..15) : sans objet
//                      pour l'emulateur, ignore
//   0xFD, code, 0x00   commande systeme ; code 0x01 = MENU (retour a
//                      l'ecran de selection des jeux) ; code 0x02 = controle
//                      de liaison de la page (ecrit avec accuse de reception,
//                      rien a faire : l'accuse suffit au voyant de la page)
//   Liste des jeux et changement de jeu sans redemarrer :
//   0xFD, 0x03, 0x00                LISTE : envoyer la liste des jeux
//   0xFD, 0x04, g, il, ih, 0x00     LANCER le jeu n° il + 256 x ih de la
//                                   liste numero g (generation, voir
//                                   osd_games_generation())
//   0xFD, 0x05, p, 0x00             PAUSE (p = 1) ou REPRISE (p = 0)
//   0xFD, 0x06, 0x00                LISTE TV
//   Liste COMPLETE (tous les jeux, plus seulement la page de 50) :
//   0xFD, 0x07, 0x00                LISTE COMPLETE
//   0xFD, 0x07, g, cl, ch, 0x00     LISTE COMPLETE, le telephone ayant deja
//                                   celle de numero g et de CRC cl + 256 x ch
//                                   (rien a renvoyer si elle n'a pas change)
//   0xFD, 0x08, g, il, ih, 0x00     LANCER le jeu n° il + 256 x ih du
//                                   catalogue numero g (osd_catalog_generation())
// Reponses de la console : caracteristique FFF2 (notifications), format
// dans games_link.h.
// Desactivable par ENABLE_BLE_GAMEPAD (hw_config.h) : les fonctions
// ci-dessous restent alors appelables et ne font rien.
void bluetooth_controller_init();

// Masque NES recu du telephone.
// Bit 0=A, 1=B, 2=SELECT, 3=START, 4=UP, 5=DOWN, 6=LEFT, 7=RIGHT.
// Retourne 0 sans connexion, ou si aucune trame d'etat n'est arrivee depuis
// le delai de securite (la page renvoie l'etat des touches tenues toutes
// les 100 ms).
uint8_t bluetooth_controller_get_state();

bool bluetooth_controller_is_connected();

// Bouton "MENU JEUX" de la page : vrai UNE fois par demande (la lecture efface
// la demande). Lu par osd.cpp pendant une partie (retour au menu par
// redemarrage, comme SELECT maintenu 3 s) et par ui_lvgl.cpp dans les menus
// (retour a l'onglet Jeux).
bool bluetooth_controller_take_menu_request();

// A appeler juste avant ESP.restart() : coupe la liaison pour que le telephone
// voie la deconnexion tout de suite (apres un MENU, la page se reconnecte
// alors d'elle-meme au redemarrage de la console).
void bluetooth_controller_disconnect_before_restart();

// ---- liste des jeux et changement de jeu depuis le telephone ----
// Demandes du telephone, dans l'ordre d'arrivee (8 au plus en attente) ;
// lire depuis la tache Arduino seulement. gen et index : pour LANCER.
#define BLE_GAMES_REQ_NONE    0
#define BLE_GAMES_REQ_LIST    1
#define BLE_GAMES_REQ_LAUNCH  2
#define BLE_GAMES_REQ_PAUSE   3
#define BLE_GAMES_REQ_RESUME  4
#define BLE_GAMES_REQ_TVLIST  5   // MENU JEUX en sortie Televiseur (liste sur la television)
// Liste COMPLETE (tous les jeux du catalogue, pas seulement la page affichee)
// et lancement par numero du catalogue (gen : osd_catalog_generation()).
#define BLE_GAMES_REQ_LIST_ALL       6   // le telephone n'a pas de liste
#define BLE_GAMES_REQ_LIST_ALL_HAVE  7   // il en a une : gen = son numero, index = son CRC
#define BLE_GAMES_REQ_LAUNCH_ALL     8   // gen, index : numero du catalogue
// Favori (bascule), volume de la console, reprise d'un jeu
#define BLE_GAMES_REQ_FAV            9   // gen, index : numero du catalogue
#define BLE_GAMES_REQ_VOLUME        10   // index : volume (0..200 %)
#define BLE_GAMES_REQ_RESUME_ALL    11   // gen, index : numero du catalogue, lance avec sa reprise
int bluetooth_controller_take_games_request(uint8_t *gen, uint16_t *index);
// Oublie les demandes de LANCEMENT en attente (au debut de chaque partie) ;
// les autres demandes (liste, pause, reprise) sont gardees, dans l'ordre.
void bluetooth_controller_flush_games_launches();
// Vrai si la page est connectee et abonnee aux notifications FFF2.
bool bluetooth_controller_games_ready();
// Taille maximale d'un message vers le telephone (MTU negociee - 3, 20 au
// moins).
size_t bluetooth_controller_max_message();
// Envoie un message au telephone (notification FFF2). Faux si impossible
// pour l'instant (tampons Bluetooth pleins, page pas abonnee...) : le
// renvoyer plus tard. Ne bloque jamais.
bool bluetooth_controller_send(const uint8_t *data, size_t len);

// Partage de la radio et de la memoire avec le Wi-Fi. Le Bluetooth garde sa
// memoire interne meme en pause, ce qui empeche le point d'acces ou la
// connexion a la box de fonctionner : il est donc completement ARRETE
// (BLEDevice::deinit(false), memoire rendue) tant que le Wi-Fi est allume
// (box, point d'acces ou connexion en cours). A appeler apres chaque
// changement possible de l'etat du Wi-Fi (fin du logo, boucles des menus,
// debut de partie avec game_start = true, fin de wifi_boot_enable()) :
//  - Wi-Fi allume : Bluetooth arrete s'il tournait (telephone deconnecte),
//    ou pas demarre ;
//  - Wi-Fi coupe : le Bluetooth (re)demarre - tout de suite au debut de la
//    partie, apres 2 s dans les menus (un changement de mode coupe le Wi-Fi
//    puis le rallume aussitot) ; la page du telephone se reconnecte seule ;
//  - demarrage rate : un seul nouvel essai, au debut de la partie.
// Sans effet si ENABLE_BLE_GAMEPAD vaut 0.
void bluetooth_controller_radio_check(bool game_start = false);

// A appeler juste AVANT d'allumer le Wi-Fi (wifi_boot_enable()) : arret
// immediat du Bluetooth (telephone deconnecte, au plus 0,6 s d'attente, puis
// pile arretee et memoire rendue) - bluetooth_controller_radio_check() ne
// tourne pas pendant la connexion (bloquante jusqu'a 10 s). Sans effet si le
// Bluetooth ne tourne pas.
void bluetooth_controller_stop_for_wifi();

// Vrai si le Bluetooth attend la coupure du Wi-Fi : le telephone ne peut alors
// pas se connecter. Icone Bluetooth orange dans la barre du haut des menus.
bool bluetooth_controller_waiting_for_wifi();
