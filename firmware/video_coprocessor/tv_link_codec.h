// tv_link_codec.h - compression des bandes d'image de la liaison TV.
//
// CE FICHIER EXISTE EN DEUX COPIES STRICTEMENT IDENTIQUES :
//   - nes_emulator/tv_link_codec.h       (carte principale ESP32-S3 : compression)
//   - video_coprocessor/tv_link_codec.h  (carte video ESP32 : decompression)
//
// Pourquoi : la liaison SPI fait 10 MHz, soit 1,25 Mo/s. Une image NES brute
// fait 61 440 octets : 20 images completes par seconde au plus. Quand tout
// l'ecran bouge (defilement), il faudrait sauter des images (38 a 44
// envoyees par seconde sur 60). Les images NES se compressent tres
// bien (aplats, tuiles de 8 pixels qui se repetent) : une bande compressee
// fait le plus souvent 5 a 20 % de sa taille.
//
// Format d'une bande compressee (3840 pixels = 15 lignes de 256, un octet
// par pixel), suite de commandes :
//   0x00..0x3F  n+1 octets copies tels quels (1 a 64), qui suivent
//   0x40..0x7F  (n&63)+3 fois l'octet qui suit (3 a 66)
//   0x80..0xBF  (n&63)+3 octets recopies de la ligne du dessus (256 plus tot)
//   0xC0..0xFF  (n&63)+3 octets recopies d plus tot, d = octet qui suit (1..255)
// Les recopies peuvent se chevaucher (d plus petit que la longueur) : elles
// se font octet par octet, dans l'ordre. Le decodage verifie tout (debut de
// bande, longueur exacte) : une bande abimee est refusee, jamais ecrite a
// moitie hors de son tampon.
#ifndef TV_LINK_CODEC_H
#define TV_LINK_CODEC_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define TVCODEC_ROW 256          // largeur d'une ligne (recopie "ligne du dessus")
#define TVCODEC_MIN_MATCH 3
#define TVCODEC_MAX_MATCH 66
#define TVCODEC_MAX_LITERAL 64

// Longueur commune de a[0..] et b[0..], au plus max.
static inline int tvcodec_match(const uint8_t *a, const uint8_t *b, int max) {
  int n = 0;
  while (n < max && a[n] == b[n]) n++;
  return n;
}

// Compresse n octets de src dans out (au plus max_out octets). Renvoie la
// taille compressee, ou 0 si elle depasserait max_out (envoyer la bande
// brute dans ce cas).
static inline size_t tvlink_band_encode(const uint8_t *src, size_t n, uint8_t *out, size_t max_out) {
  // Distances essayees pour les recopies "d plus tot" : tuiles NES de 8 et
  // 16 pixels, motifs de 2 et 4, objets de 24 et 32.
  static const uint8_t dists[] = {8, 16, 4, 2, 32, 24};
  size_t o = 0;
  size_t lit_start = 0, lit_len = 0;
  size_t p = 0;
  while (p < n) {
    const int max = (int)((n - p) < TVCODEC_MAX_MATCH ? (n - p) : TVCODEC_MAX_MATCH);
    int best_len = 0, best_kind = 0, best_d = 0, best_gain = 0;
    if (max >= TVCODEC_MIN_MATCH) {
      const uint8_t c0 = src[p], c1 = src[p + 1], c2 = src[p + 2];
      // Aplat : n fois le meme octet (commande de 2 octets).
      if (c1 == c0 && c2 == c0) {
        int r = 3;
        while (r < max && src[p + r] == c0) r++;
        best_len = r; best_kind = 1; best_gain = r - 2;
      }
      // Ligne du dessus (commande d'un seul octet).
      if (p >= TVCODEC_ROW) {
        const uint8_t *q = src + p - TVCODEC_ROW;
        if (q[0] == c0 && q[1] == c1 && q[2] == c2) {
          int v = 3 + tvcodec_match(src + p + 3, q + 3, max - 3);
          if (v - 1 > best_gain) { best_len = v; best_kind = 2; best_gain = v - 1; }
        }
      }
      // Motif qui se repete a courte distance (commande de 2 octets).
      for (unsigned i = 0; i < sizeof(dists) && best_len < max; i++) {
        const size_t d = dists[i];
        if (p < d) continue;
        const uint8_t *q = src + p - d;
        if (q[0] != c0 || q[1] != c1 || q[2] != c2) continue;
        int h = 3 + tvcodec_match(src + p + 3, q + 3, max - 3);
        if (h - 2 > best_gain) { best_len = h; best_kind = 3; best_d = (int)d; best_gain = h - 2; }
      }
    }
    if (best_kind == 0) {
      // Bande qui se compresse mal (image tres detaillee) : abandon des le
      // premier quart, la bande partira brute - inutile de depenser le
      // temps du reste du codage.
      if ((p & 255) == 0 && p >= n / 4 && o + lit_len > (p * 7) / 10) return 0;
      if (lit_len == 0) lit_start = p;
      lit_len++;
      p++;
      if (lit_len == TVCODEC_MAX_LITERAL) {
        if (o + 1 + lit_len > max_out) return 0;
        out[o++] = (uint8_t)(lit_len - 1);
        memcpy(out + o, src + lit_start, lit_len);
        o += lit_len;
        lit_len = 0;
      }
      continue;
    }
    if (lit_len) {
      if (o + 1 + lit_len > max_out) return 0;
      out[o++] = (uint8_t)(lit_len - 1);
      memcpy(out + o, src + lit_start, lit_len);
      o += lit_len;
      lit_len = 0;
    }
    if (o + 2 > max_out) return 0;
    const uint8_t len_code = (uint8_t)(best_len - TVCODEC_MIN_MATCH);
    if (best_kind == 1) {
      out[o++] = (uint8_t)(0x40 | len_code);
      out[o++] = src[p];
    } else if (best_kind == 2) {
      out[o++] = (uint8_t)(0x80 | len_code);
    } else {
      out[o++] = (uint8_t)(0xC0 | len_code);
      out[o++] = (uint8_t)best_d;
    }
    p += (size_t)best_len;
  }
  if (lit_len) {
    if (o + 1 + lit_len > max_out) return 0;
    out[o++] = (uint8_t)(lit_len - 1);
    memcpy(out + o, src + lit_start, lit_len);
    o += lit_len;
  }
  return o;
}

// Decompresse in[0..len) dans dst (exactement n octets). Renvoie 1 si la
// bande est complete et bien formee, 0 sinon (dst peut alors etre a moitie
// ecrit : decompresser dans un tampon de travail, pas dans l'image).
// *used (si non nul) : octets de in effectivement lus. Ce qui suit (bourrage
// nul, ou paquet suivant colle a celui-ci par la liaison) n'est pas lu : la
// somme de controle de la bande decompressee dit si elle est bonne.
// Avant, des octets non nuls apres la fin faisaient refuser la bande - une
// bande collee au paquet suivant etait perdue a chaque envoi.
static inline int tvlink_band_decode(const uint8_t *in, size_t len, uint8_t *dst, size_t n, size_t *used) {
  size_t i = 0, p = 0;
  if (used) *used = 0;
  while (p < n) {
    if (i >= len) return 0;
    const uint8_t c = in[i++];
    if (c < 0x40) {
      const size_t k = (size_t)c + 1;
      if (i + k > len || p + k > n) return 0;
      memcpy(dst + p, in + i, k);
      i += k;
      p += k;
      continue;
    }
    const size_t k = (size_t)(c & 0x3F) + TVCODEC_MIN_MATCH;
    if (p + k > n) return 0;
    if (c < 0x80) {
      if (i >= len) return 0;
      memset(dst + p, in[i++], k);
      p += k;
      continue;
    }
    size_t d;
    if (c < 0xC0) {
      d = TVCODEC_ROW;
    } else {
      if (i >= len) return 0;
      d = in[i++];
      if (d == 0) return 0;
    }
    if (d > p) return 0;
    uint8_t *w = dst + p;
    const uint8_t *r = w - d;
    for (size_t j = 0; j < k; j++) w[j] = r[j];
    p += k;
  }
  if (used) *used = i;
  return 1;
}

// ---------------------------------------------------------------------------
// Bande "16 couleurs" : quand la compression ne gagne rien (image tres
// detaillee), une bande de 15 lignes NES n'utilise presque toujours pas plus
// de 16 couleurs differentes. Chaque pixel tient alors sur 4 bits :
//   16 octets : table des couleurs (numero 0..15 -> couleur de la palette)
//   n/2 octets : 2 pixels par octet (pixel pair dans les 4 bits du bas)
// soit 1936 octets au lieu de 3840 : deux fois plus d'images par seconde.
#define TVCODEC_NIBBLE_MAP 16

// Renvoie la taille (16 + n/2), ou 0 si la bande a plus de 16 couleurs ou
// si out est trop petit. n doit etre pair.
static inline size_t tvlink_band_pack4(const uint8_t *src, size_t n, uint8_t *out, size_t max_out) {
  const size_t need = TVCODEC_NIBBLE_MAP + n / 2;
  if (need > max_out || (n & 1)) return 0;
  uint8_t code[256];
  uint8_t used = 0;
  memset(code, 0xFF, sizeof(code));
  for (size_t i = 0; i < n; i++) {
    const uint8_t c = src[i];
    if (code[c] == 0xFF) {
      if (used == TVCODEC_NIBBLE_MAP) return 0;
      code[c] = used;
      out[used++] = c;
    }
  }
  while (used < TVCODEC_NIBBLE_MAP) out[used++] = 0;
  uint8_t *q = out + TVCODEC_NIBBLE_MAP;
  for (size_t i = 0; i < n; i += 2) *q++ = (uint8_t)(code[src[i]] | (code[src[i + 1]] << 4));
  return need;
}

// Renvoie 1 si len suffit (le bourrage nul eventuel est ignore), 0 sinon.
static inline int tvlink_band_unpack4(const uint8_t *in, size_t len, uint8_t *dst, size_t n) {
  if ((n & 1) || len < TVCODEC_NIBBLE_MAP + n / 2) return 0;
  const uint8_t *map = in;
  const uint8_t *q = in + TVCODEC_NIBBLE_MAP;
  for (size_t i = 0; i < n; i += 2) {
    const uint8_t b = *q++;
    dst[i] = map[b & 15];
    dst[i + 1] = map[b >> 4];
  }
  return 1;
}

#endif  // TV_LINK_CODEC_H
