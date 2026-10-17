// wifi_config.example.h - modele des identifiants Wi-Fi de la console.
//
// Copier ce fichier en "wifi_config.h" (meme dossier) et y mettre le nom et
// le mot de passe de votre box. wifi_config.h est ignore par git (.gitignore)
// : vos identifiants ne partent pas sur le depot. Sans wifi_config.h, le
// croquis compile avec ce modele (la connexion a la box echouera, le point
// d'acces de la console reste utilisable).
//
// Le reseau de la box peut aussi etre change sans recompiler, depuis le
// portail web (enregistre en NVS, prioritaire sur les valeurs ci-dessous).
#ifndef WIFI_CONFIG_H
#define WIFI_CONFIG_H

// Box / routeur (mode Station)
#define WIFI_SSID          "MonReseauWiFi"
#define WIFI_PASSWORD      "MonMotDePasse"

// Point d'acces ouvert par la console si la box ne repond pas (mode Auto) ou
// en mode "Point d'acces". Mot de passe WPA2 : 8 caracteres au moins -
// pensez a le changer.
#define WIFI_AP_SSID       "NES-WiFi"
#define WIFI_AP_PASSWORD   "nes12345"

// Delai maximal de connexion a la box avant le repli en point d'acces (ms).
#define WIFI_STA_TIMEOUT_MS 10000

#endif
