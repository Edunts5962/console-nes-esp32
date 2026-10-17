# Manette du téléphone : protocole Bluetooth

La page [`docs/manette/manette8.html`](manette/manette8.html) transforme un téléphone Android en manette NES. Elle se connecte en **Bluetooth basse consommation** (Web Bluetooth) à l'ESP32-S3 de la console. Ses boutons s'ajoutent (OU logique) à ceux de la manette filaire et des zones tactiles.

- Nom BLE : `NES-ESP32S3`
- Service : `0000fff0-0000-1000-8000-00805f9b34fb`
- Caractéristique `0000fff1-…` : *Write / Write Without Response* — états de la manette et commandes du téléphone
- Caractéristique `0000fff2-…` : *Notify* — réponses de la console (liste des jeux, état, événements)
- Activation à la compilation : `ENABLE_BLE_GAMEPAD` dans `firmware/nes_emulator/hw_config.h` (1 par défaut)

Côté console : `bluetooth_controller.cpp/.h` (liaison) et `games_link.cpp/.h` (messages de la console, format détaillé en tête de `games_link.h`).

## Ouvrir la page

Web Bluetooth demande **Chrome sur Android** et une page servie en **HTTPS** (ou depuis `localhost`). Le plus simple : activer GitHub Pages sur ce dépôt, dossier `/docs` (voir le README), et ouvrir `https://<pseudo>.github.io/console-nes-esp32/manette/manette8.html` (le dossier `docs/` n'apparaît pas dans l'adresse).

La page est **installable** : servie avec `manette8.webmanifest` et les icônes `manette8-icon-*.png` à côté, Chrome > ⋮ > « Ajouter à l'écran d'accueil » l'ouvre ensuite comme une application. L'écran du téléphone reste allumé tant que la manette est connectée.

## Trames écrites sur FFF1

| Trame | Rôle | Envoyée par la page |
|---|---|---|
| 1 octet `m` | état complet de la manette (masque ci-dessous) | à chaque appui ou relâchement, puis toutes les 100 ms tant qu'une touche est tenue |
| `FD 01 00` | MENU : retour à la liste des jeux de la console (redémarrage si une partie est en cours) | bouton « MENU DE LA CONSOLE » (deux appuis) |
| `FD 02 00` | contrôle de liaison, écrit **avec** accusé de réception ; ignoré par la console | toutes les secondes (voyant de liaison) |
| `FD 03 00` | LISTE : page de 50 jeux affichée par la console | ouverture du panneau des jeux (consoles anciennes) |
| `FD 04 gén n_lo n_hi 00` | LANCER le jeu n de la page numéro `gén` | bouton LANCER (consoles anciennes) |
| `FD 05 p 00` | PAUSE (`p` = 1) / REPRISE (`p` = 0) | ouverture / fermeture du panneau |
| `FD 06 00` | LISTE TV : liste des jeux sur la télévision, ou reprise si elle est ouverte | MENU JEUX quand la console est en sortie Téléviseur |
| `FD 07 00` | LISTE COMPLÈTE : tous les jeux du catalogue (1000 au plus) | ouverture du panneau |
| `FD 07 gén crc_lo crc_hi 00` | LISTE COMPLÈTE, la page ayant déjà la liste `gén` / `crc` : rien n'est renvoyé si elle n'a pas changé | réouverture du panneau |
| `FD 08 gén n_lo n_hi 00` | LANCER le jeu n du catalogue `gén` | bouton LANCER |
| `FD 09 gén n_lo n_hi 00` | FAVORI : ajoute ou retire le jeu n | étoile d'une ligne |
| `FD 0A vol 00` | VOLUME de la console, 0 à 200 % | curseur « Volume de la console » |
| `FD 0B gén n_lo n_hi 00` | REPRENDRE : lance le jeu avec sa partie gardée (sinon comme `FD 08`) | carte « Reprendre » |

Masque des boutons : A = `0x01`, B = `0x02`, SELECT = `0x04`, START = `0x08`, HAUT = `0x10`, BAS = `0x20`, GAUCHE = `0x40`, DROITE = `0x80`. Les diagonales combinent deux directions (haut-gauche = `0x50`…).

Un octet seul est toujours un état de manette, même `0xFD` ou `0xFE` : les préfixes ne comptent que dans une trame d'au moins deux octets. Le `00` final des commandes fait qu'un programme qui lirait le dernier octet comme un état de manette n'y verrait qu'un relâchement. Une trame `FE n` (ancien curseur « vitesse ») est ignorée.

## Messages de la console sur FFF2

Résumé (format exact en tête de `firmware/nes_emulator/games_link.h`) :

| Type | Message |
|---|---|
| `0x01` | ÉTAT : menu / partie / pause, jeu en cours, numéro de liste, sortie Téléviseur, numéro du catalogue, nombre total de jeux, volume |
| `0x02`–`0x04` | page de 50 jeux : début, morceaux, fin (CRC-16/CCITT-FALSE) |
| `0x05` | ÉVÉNEMENT : chargement, jeu lancé, refus (raison : liste périmée, fichier introuvable, pas une ROM NES, mapper non pris en charge, échec de chargement) |
| `0x06`–`0x08` | liste COMPLÈTE : début, morceaux (position sur 3 octets), fin (CRC) |
| `0x09` | bibliothèque : favoris, récents, jeu à reprendre |

Texte d'une liste : une ligne par jeu, `drapeaux<TAB>Ko<TAB>nom\n` ; drapeaux `S` (carte SD) ou `F` (mémoire interne), suivi de `!` si la ROM est marquée comme fautive.

## Sécurité

- Sans trame d'état pendant **500 ms**, la console relâche tous les boutons (la page renvoie l'état toutes les 100 ms : 5 trames perdues sont tolérées).
- Déconnexion, page masquée ou téléphone en veille : tout est relâché.

## Utilisation

**Dans les menus de la console** (onglet Jeux, sans fiche, recherche ni écran QR ouverts) :

| Bouton | Effet |
|---|---|
| Croix haut / bas | jeu précédent / suivant (maintenue : défile) |
| Croix gauche / droite | page précédente / suivante |
| A ou START | lancer le jeu sous le curseur |
| SELECT | liste suivante : Tous → Favoris → Récents |

**Pendant une partie** : manette NES complète. Le bouton **MENU JEUX** ouvre sur le téléphone le panneau des jeux (onglets Tous / Favoris / Récents, recherche, étoiles, carte « Reprendre ») ; la partie est en pause tant qu'il est ouvert. **LANCER** change de jeu **sans redémarrer** la console. En sortie Téléviseur, MENU JEUX ouvre la liste sur la télévision.

**Jeu en plein écran** : dès que le téléphone est connecté, la partie occupe tout l'écran LCD (320 × 240) ; les zones tactiles sont alors désactivées, sauf la zone SELECT invisible du coin bas-droit (maintien 3 s = retour au menu). Réglages `LCD_FULLSCREEN_*` dans `hw_config.h`.

### Croix directionnelle

Croix d'un seul tenant, comme sur la manette d'origine : on fait glisser le pouce d'une direction à l'autre sans le lever ; une diagonale s'obtient entre deux branches (secteur de 34°) ; le centre est neutre. Réglages en tête du bloc « CROIX DIRECTIONNELLE GLISSANTE » du script : `DPAD_DEAD_ZONE`, `DPAD_DIAGONAL_HALF`, `DPAD_HYSTERESIS`, `DPAD_TILT`.

### Voyant de liaison

| Voyant | Signification |
|---|---|
| vert « Bluetooth actif » | la console accuse réception du contrôle envoyé chaque seconde |
| orange « Pas de réponse » | plus d'accusé depuis 2,5 s |
| orange « Reconnexion… » | liaison coupée, la page se reconnecte seule (essai toutes les 2 s pendant 1 minute) |
| rouge « Liaison perdue » | reconnexion impossible : « CONNECTER » |
| gris « Non connecté » | aucune connexion, ou QUITTER |

Sur la console, l'icône Bluetooth de la barre du haut est colorée quand le téléphone est connecté, grisée sinon, **orange** quand le Bluetooth attend la coupure du Wi-Fi.

## Bluetooth et Wi-Fi

L'ESP32-S3 n'a pas assez de mémoire interne pour faire tourner ensemble le Bluetooth et le Wi-Fi (box ou point d'accès) :

- Wi-Fi **coupé au démarrage** (`WIFI_START_AT_BOOT` = 0, défaut) : la manette du téléphone marche tout de suite, dans les menus et en partie.
- Wi-Fi **allumé** dans Réglages : le Bluetooth est **complètement arrêté** (téléphone déconnecté, icône orange). Le portail se pilote au tactile.
- Wi-Fi **coupé** dans Réglages, ou **lancement d'un jeu** (la console coupe alors le Wi-Fi) : le Bluetooth redémarre et la page se reconnecte d'elle-même.

## Limites

- Web Bluetooth : Chrome sur Android, page en HTTPS ou en local. Safari (iPhone, iPad) ne le propose pas.
- Les onglets Réglages, Vidéo et Portail se pilotent au doigt ou à la manette filaire.
