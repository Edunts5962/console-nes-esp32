// tv_link_protocol.h - protocole de la liaison "carte principale -> carte
// video TV" (version 3).
//
// CE FICHIER EXISTE EN DEUX COPIES STRICTEMENT IDENTIQUES :
//   - nes_emulator/tv_link_protocol.h       (carte principale ESP32-S3, maitre SPI)
//   - video_coprocessor/tv_link_protocol.h  (carte video ESP32 classique, esclave SPI)
// La carte video affiche un message sur la TV si les versions de protocole ne
// correspondent pas (voir TVLINK_PROTOCOL_VERSION). Meme chose pour
// tv_link_codec.h (compression des bandes).
//
// Principe : liaison SPI a sens unique (MOSI seulement), 3 fils + masse.
// Tous les paquets commencent par un en-tete de 8 octets :
//
//   octet  0..2 : 'N' 'E' 'S'
//   octet  3    : type ('F' bande d'image, 'P' palette, 'A' son, 'C' configuration)
//   octets 4..7 : selon le type (voir plus bas)
//
// Toutes les tailles de paquets sont des multiples de 4 octets : c'est une
// exigence du DMA de l'esclave SPI de l'ESP32 classique (documentation
// ESP-IDF, "SPI Slave Driver", restrictions connues). Un paquet plus court
// est complete par des octets nuls.
//
// Les bandes d'image peuvent etre compressees (tv_link_codec.h), et la
// configuration porte un drapeau "image progressive".
//
// IMAGE : 256 x 240 pixels, 1 octet par pixel (numero de couleur), decoupee
// en 16 bandes de 15 lignes (3840 octets). Chaque bande voyage dans son
// propre paquet, avec son numero et une somme de controle des 3840 pixels
// (apres decompression). Seules les bandes qui ont change depuis le dernier
// envoi sont transmises (plus un rafraichissement complet periodique). La
// derniere bande envoyee pour une image porte le drapeau TVLINK_BAND_LAST :
// la carte video affiche alors l'image complete d'un coup, avec la derniere
// palette recue.
//
//   'F' : octet 4 = numero d'image (0..255, cyclique)
//         octet 5 = numero de bande (0..15) | TVLINK_BAND_NIBBLE (0x20)
//                   | TVLINK_BAND_PACKED (0x40) | TVLINK_BAND_LAST (0x80)
//         octets 6..7 = somme de controle 16 bits des 3840 pixels (petit-boutiste)
//         puis 3840 octets de pixels ; ou la bande compressee si
//         TVLINK_BAND_PACKED (taille variable, bourrage nul) ; ou la bande
//         "16 couleurs" si TVLINK_BAND_NIBBLE (16 + 1920 octets, voir
//         tv_link_codec.h)                                    -> 3848 octets au plus
//   'P' : octet 4 = numero de palette (change a chaque nouvelle palette)
//         octet 5 = 0
//         octets 6..7 = somme de controle 16 bits des 768 octets
//         puis 256 couleurs de 3 octets (rouge, vert, bleu)    -> 776 octets
//   'A' : octets 4..5 = nombre d'echantillons (256, petit-boutiste)
//         octets 6..7 = somme de controle 16 bits des echantillons
//         puis 256 echantillons 16 bits signes mono, 16 kHz  -> 520 octets
//   'C' : octet 4 = version de protocole (3)
//         octet 5 = norme video (0 = NTSC 60 Hz, 1 = PAL 50 Hz, 2 = PAL 60 Hz)
//         octet 6 = drapeaux (bit 0 : une partie est en cours ;
//                   bit 1 : image progressive)
//         octet 7 = controle : 0xA5 ^ octet4 ^ octet5 ^ octet6  -> 8 octets
#ifndef TV_LINK_PROTOCOL_H
#define TV_LINK_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#define TVLINK_PROTOCOL_VERSION 3

#define TVLINK_WIDTH   256
#define TVLINK_HEIGHT  240

#define TVLINK_HEADER_SIZE 8
#define TVLINK_MAGIC0 'N'
#define TVLINK_MAGIC1 'E'
#define TVLINK_MAGIC2 'S'
#define TVLINK_TYPE_BAND    'F'
#define TVLINK_TYPE_PALETTE 'P'
#define TVLINK_TYPE_AUDIO   'A'
#define TVLINK_TYPE_CONFIG  'C'

#define TVLINK_BAND_ROWS    15
#define TVLINK_BAND_BYTES   (TVLINK_WIDTH * TVLINK_BAND_ROWS)     // 3840
#define TVLINK_NUM_BANDS    (TVLINK_HEIGHT / TVLINK_BAND_ROWS)    // 16
#define TVLINK_BAND_LAST    0x80
#define TVLINK_BAND_PACKED  0x40
#define TVLINK_BAND_NIBBLE  0x20   // v3 : 4 bits par pixel (16 couleurs au plus)
#define TVLINK_BAND_INDEX_MASK 0x0F
#define TVLINK_BAND_PACKET_BYTES (TVLINK_HEADER_SIZE + TVLINK_BAND_BYTES)   // 3848

#define TVLINK_PALETTE_COLORS 256
#define TVLINK_PALETTE_BYTES  (TVLINK_PALETTE_COLORS * 3)                   // 768
#define TVLINK_PALETTE_PACKET_BYTES (TVLINK_HEADER_SIZE + TVLINK_PALETTE_BYTES)   // 776

#define TVLINK_AUDIO_RATE          16000
#define TVLINK_AUDIO_SAMPLES       256
#define TVLINK_AUDIO_PACKET_BYTES  (TVLINK_HEADER_SIZE + TVLINK_AUDIO_SAMPLES * 2)   // 520

#define TVLINK_CONFIG_PACKET_BYTES TVLINK_HEADER_SIZE   // 8
#define TVLINK_NORM_NTSC  0   // 60 Hz, 525 lignes, couleur NTSC
#define TVLINK_NORM_PAL   1   // 50 Hz, 625 lignes, couleur PAL
#define TVLINK_NORM_PAL60 2   // 60 Hz, 525 lignes, couleur PAL
#define TVLINK_NORM_MAX   TVLINK_NORM_PAL60
#define TVLINK_FLAG_IN_GAME     0x01
#define TVLINK_FLAG_PROGRESSIVE 0x02

#define TVLINK_MAX_PACKET_BYTES TVLINK_BAND_PACKET_BYTES

// Frequence d'horloge SPI conseillee. L'esclave SPI de l'ESP32 classique est
// garanti par Espressif jusqu'a un peu moins de 11,4 MHz sur ses broches
// natives (IO_MUX : GPIO12/13/14/15 pour SPI2). 10 MHz = valeur sure.
#define TVLINK_SPI_HZ_DEFAULT 10000000

#if (TVLINK_BAND_PACKET_BYTES % 4) || (TVLINK_AUDIO_PACKET_BYTES % 4) || (TVLINK_CONFIG_PACKET_BYTES % 4) || (TVLINK_PALETTE_PACKET_BYTES % 4)
#error "tv_link_protocol.h : toutes les tailles de paquets doivent etre des multiples de 4 octets"
#endif
#if (TVLINK_HEIGHT % TVLINK_BAND_ROWS) != 0
#error "tv_link_protocol.h : TVLINK_BAND_ROWS doit diviser TVLINK_HEIGHT"
#endif

// Empreinte 32 bits d'une bande (somme de Fletcher sur des mots de 32 bits,
// sensible a l'ordre). n doit etre un multiple de 4, p aligne sur 4 octets.
// Sert a deux choses : detecter les bandes inchangees (carte principale) et
// verifier les donnees recues (carte video, via tvlink_fold16).
typedef uint32_t __attribute__((__may_alias__)) tvlink_u32_alias;

static inline uint32_t tvlink_hash32(const uint8_t *p, size_t n) {
  const tvlink_u32_alias *w = (const tvlink_u32_alias *)(const void *)p;
  uint32_t s1 = 0x12345678u, s2 = 0x9ABCDEF0u;
  for (size_t i = 0; i < n / 4; i++) {
    s1 += w[i];
    s2 += s1;
  }
  return s1 ^ (s2 * 0x9E3779B1u);
}

static inline uint16_t tvlink_fold16(uint32_t h) {
  return (uint16_t)((h ^ (h >> 16)) & 0xFFFFu);
}

static inline void tvlink_put_magic(uint8_t *hdr, char type) {
  hdr[0] = TVLINK_MAGIC0;
  hdr[1] = TVLINK_MAGIC1;
  hdr[2] = TVLINK_MAGIC2;
  hdr[3] = (uint8_t)type;
}

static inline int tvlink_magic_ok(const uint8_t *hdr) {
  return hdr[0] == TVLINK_MAGIC0 && hdr[1] == TVLINK_MAGIC1 && hdr[2] == TVLINK_MAGIC2;
}

// Couleur 24 bits d'une valeur RGB332 (ecrans dessines en RGB332 : liste
// des jeux sur la television, ecrans d'attente de la carte video). Memes
// valeurs que LovyanGFX : 3 bits -> 0..255 par (v * 0x49) >> 1, 2 bits -> v * 0x55.
static inline void tvlink_rgb332_to_888(uint8_t c, uint8_t *rgb) {
  rgb[0] = (uint8_t)(((c >> 5) * 0x49) >> 1);
  rgb[1] = (uint8_t)((((c >> 2) & 7) * 0x49) >> 1);
  rgb[2] = (uint8_t)((c & 3) * 0x55);
}

#endif  // TV_LINK_PROTOCOL_H
