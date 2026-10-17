// wifi_portal.h - portail web de la console : gestion des jeux (envoi,
// suppression, deplacement), maintenance du stockage, mise a jour OTA. Sert
// la page sur le Wi-Fi deja actif (voir wifi_boot.h : ce module ne demarre
// PAS le Wi-Fi lui-meme, seulement le serveur HTTP).
//
// wifi_portal_loop() doit etre appelee regulierement (boucles des menus)
// pour servir les requetes.
#ifndef WIFI_PORTAL_H
#define WIFI_PORTAL_H

#include <stdint.h>

// Demarre le serveur web (port 80, WebServer.h) et enregistre ses routes
// (voir wifi_portal.cpp : "/" la page, "/api/games"/"/api/status" en
// lecture, "/api/upload"/"/api/delete" en ecriture). Ne fait rien de
// plus - suppose WiFi.status() deja connecte OU le Point d'acces deja
// demarre (wifi_boot_connect()).
void wifi_portal_start();

// A appeler a CHAQUE tour de la boucle d'affichage tant que l'ecran
// "Portail actif" est montre (WebServer.h fonctionne par appels
// repetes a handleClient(), pas par interruption/callback asynchrone).
void wifi_portal_loop();

// Arrete le serveur HTTP. Le WiFi (Station ou Point d'acces) reste actif –
// c'est wifi_boot_shutdown_for_game() (wifi_boot.h) qui coupe le WiFi
// lui-meme, appelee separement juste avant de lancer un jeu.
void wifi_portal_stop();

// Nombre de requetes servies depuis la mise sous tension (page, listes,
// envois...), affiche dans l'onglet Portail. Chaque requete est aussi ecrite
// sur le port serie ("[PORTAIL] GET / de 192.168.4.2...").
uint32_t wifi_portal_request_count();

// Les boucles des menus la lisent a chaque tour : si elle vaut true, la liste
// des jeux est relue et redessinee, puis le drapeau est remis a false.
// Definie SANS "#if ENABLE_WIFI_PORTAL" (voir wifi_portal.cpp) pour rester
// utilisable sans garde partout ou elle est lue.
extern volatile bool g_portal_games_changed;
// Reseau de la box change depuis le portail : les menus coupent puis rallument
// le Wi-Fi (connexion au nouveau reseau). Remis a false par celui qui le
// traite.
extern volatile bool g_portal_wifi_reconnect;

#endif
