// games_link.h - liste des jeux et changement de jeu depuis la manette du
// telephone (page manette8.html), par-dessus la liaison Bluetooth de
// bluetooth_controller.h.
//
// Le telephone ecrit ses demandes sur FFF1 (voir bluetooth_controller.h) ;
// la console repond par des notifications sur FFF2. Chaque message commence
// par son type :
//
//   0x01 ETAT          [01, etat, jeu lo, jeu hi, generation, nombre, drapeaux,
//                       jeu cat lo, jeu cat hi, generation cat, total lo, total hi,
//                       volume]
//                      etat : 0 menu de la console, 1 partie en cours,
//                      2 partie en pause ; jeu : index du jeu en cours dans
//                      la page (0xFFFF : aucun) ; generation : numero de la
//                      liste (change a chaque relecture) ; nombre de jeux de
//                      la page ; drapeaux bit 0 : sortie Televiseur active
//                      (la page envoie alors LISTE TV au lieu d'ouvrir son
//                      panneau) ; jeu cat : jeu en cours en numero du
//                      CATALOGUE (0xFFFF : aucun) ; generation cat :
//                      osd_catalog_generation() (change a chaque relecture,
//                      PAS quand la page change) ; total : nombre total de
//                      jeux ; volume : volume de la console (0..200 %).
//                      Envoye a chaque changement et avant une liste.
//   0x02 DEBUT LISTE   [02, generation, nombre, longueur lo, longueur hi]
//   0x03 MORCEAU       [03, generation, position lo, position hi, texte...]
//   0x04 FIN LISTE     [04, generation, crc lo, crc hi]
//                      CRC-16/CCITT-FALSE (0x1021, depart 0xFFFF) du texte
//                      complet ; le telephone redemande la liste s'il ne
//                      retrouve pas ce CRC.
//   0x05 EVENEMENT     [05, code, a lo, a hi, cat lo, cat hi]
//                      1 chargement du jeu a ; 2 jeu a lance ; cat : meme jeu
//                      en numero du catalogue (0xFFFF si sans objet) ;
//                      3 refus : a lo = raison (GAMES_LINK_REFUSE_*),
//                        a hi = detail (numero de mapper pour
//                        GAMES_LINK_REFUSE_MAPPER ; pour
//                        GAMES_LINK_REFUSE_LOAD_FAILED, 1 = retour au jeu
//                        precedent, 0 = la console revient a son menu)
//
// Texte de la liste : une ligne par jeu, "drapeaux<TAB>Ko<TAB>nom\n" -
// drapeaux "S" (carte SD) ou "F" (memoire interne), suivi de "!" si la ROM
// est en liste noire ; nom du fichier tel quel (UTF-8). 50 jeux au plus.
//
// LISTE COMPLETE (catalogue, MAX_GAMES_TOTAL jeux au plus), demandee par
// [FD, 07, ...] (bluetooth_controller.h), meme texte, ordre du catalogue :
//   0x06 DEBUT         [06, gen cat, nombre lo, nombre hi, longueur (3 octets,
//                       poids faible d'abord), drapeaux]
//                      drapeaux bit 0 : le telephone a deja cette liste
//                      (meme numero, meme CRC) - pas de morceaux, FIN suit.
//   0x07 MORCEAU       [07, gen cat, position (3 octets), texte...]
//   0x08 FIN           [08, gen cat, crc lo, crc hi]
// Lancement : [FD, 08, gen cat, numero lo, numero hi, 00] ; la console
// affiche alors la page qui contient ce jeu.
//
// BIBLIOTHEQUE (favoris, recents, reprise), envoyee apres chaque liste
// complete et a chaque changement, en numeros du catalogue :
//   0x09               [09, gen cat, 0 | 1, total, depart, n lo, n hi, ...]
//                      0 : favoris (ordre alphabetique), 1 : recents (le
//                      dernier joue en tete) ; total : taille de la liste,
//                      depart : rang du premier numero du message (une
//                      liste vide : total 0, aucun numero).
//                      [09, gen cat, 2, drapeaux, n lo, n hi] : "Reprendre"
//                      (dernier jeu joue, 0xFFFF : aucun) ; drapeaux bit 0 :
//                      une reprise automatique existe.
// Demandes : [FD, 09, gen cat, n lo, n hi, 00] favori (bascule) ;
// [FD, 0A, volume, 00] ; [FD, 0B, gen cat, n lo, n hi, 00] lancer en
// reprenant la partie gardee (comme 0x08 sinon).
#ifndef GAMES_LINK_H
#define GAMES_LINK_H

#define GAMES_LINK_STATE_MENU    0
#define GAMES_LINK_STATE_GAME    1
#define GAMES_LINK_STATE_PAUSED  2

// Actions rendues par games_link_poll()
#define GAMES_LINK_NONE    0
#define GAMES_LINK_LAUNCH  1   // *index : jeu a lancer (liste a jour, index valide)
#define GAMES_LINK_PAUSE   2
#define GAMES_LINK_RESUME  3
#define GAMES_LINK_TVLIST  4   // Liste des jeux sur la television (MENU JEUX en sortie Televiseur)

// Raisons de refus (evenement 3)
#define GAMES_LINK_REFUSE_STALE        1   // liste perimee (relue depuis)
#define GAMES_LINK_REFUSE_UNKNOWN      2   // numero de jeu hors de la liste
#define GAMES_LINK_REFUSE_NOFILE       3   // fichier introuvable ou illisible
#define GAMES_LINK_REFUSE_NOT_NES      4   // pas une ROM NES, ou ROM incomplete
#define GAMES_LINK_REFUSE_MAPPER       5   // mapper non pris en charge (detail)
#define GAMES_LINK_REFUSE_LOAD_FAILED  6   // echec du chargement (detail : 1 retour au jeu precedent, 0 menu)

// A appeler souvent depuis la tache Arduino : boucle des menus LVGL, chaque
// image en partie, boucle de pause. Envoie la liste par morceaux quand elle
// est demandee, signale les changements d'etat, et rend la premiere demande
// a traiter par l'appelant. state : GAMES_LINK_STATE_* ; current : index du
// jeu en cours (-1 : aucun). Ne bloque jamais.
int games_link_poll(int state, int current, int *index);

// Evenements vers le telephone, envoyes tout de suite (perdus si la page
// n'est pas connectee).
void games_link_report_loading(int index);
void games_link_report_started(int index);
void games_link_report_refused(int reason, int detail);
// Le dernier GAMES_LINK_LAUNCH demande la reprise automatique du jeu
// ("Reprendre" sur le telephone).
bool games_link_launch_resume();

#endif
