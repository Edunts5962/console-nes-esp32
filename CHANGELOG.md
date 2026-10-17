# Historique des versions

Versions du logiciel de la console (`firmware/nes_emulator`). La carte vidéo (`firmware/video_coprocessor`) a sa propre numérotation, indiquée entre parenthèses quand elle change.

## 2.20.1 — 2026-10-11
- Mise à jour du logiciel par le portail : l'en-tête du fichier `.bin` est contrôlé sur le premier morceau reçu seulement (les bons fichiers étaient refusés). Après une mise à jour, le Wi-Fi est rallumé au démarrage pour que la page du portail retrouve la console.

## 2.20 — 2026-10-11
- Sauvegardes : RAM de pile des cartouches (`/saves/<jeu>.sav`), sauvegarde instantanée sur 3 emplacements et reprise automatique en quittant le jeu.
- Menu pause en partie (écran ou télévision) : reprendre, sauvegarder, charger, volume, favori, jeux récents, favoris, tous les jeux, recommencer, menu de la console.
- Volume en jeu (SELECT + haut/bas) et depuis le téléphone ; favoris, jeux récents et « Reprendre » sur la console, le téléphone et la télévision ; « Aller à la page ».
- Un jeu qui plante au lancement est mis de côté (dossier `/quarantaine`) au lieu d'être supprimé ; Réglages > Jeux écartés et le portail permettent de le récupérer.
- Page du téléphone : un seul bouton CONNECTER, écran gardé allumé, page installable, réglage du volume.
- Aide « Commandes » dans Réglages, aide-mémoire imprimable (`docs/aide_memoire.html`).
- La ROM et les graphismes en ROM ne sont plus modifiables par le jeu.

## 2.19 — 2026-10-11
- Le téléphone reçoit la liste complète des jeux (jusqu'à 1000) avec recherche ; il la garde et ne la redemande que si elle a changé.
- Portail : formatage de la carte SD toujours en FAT32, type de FAT et remplissage de la racine affichés, envoi refusé clairement quand la racine est pleine (cartes FAT16) (2.19.1).

## 2.18 — 2026-10-11
- Jusqu'à 1000 jeux, lus dans un catalogue en PSRAM et affichés par pages de 50 ; recherche sur tous les jeux.
- Mappers : MMC3 (compteur d'interruptions), MMC1 (cartouches de 512 Ko) et MMC5 refaits ; nouveaux mappers 69, 71, 113, 118, 232 ; mappers 10, 41, 42, 46, 50, 73, 87, 93, 160, 229 activés.

## 2.17 — 2026-10-10
- Norme PAL 60 (couleur PAL à 60 images/s) ; liste des jeux sur la télévision aux couleurs adaptées au composite (carte vidéo 3.1).

## 2.16 — 2026-10-10
- Qualité d'image sur la télévision : bandes compressées, image progressive au choix, sorties de la liaison moins puissantes, rafraîchissement plus fréquent, palette 24 bits exacte (protocole 3, carte vidéo 3.0).

## 2.15 — 2026-10-10
- Wi-Fi coupé au démarrage (allumé à la demande dans Réglages) ; Bluetooth complètement arrêté tant que le Wi-Fi est allumé.
- En sortie Téléviseur, MENU JEUX du téléphone ouvre la liste sur la télévision.

## 2.14 — 2026-10-10
- Liste des jeux sur la télévision (dans les menus et en partie, SELECT + START tenus 1 s).
- Liaison vidéo activée par défaut, sur le bus SPI2.
- Page du téléphone : croix directionnelle d'un seul tenant.

## 2.13 — 2026-10-04
- Liste des jeux et changement de jeu depuis le téléphone, sans redémarrer la console ; contrôle des ROMs avant lancement.

## 2.8 à 2.12 — 2026-10-03
- Manette du téléphone en Bluetooth LE (page `manette8.html`) : bouton MENU JEUX, choix du jeu, icône Bluetooth, jeu en plein écran sur le LCD, couleurs des jeux corrigées sur l'écran ; cohabitation avec le point d'accès Wi-Fi.

## 2.5 — 2026-09-27
- Liaison vidéo à 10 MHz par bandes numérotées avec somme de contrôle ; carte vidéo réécrite (couleur PAL/NTSC avec LovyanGFX, son PWM sur GPIO26) (carte vidéo 2.0).
- Manette NES à CD4021 ; projets KiCad de la carte porteuse et de la manette.

## 2.0 à 2.4 — 2026-09-25/26
- Menus refaits avec LVGL 8 : liste des jeux, Réglages, Vidéo, Portail ; luminosité, thèmes de couleurs, recherche, fiche d'un jeu.
- Étalonnage et test du tactile, QR codes du portail.
- Jeux en mémoire interne (partition FAT de la flash) en plus de la carte SD ; portail : deux emplacements, déplacement, maintenance.
- Mode Wi-Fi : Auto, Box ou Point d'accès.

## 1.x — 2026-09-23/25
- Portage du dépôt d'origine sur la carte ES3C28P : écran ILI9341 (TFT_eSPI), carte SD en SD_MMC, son par le codec ES8311, zones tactiles, rétroéclairage.
- Détection des jeux qui plantent au lancement, retour au menu par SELECT maintenu 3 s.
- Connexion Wi-Fi et portail web d'envoi de jeux ; volume mémorisé.
