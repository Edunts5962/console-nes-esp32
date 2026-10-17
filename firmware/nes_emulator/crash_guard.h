// crash_guard.h - detection des jeux qui font planter la console au
// lancement, et mise a l'ecart de ces jeux.
//
// Principe : avant de lancer nofrendo_main() sur une ROM, on note dans la
// NVS (memoire flash non volatile, qui survit a un plantage) le nom du
// fichier qu'on s'apprete a lancer. Si la carte redemarre alors que ce
// marqueur est encore present, c'est qu'un plantage s'est produit PENDANT le
// lancement de cette ROM. Comme la carte redemarre d'elle-meme apres un
// plantage (comportement par defaut d'ESP-IDF), setup() retombe sur les
// menus : il suffit d'eviter de relancer la meme ROM sans prevenir.
//
// Le jeu fautif n'est pas supprime : il est deplace dans le dossier
// "/quarantaine" de son emplacement (rom_storage_quarantine()) et reste
// recuperable (Reglages > Jeux ecartes, ou le portail web). Si le plantage
// suit le chargement d'une reprise automatique, c'est la reprise (.sta) qui
// est effacee et le jeu reste dans la liste.
//
// Deux moments distincts, a cause de l'ordre de setup() : la detection
// (lecture NVS, sans la carte SD) se fait dans crash_guard_init(), appelee
// tres tot ; la mise a l'ecart a besoin du stockage monte, voir
// crash_guard_process_pending_deletion(), a appeler juste apres le montage
// de la carte SD (et avant l'affichage des menus).
//
// Un chien de garde materiel (Task Watchdog) couvre aussi la fenetre de
// demarrage : si une ROM bloque l'emulateur AVANT la premiere image (boucle
// infinie, sans plantage visible du materiel), il force un redemarrage. Il
// est desarme des que la partie a tourne quelques secondes
// (crash_guard_confirm_boot()) : un probleme survenant plus tard en cours
// de partie ne marque pas la ROM.
#ifndef CRASH_GUARD_H
#define CRASH_GUARD_H

#ifdef __cplusplus
extern "C" {
#endif

// A appeler une seule fois, tot dans setup() (juste apres Serial.begin()),
// AVANT le montage de la carte SD. Detecte seulement un eventuel plantage
// survenu pendant le lancement precedent (lecture NVS uniquement) - la mise
// a l'ecart du fichier se fait plus tard, voir
// crash_guard_process_pending_deletion() ci-dessous.
void crash_guard_init();

// A appeler une seule fois, juste APRES le montage du stockage (carte SD et
// memoire interne) et AVANT les menus : (1) si crash_guard_init() a detecte
// un plantage, met le jeu fautif en quarantaine (ou efface sa reprise
// automatique) ; en cas d'echec seulement, la ROM est marquee "!" dans la
// liste ; (2) met aussi en quarantaine les ROMs encore marquees "!" lors de
// demarrages precedents, et les retire de la liste noire en cas de succes.
// La liste des jeux etant relue a l'ouverture des menus, elle est a jour
// sans autre rafraichissement.
void crash_guard_process_pending_deletion();

// A appeler juste avant chaque nofrendo_main(), avec le chemin de la ROM
// qu'on s'apprete a lancer (ex: "/mario.nes", tel que renvoye par
// get_selected_game()).
void crash_guard_begin_launch(const char *rom_path);

// Meme chose pour un changement de jeu SANS REDEMARRAGE (manette du telephone,
// osd.cpp) : en cas de plantage pendant ce lancement, la ROM est seulement
// marquee "!" au demarrage suivant, jamais ecartee (le plantage peut venir
// du changement lui-meme). La marque disparait des que le jeu demarre
// normalement.
void crash_guard_begin_launch_hot(const char *rom_path);

// A appeler a chaque image affichee (depuis vid_flush()) : nourrit le
// watchdog de demarrage tant qu'il est arme, et confirme definitivement le
// lancement (desarme le watchdog, efface le marqueur "en cours") une fois
// quelques secondes de jeu ecoulees sans probleme.
void crash_guard_confirm_boot();

// Partie en pause (liste des jeux ouverte sur le telephone) :
// crash_guard_feed() nourrit le watchdog sans confirmer le lancement ;
// crash_guard_extend_grace() a la reprise repousse la confirmation de la duree
// de la pause (seul le temps de jeu compte).
void crash_guard_feed();
void crash_guard_extend_grace(unsigned long paused_ms);

// A appeler juste avant un retour au menu VOLONTAIRE (ESP.restart() suite au
// maintien de SELECT, voir osd.cpp/update_quit_hold_feedback()) : efface le
// marqueur "en cours" pour ne pas confondre ce redemarrage volontaire avec
// un plantage.
void crash_guard_clear_for_clean_exit();

// Renvoie true si ce nom de fichier (avec ou sans le '/' initial - les deux
// formes sont acceptees) figure dans la liste noire. Utilise par
// show_menu()/draw_menu_item() (osd.cpp) pour signaler visuellement une ROM
// problematique dans la liste.
bool crash_guard_is_rom_bad(const char *rom_filename);

// La reprise automatique du jeu en cours de lancement va etre chargee : un
// plantage avant la confirmation efface la reprise (.sta) au lieu d'ecarter le
// jeu.
void crash_guard_note_resume();
// Message a montrer une fois dans les menus (jeu ecarte, reprise effacee) ;
// NULL : rien. Le texte reste valable jusqu'a l'appel suivant.
const char *crash_guard_take_notice();
// Jeu remis dans la liste (Jeux ecartes) : marques "!" retirees.
void crash_guard_forget(const char *rom_filename);

#ifdef __cplusplus
}
#endif

#endif
