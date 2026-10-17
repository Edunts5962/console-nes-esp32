// tv_link_receiver.h - logique de reception de la carte video TV.
//
// Aucune dependance materielle (ni Arduino, ni ESP-IDF) : ce fichier ne fait
// que decoder les paquets recus (voir tv_link_protocol.h et tv_link_codec.h)
// et gerer la file audio.
//
// Plusieurs paquets dans un meme tampon (paquets colles par la liaison quand
// ils se suivent de trop pres) ; bande compressee acceptee meme suivie
// d'octets inconnus (sa somme de controle suffit).
// Bandes compressees ou "16 couleurs" (decodees dans un tampon de reserve, qui
// prend la place de la bande seulement si tout est correct : une bande abimee
// ne laisse jamais une image a moitie ecrite), palette 24 bits (paquet 'P'),
// drapeau "image progressive".
#ifndef TV_LINK_RECEIVER_H
#define TV_LINK_RECEIVER_H

#include <stdint.h>
#include <string.h>
#include "tv_link_protocol.h"
#include "tv_link_codec.h"

struct TvLinkStats {
  uint32_t packets;            // paquets valides, tous types confondus
  uint32_t bands;              // bandes d'image acceptees
  uint32_t packed_bands;       // dont compressees
  uint32_t nibble_bands;       // dont "16 couleurs" (4 bits par pixel)
  uint32_t band_bytes;         // octets de bandes recus (en-tetes compris) : taux de compression
  uint32_t frames;             // images completes (bande portant TVLINK_BAND_LAST)
  uint32_t audio_packets;
  uint32_t config_packets;
  uint32_t palette_packets;
  uint32_t bad_header;         // en-tete ou type invalide (parasite, desynchronisation)
  uint32_t bad_length;         // paquet trop court
  uint32_t bad_checksum;       // donnees abimees en route (image, palette ou son)
  uint32_t missed_frame_ends;  // image commencee dont la derniere bande n'est jamais arrivee
  uint32_t merged;             // Paquets arrives colles a un autre dans le meme tampon
  uint32_t trailing_garbage;   // Octets inconnus apres le dernier paquet d'un tampon
};

enum {
  TVLINK_EV_NONE    = 0,
  TVLINK_EV_FRAME   = 1 << 0,   // une image complete est prete dans les bandes
  TVLINK_EV_AUDIO   = 1 << 1,   // audio_samples() / audio_count() valides jusqu'au prochain paquet
  TVLINK_EV_CONFIG  = 1 << 2,   // config_*() mis a jour
  TVLINK_EV_PALETTE = 1 << 3    // palette() : nouvelle palette (numero different)
};

class TvLinkReceiver {
 public:
  // bands : TVLINK_NUM_BANDS zones de TVLINK_BAND_BYTES octets (l'image en
  // cours de reconstruction) ; spare : une zone de plus, de la meme taille
  // (decompression). Les zones s'echangent : lire les bandes par band(i).
  // Elles gardent leur contenu d'une image a l'autre : une bande non renvoyee
  // reste valable.
  void begin(uint8_t *const *bands, uint8_t *spare) {
    for (int i = 0; i < TVLINK_NUM_BANDS; i++) bands_[i] = bands[i];
    spare_ = spare;
    memset(&stats_, 0, sizeof(stats_));
    frame_open_ = false;
    frame_id_ = 0;
    audio_ = 0;
    audio_n_ = 0;
    cfg_version_ = 0;
    cfg_norm_ = TVLINK_NORM_NTSC;
    cfg_in_game_ = false;
    cfg_progressive_ = false;
    have_palette_ = false;
    palette_serial_ = 0;
  }

  // Traite un paquet recu (len = nombre d'octets reellement recus). Renvoie
  // une combinaison de TVLINK_EV_*. consumed() donne ensuite la taille du
  // paquet (multiple de 4), 0 s'il etait invalide : s'il reste des octets
  // apres lui, c'est peut-etre un deuxieme paquet colle au premier (la liaison
  // peut en coller deux quand ils se suivent de trop pres - voir
  // handle_all()).
  int handle(const uint8_t *pkt, size_t len) {
    consumed_ = 0;
    if (len < TVLINK_HEADER_SIZE || !tvlink_magic_ok(pkt)) {
      stats_.bad_header++;
      return TVLINK_EV_NONE;
    }
    switch (pkt[3]) {
      case TVLINK_TYPE_BAND: return handle_band(pkt, len);
      case TVLINK_TYPE_PALETTE: return handle_palette(pkt, len);
      case TVLINK_TYPE_AUDIO: return handle_audio(pkt, len);
      case TVLINK_TYPE_CONFIG: return handle_config(pkt, len);
      default:
        stats_.bad_header++;
        return TVLINK_EV_NONE;
    }
  }

  size_t consumed() const { return consumed_; }

  // Octets restant apres le dernier paquet reconnu d'un tampon (ni bourrage
  // nul, ni paquet) : comptes dans stats().trailing_garbage.
  void note_trailing(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++)
      if (p[i]) { stats_.trailing_garbage++; return; }
  }

  // Type d'un paquet recu, sans le traiter (0 si en-tete invalide).
  static char packet_type(const uint8_t *pkt, size_t len) {
    if (len < TVLINK_HEADER_SIZE || !tvlink_magic_ok(pkt)) return 0;
    return (char)pkt[3];
  }

  const uint8_t *band(int i) const { return bands_[i]; }
  const TvLinkStats &stats() const { return stats_; }
  const int16_t *audio_samples() const { return audio_; }
  int audio_count() const { return audio_n_; }
  uint8_t config_version() const { return cfg_version_; }
  uint8_t config_norm() const { return cfg_norm_; }   // TVLINK_NORM_*
  bool config_in_game() const { return cfg_in_game_; }
  bool config_progressive() const { return cfg_progressive_; }
  bool has_palette() const { return have_palette_; }
  const uint8_t *palette() const { return palette_; }

  // Traite tous les paquets d'un tampon recu (un seul d'habitude, parfois deux
  // colles). on_packet(ev) est appele apres chacun (le son d'un paquet n'est
  // valable que jusqu'au suivant). Renvoie le nombre de paquets reconnus.
  template <typename F>
  int handle_all(const uint8_t *buf, size_t len, F on_packet) {
    size_t off = 0;
    int n = 0;
    while (off + TVLINK_HEADER_SIZE <= len) {
      if (n > 0 && !tvlink_magic_ok(buf + off)) break;   // pas un paquet colle : reste du tampon
      const int ev = handle(buf + off, len - off);
      const size_t used = consumed_;
      on_packet(ev);
      if (!used) return n;      // paquet invalide : erreur deja comptee
      n++;
      if (n > 1) stats_.merged++;
      off += used;
    }
    if (off < len) note_trailing(buf + off, len - off);
    return n;
  }

  // Paquet de configuration valide ? (octet 7 = controle des octets 4..6)
  static uint8_t config_check(const uint8_t *hdr) {
    return (uint8_t)(0xA5 ^ hdr[4] ^ hdr[5] ^ hdr[6]);
  }

 private:
  int handle_band(const uint8_t *pkt, size_t len) {
    const bool packed = (pkt[5] & TVLINK_BAND_PACKED) != 0;
    const bool nibble = (pkt[5] & TVLINK_BAND_NIBBLE) != 0;
    if (packed && nibble) {
      stats_.bad_header++;
      return TVLINK_EV_NONE;
    }
    const size_t min_len = packed ? (size_t)TVLINK_HEADER_SIZE + 1
                         : nibble ? (size_t)TVLINK_HEADER_SIZE + TVCODEC_NIBBLE_MAP + TVLINK_BAND_BYTES / 2
                                  : (size_t)TVLINK_BAND_PACKET_BYTES;
    if (len < min_len) {
      stats_.bad_length++;
      return TVLINK_EV_NONE;
    }
    uint8_t idx = pkt[5] & TVLINK_BAND_INDEX_MASK;
    if (idx >= TVLINK_NUM_BANDS) {
      stats_.bad_header++;
      return TVLINK_EV_NONE;
    }
    const uint8_t *payload = pkt + TVLINK_HEADER_SIZE;
    uint16_t expected = (uint16_t)(pkt[6] | (pkt[7] << 8));
    if (packed || nibble) {
      size_t plen = len - TVLINK_HEADER_SIZE;
      if (plen > TVLINK_BAND_BYTES) plen = TVLINK_BAND_BYTES;
      size_t used = TVCODEC_NIBBLE_MAP + TVLINK_BAND_BYTES / 2;
      const int ok = packed ? tvlink_band_decode(payload, plen, spare_, TVLINK_BAND_BYTES, &used)
                            : tvlink_band_unpack4(payload, plen, spare_, TVLINK_BAND_BYTES);
      if (!ok || tvlink_fold16(tvlink_hash32(spare_, TVLINK_BAND_BYTES)) != expected) {
        stats_.bad_checksum++;
        return TVLINK_EV_NONE;
      }
      uint8_t *t = bands_[idx];   // la bande decompressee prend la place de la precedente
      bands_[idx] = spare_;
      spare_ = t;
      if (packed) stats_.packed_bands++;
      else stats_.nibble_bands++;
      consumed_ = (TVLINK_HEADER_SIZE + used + 3) & ~(size_t)3;
      stats_.band_bytes += (uint32_t)consumed_;
    } else {
      if (tvlink_fold16(tvlink_hash32(payload, TVLINK_BAND_BYTES)) != expected) {
        stats_.bad_checksum++;
        return TVLINK_EV_NONE;
      }
      memcpy(bands_[idx], payload, TVLINK_BAND_BYTES);
      consumed_ = TVLINK_BAND_PACKET_BYTES;
      stats_.band_bytes += TVLINK_BAND_PACKET_BYTES;
    }
    if (frame_open_ && pkt[4] != frame_id_) stats_.missed_frame_ends++;
    frame_id_ = pkt[4];
    frame_open_ = true;
    stats_.bands++;
    stats_.packets++;
    if (pkt[5] & TVLINK_BAND_LAST) {
      frame_open_ = false;
      stats_.frames++;
      return TVLINK_EV_FRAME;
    }
    return TVLINK_EV_NONE;
  }

  int handle_palette(const uint8_t *pkt, size_t len) {
    if (len < TVLINK_PALETTE_PACKET_BYTES) {
      stats_.bad_length++;
      return TVLINK_EV_NONE;
    }
    const uint8_t *payload = pkt + TVLINK_HEADER_SIZE;
    uint16_t expected = (uint16_t)(pkt[6] | (pkt[7] << 8));
    if (tvlink_fold16(tvlink_hash32(payload, TVLINK_PALETTE_BYTES)) != expected) {
      stats_.bad_checksum++;
      return TVLINK_EV_NONE;
    }
    stats_.palette_packets++;
    stats_.packets++;
    consumed_ = TVLINK_PALETTE_PACKET_BYTES;
    // Renvoi periodique de la meme palette : rien a faire.
    if (have_palette_ && pkt[4] == palette_serial_ && memcmp(palette_, payload, TVLINK_PALETTE_BYTES) == 0)
      return TVLINK_EV_NONE;
    memcpy(palette_, payload, TVLINK_PALETTE_BYTES);
    palette_serial_ = pkt[4];
    have_palette_ = true;
    return TVLINK_EV_PALETTE;
  }

  int handle_audio(const uint8_t *pkt, size_t len) {
    uint16_t n = (uint16_t)(pkt[4] | (pkt[5] << 8));
    if (n == 0 || n > TVLINK_AUDIO_SAMPLES || (n & 1) ||
        len < (size_t)TVLINK_HEADER_SIZE + (size_t)n * 2) {
      stats_.bad_length++;
      return TVLINK_EV_NONE;
    }
    const uint8_t *payload = pkt + TVLINK_HEADER_SIZE;
    uint16_t expected = (uint16_t)(pkt[6] | (pkt[7] << 8));
    if (tvlink_fold16(tvlink_hash32(payload, (size_t)n * 2)) != expected) {
      stats_.bad_checksum++;
      return TVLINK_EV_NONE;
    }
    audio_ = (const int16_t *)(const void *)payload;   // petit-boutiste, comme l'ESP32
    audio_n_ = n;
    stats_.audio_packets++;
    stats_.packets++;
    consumed_ = ((size_t)TVLINK_HEADER_SIZE + (size_t)n * 2 + 3) & ~(size_t)3;
    return TVLINK_EV_AUDIO;
  }

  int handle_config(const uint8_t *pkt, size_t len) {
    if (len < TVLINK_CONFIG_PACKET_BYTES) {
      stats_.bad_length++;
      return TVLINK_EV_NONE;
    }
    if (pkt[7] != config_check(pkt) || pkt[5] > TVLINK_NORM_MAX) {
      stats_.bad_checksum++;
      return TVLINK_EV_NONE;
    }
    cfg_version_ = pkt[4];
    cfg_norm_ = pkt[5];
    cfg_in_game_ = (pkt[6] & TVLINK_FLAG_IN_GAME) != 0;
    cfg_progressive_ = (pkt[6] & TVLINK_FLAG_PROGRESSIVE) != 0;
    stats_.config_packets++;
    stats_.packets++;
    consumed_ = TVLINK_CONFIG_PACKET_BYTES;
    return TVLINK_EV_CONFIG;
  }

  uint8_t *bands_[TVLINK_NUM_BANDS];
  uint8_t *spare_;
  size_t consumed_ = 0;
  TvLinkStats stats_;
  bool frame_open_;
  uint8_t frame_id_;
  const int16_t *audio_;
  int audio_n_;
  uint8_t cfg_version_;
  uint8_t cfg_norm_;
  bool cfg_in_game_;
  bool cfg_progressive_;
  bool have_palette_;
  uint8_t palette_serial_;
  uint8_t palette_[TVLINK_PALETTE_BYTES];
};

// ----------------------------------------------------------------------------
// File audio "un producteur / un consommateur" (tache de reception -> routine
// d'interruption a 16 kHz). Taille = puissance de 2.
// ----------------------------------------------------------------------------
class TvAudioRing {
 public:
  static const uint32_t SIZE = 4096;   // 256 ms a 16 kHz
  void reset() { head_ = 0; tail_ = 0; dropped_ = 0; }
  uint32_t count() const { return head_ - tail_; }
  uint32_t dropped() const { return dropped_; }

  // Producteur : ajoute n echantillons (les plus recents sont perdus si la
  // file est pleine - ne devrait pas arriver, voir TvAudioPlayer).
  void push(const int16_t *s, uint32_t n) {
    uint32_t room = SIZE - count();
    if (n > room) {
      dropped_ += n - room;
      n = room;
    }
    uint32_t h = head_;
    for (uint32_t i = 0; i < n; i++) buf_[(h + i) & (SIZE - 1)] = s[i];
    __sync_synchronize();
    head_ = h + n;
  }

  // Consommateur (interruption).
  bool pop(int16_t *out) {
    uint32_t t = tail_;
    if (head_ == t) return false;
    *out = buf_[t & (SIZE - 1)];
    __sync_synchronize();
    tail_ = t + 1;
    return true;
  }

 private:
  volatile int16_t buf_[SIZE];
  volatile uint32_t head_ = 0;
  volatile uint32_t tail_ = 0;
  volatile uint32_t dropped_ = 0;
};

// Lecture a cadence fixe : attend ~80 ms de son avant de demarrer (absorbe
// l'irregularite d'arrivee des paquets, qui peuvent attendre derriere des
// bandes d'image), puis corrige tout doucement la derive entre les quartz des
// deux cartes (1 echantillon sur 256 saute ou repete, inaudible).
class TvAudioPlayer {
 public:
  static const uint32_t PREFILL = 1280;   // 80 ms
  static const uint32_t LOW_WATER = 640;   // 40 ms
  static const uint32_t HIGH_WATER = 2560; // 160 ms

  TvAudioRing ring;

  void reset() {
    ring.reset();
    playing_ = false;
    repeat_ = false;
    phase_ = 0;
    last_ = 0;
    underruns_ = 0;
    corrections_ = 0;
  }

  // Appelee a 16 kHz (routine d'interruption). Renvoie l'echantillon a jouer.
  int16_t next() {
    if (!playing_) {
      if (ring.count() < PREFILL) return 0;
      playing_ = true;
    }
    if (repeat_) {
      repeat_ = false;
      return last_;
    }
    int16_t s;
    if (!ring.pop(&s)) {
      playing_ = false;
      underruns_ = underruns_ + 1;
      last_ = 0;
      return 0;
    }
    if (++phase_ >= 256) {
      phase_ = 0;
      uint32_t c = ring.count();
      if (c > HIGH_WATER) {
        int16_t skip;
        ring.pop(&skip);
        corrections_ = corrections_ + 1;
      } else if (c < LOW_WATER) {
        repeat_ = true;
        corrections_ = corrections_ + 1;
      }
    }
    last_ = s;
    return s;
  }

  bool playing() const { return playing_; }
  uint32_t underruns() const { return underruns_; }
  uint32_t corrections() const { return corrections_; }

 private:
  volatile bool playing_ = false;
  bool repeat_ = false;
  uint32_t phase_ = 0;
  int16_t last_ = 0;
  volatile uint32_t underruns_ = 0;
  volatile uint32_t corrections_ = 0;
};

#endif  // TV_LINK_RECEIVER_H
