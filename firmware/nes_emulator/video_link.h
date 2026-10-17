// video_link.h - liaison vers la carte video TV (second ESP32 "classique").
//
// Principe : l'ESP32-S3 n'a ni DAC ni assez de broches libres pour produire
// lui-meme un signal video. Il envoie donc l'image du jeu (256 x 240, un
// numero de couleur par pixel, palette 24 bits) et le son (16 kHz mono) a
// un second ESP32 dedie, par une liaison SPI a sens unique : 3 fils + masse.
// C'est lui qui fabrique le signal composite couleur (PAL, NTSC ou PAL 60)
// et le son pour la television (croquis video_coprocessor).
//
//   - SPI a 10 MHz : l'esclave SPI de l'ESP32 classique n'est garanti que
//     jusqu'a un peu moins de 11,4 MHz ;
//   - bandes de 15 lignes numerotees, avec somme de controle, compressees
//     (tv_link_codec.h) ; seules les bandes modifiees sont renvoyees ;
//   - memoire : image figee en PSRAM, tampons DMA pris en memoire interne a
//     la premiere image envoyee seulement ;
//   - paquet de configuration (norme, image progressive) renvoye chaque
//     seconde : il sert aussi de signal de presence ("Console connectee" sur
//     la television) ;
//   - sorties SCLK/CS/MOSI a puissance reduite (VIDEO_LINK_DRIVE_CAP) :
//     fronts plus doux, moins d'erreurs sur des fils volants.
//
// ENABLE_VIDEO_LINK : a mettre a 0 sans carte video (GPIO2, GPIO3 et GPIO43
// restent alors libres).
#ifndef VIDEO_LINK_H
#define VIDEO_LINK_H

#include <stdint.h>
#include "tv_link_protocol.h"

// Mettre a 1 UNIQUEMENT une fois la carte video cablee et flashee avec
// video_coprocessor/video_coprocessor.ino (meme version de protocole).
#define ENABLE_VIDEO_LINK 1

#if ENABLE_VIDEO_LINK
// -------------------- Brochage (cette carte = MAITRE) --------------------
//   GPIO2  (connecteur "Expanded IO", broche IO2)  : SCLK -> GPIO14 carte video
//   GPIO3  (connecteur "Expanded IO", broche IO3)  : CS   -> GPIO15 carte video
//   GPIO43 (connecteur "UART", broche TXD0)        : MOSI -> GPIO13 carte video
//   GND    (connecteur "UART")                     : masse commune (obligatoire)
// GPIO43 n'est libre que si "USB CDC On Boot" est ACTIVE dans l'IDE Arduino
// (sinon c'est la sortie du port serie).
#define VIDEO_LINK_PIN_SCLK  2
#define VIDEO_LINK_PIN_CS    3
#define VIDEO_LINK_PIN_MOSI  43

#define VIDEO_LINK_SPI_HZ    TVLINK_SPI_HZ_DEFAULT   // 10 MHz (ne pas depasser 11 MHz)

// Tampons DMA pour les bandes d'image (3848 octets chacun). 6 suffisent a
// garder le bus occupe entre deux images NES (16,7 ms). Minimum 2.
#define VIDEO_LINK_TX_BUFFERS 6

// Puissance des sorties SCLK, CS et MOSI (0 = 5 mA, 1 = 10 mA, 2 = 20 mA,
// valeur par defaut de l'ESP32-S3, 3 = 40 mA). Des fronts plus doux font moins
// "sonner" les fils volants (rebonds sur SCLK = bits en trop, paquets rejetes
// par la carte video) ; a 10 MHz, 10 mA donnent encore des fronts d'une
// dizaine de nanosecondes. Revenir a 2 avec des fils longs (plus de 30 cm) si
// les erreurs augmentent.
#define VIDEO_LINK_DRIVE_CAP 1

// Pause (microsecondes, CS releve) entre deux paquets : le temps que la carte
// video se prepare a recevoir le suivant (sa routine d'interruption relance la
// reception). Sans elle, deux paquets envoyes de suite pouvaient arriver
// colles ou amputes. 20 us = 2 % du debit pour une bande compressee typique.
#define VIDEO_LINK_GAP_US 20
#endif  // ENABLE_VIDEO_LINK

// Raccourcis conserves pour le reste du croquis.
#define VIDEO_LINK_WIDTH                TVLINK_WIDTH
#define VIDEO_LINK_HEIGHT               TVLINK_HEIGHT
#define VIDEO_LINK_AUDIO_SAMPLE_RATE    TVLINK_AUDIO_RATE     // DOIT valoir AUDIO_SAMPLE_RATE (osd.cpp)
#define VIDEO_LINK_AUDIO_CHUNK_SAMPLES  TVLINK_AUDIO_SAMPLES

// A appeler une fois dans setup(). Prepare seulement le bus SPI et les
// petits tampons (son, configuration) : les tampons d'image ne sont pris
// qu'a la premiere image envoyee. Sans effet si ENABLE_VIDEO_LINK vaut 0.
void video_link_init();

// true si la liaison est compilee ET son bus SPI a demarre.
bool video_link_is_ready();

// Ne bloque jamais : si l'image precedente n'a pas encore fini de partir,
// celle-ci est simplement sautee. La copie n'est faite que lorsqu'une nouvelle
// image peut effectivement partir ; la palette n'est envoyee que lorsqu'elle
// change (et chaque seconde).
// Renvoie faux si l'image a ete sautee (precedente pas encore partie, ou
// memoire des tampons pas encore disponible).
// full_refresh_ms = toutes les bandes renvoyees au moins a cette periode, meme
// inchangees (une bande abimee en route est jetee par la carte video et
// resterait sinon a l'ecran). 2 s en partie ; 300 ms pour les ecrans fixes
// (liste des jeux), sans quoi une bande perdue laissait des restes de l'ecran
// precedent.
bool video_link_submit_frame(uint8_t *const *lines, const uint8_t *palette_rgb888, uint32_t full_refresh_ms = 2000);

// Palette des images dessinees en RGB332 (liste des jeux sur la television,
// tv_menu.cpp) : la couleur de l'indice v est la couleur RGB332 v.
const uint8_t *video_link_rgb332_palette();

// Image progressive (true : 288 lignes en PAL, 240 en NTSC, sans
// entrelacement, comme une vraie NES) ou entrelacee (false, par defaut).
// Envoyee avec la configuration (tout de suite, puis chaque seconde).
void video_link_set_progressive(bool progressive);

// Echantillons mono 16 bits a 16 kHz, deja au volume de la console. L'audio
// passe dans la meme file que l'image, en paquets de 256 echantillons.
void video_link_queue_audio(const int16_t *samples, int count);

// Norme video choisie dans Reglages (true = PAL 50 Hz, false = NTSC 60 Hz).
// Envoyee tout de suite, puis rappelee chaque seconde par video_link_tick().
void video_link_set_video_standard(bool pal);

// Norme video, PAL 60 Hz compris : TVLINK_NORM_NTSC, TVLINK_NORM_PAL ou
// TVLINK_NORM_PAL60.
void video_link_set_video_norm(uint8_t norm);
// Norme en cours (TVLINK_NORM_*) - la liste des jeux sur la television
// garde une marge plus large en NTSC (8 colonnes cachees de chaque cote).
uint8_t video_link_video_norm();

// A appeler regulierement tant que la sortie Televiseur est choisie :
// dans les menus (in_game = false) comme en jeu (in_game = true). Renvoie la
// configuration une fois par seconde (la liaison est a sens unique : c'est
// aussi ce qui dit a la carte video que la console est la) et relance
// l'envoi des bandes en attente.
// Attend (timeout_ms au plus) que l'image en cours soit entierement partie
// vers la carte video.
void video_link_flush(uint32_t timeout_ms);

void video_link_tick(bool in_game);

// Garde le choix "Televiseur" a travers le redemarrage logiciel de fin de
// partie (SELECT maintenu), mais PAS a travers une coupure d'alimentation :
// a la mise sous tension, la console repart toujours sur l'ecran LCD.
// Fonctionne aussi quand ENABLE_VIDEO_LINK vaut 0 (recall renvoie alors false).
void video_link_remember_tv_mode(bool on);
bool video_link_recall_tv_mode();

#endif  // VIDEO_LINK_H
