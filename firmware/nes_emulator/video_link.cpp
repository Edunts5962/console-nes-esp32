// video_link.cpp - liaison vers la carte video TV : voir video_link.h
// (principe, brochage) et tv_link_protocol.h (format des paquets).
//
// SPI maitre a 10 MHz sur le bus SPI2 (l'ecran utilise SPI3), transferts
// par DMA. Chaque bande de 15 lignes est envoyee avec son numero et une
// somme de controle, compressee si c'est rentable (tv_link_codec.h) ; seules
// les bandes modifiees partent, plus un rafraichissement complet periodique.
// Le son (paquets de 256 echantillons) passe en priorite.
#include "video_link.h"
#include <Arduino.h>
#include "esp_system.h"

// ---------------------------------------------------------------------------
// Memoire du choix "Televiseur" a travers le redemarrage de fin de partie.
// RTC_NOINIT : cette variable survit a un redemarrage logiciel
// (ESP.restart), pas a une coupure de courant (contenu alors aleatoire,
// d'ou le mot magique et le controle de la cause du redemarrage).
// ---------------------------------------------------------------------------
#define TV_MODE_MAGIC 0x54564F4Eu   // "TVON"
RTC_NOINIT_ATTR static uint32_t s_rtc_tv_mode;

void video_link_remember_tv_mode(bool on) {
  s_rtc_tv_mode = on ? TV_MODE_MAGIC : 0;
}

bool video_link_recall_tv_mode() {
#if ENABLE_VIDEO_LINK
  return esp_reset_reason() == ESP_RST_SW && s_rtc_tv_mode == TV_MODE_MAGIC;
#else
  return false;
#endif
}

#if ENABLE_VIDEO_LINK

#include <string.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "tv_link_codec.h"

// Bus SPI2 : sur l'ESP32-S3, TFT_eSPI avec USE_HSPI_PORT prend SPI3 pour
// l'ecran (SPIClass(HSPI) = "SPI 3 bus" dans le coeur Arduino). Partager ce
// bus bloquerait la liaison ; SPI2 est libre sur cette carte (carte SD en
// SD_MMC, tactile et codec en I2C).
#define VIDEO_LINK_SPI_HOST SPI2_HOST

#define AUDIO_TX_BUFFERS   4      // 4 paquets de 16 ms peuvent attendre dans la file
#define AUDIO_RING_SIZE    4096   // 256 ms d'avance cote console
#define FULL_REFRESH_MS    2000   // toutes les bandes renvoyees au moins toutes les 2 s
#define CONFIG_PERIOD_MS   1000   // configuration (= signal de presence) chaque seconde
#define STATS_PERIOD_MS    5000

extern bool g_video_pal_mode;   // osd.cpp / hw_config.h

struct TxSlot {
  uint8_t *buf;
  spi_transaction_t t;
  volatile bool busy;
};

static spi_device_handle_t s_spi = NULL;
static bool s_ready = false;

// ---- Image ----
static bool s_frame_buffers_ok = false;
static unsigned long s_frame_buffers_retry_at = 0;   // Nouvel essai apres un manque de memoire
static uint8_t *s_snapshot = NULL;                  // image figee, indices de couleur (PSRAM de preference)
static uint32_t s_hash_cur[TVLINK_NUM_BANDS];       // empreinte des bandes de s_snapshot
static uint32_t s_hash_sent[TVLINK_NUM_BANDS];   // empreinte de la derniere bande envoyee
static uint32_t s_pending = 0;                      // bandes de s_snapshot pas encore en file
static int s_last_band = -1;
static uint8_t s_frame_id = 0;
static bool s_force_full = true;
static unsigned long s_last_full_ms = 0;
static unsigned long s_last_submit_ms = 0;
static TxSlot s_band_slots[VIDEO_LINK_TX_BUFFERS];
static int s_band_slot_count = 0;
static uint8_t s_raw_skip[TVLINK_NUM_BANDS];        // bande partie brute -> 7 envois bruts sans essai

// ---- Palette ----
#define PALETTE_PERIOD_MS 1000                      // renvoi periodique (carte video redemarree...)
static TxSlot s_palette_slot;
static uint8_t *s_palette = NULL;                   // palette de l'image figee (768 o, PSRAM, prise avec l'image)
static uint32_t s_palette_hash = 0;
static bool s_palette_known = false;
static bool s_palette_dirty = false;                // a envoyer AVANT les bandes en attente
static uint8_t s_palette_serial = 0;
static unsigned long s_last_palette_ms = 0;

// ---- Son ----
static TxSlot s_audio_slots[AUDIO_TX_BUFFERS];
static int16_t s_ring[AUDIO_RING_SIZE];
static int s_ring_head = 0, s_ring_tail = 0, s_ring_count = 0;

// ---- Configuration ----
static TxSlot s_config_slot;
static uint8_t s_norm = TVLINK_NORM_PAL;   // TVLINK_NORM_* (PAL 60 en plus)
static bool s_progressive = false;
static bool s_in_game = false;
static bool s_config_dirty = true;
static unsigned long s_last_config_ms = 0;

// ---- Statistiques (port serie, toutes les 5 s en mode Televiseur) ----
static uint32_t st_frames_sent = 0, st_frames_same = 0, st_frames_busy = 0;
static uint32_t st_bands = 0, st_audio_packets = 0, st_audio_dropped = 0, st_queue_errors = 0;
static uint32_t st_band_bytes = 0, st_raw_bands = 0, st_nibble_bands = 0, st_palettes = 0, st_encode_us = 0;
static unsigned long st_last_ms = 0;

static void poll_completed() {
  spi_transaction_t *done;
  while (spi_device_get_trans_result(s_spi, &done, 0) == ESP_OK) {
    TxSlot *slot = (TxSlot *)done->user;
    if (slot) slot->busy = false;
  }
}

static bool queue_slot(TxSlot *slot, size_t len) {
  memset(&slot->t, 0, sizeof(slot->t));
  slot->t.length = len * 8;   // en bits
  slot->t.tx_buffer = slot->buf;
  slot->t.user = slot;
  slot->busy = true;
  if (spi_device_queue_trans(s_spi, &slot->t, 0) != ESP_OK) {
    slot->busy = false;
    st_queue_errors++;
    return false;
  }
  return true;
}

static TxSlot *free_slot(TxSlot *slots, int n) {
  for (int i = 0; i < n; i++)
    if (!slots[i].busy) return &slots[i];
  return NULL;
}

static uint8_t *dma_alloc(size_t n) {
  return (uint8_t *)heap_caps_malloc(n, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
}

// Pause apres chaque paquet, CS releve, avant le suivant (appelee par le
// pilote SPI entre deux transactions) : la carte video doit avoir prepare la
// reception du paquet suivant. Sans elle, deux paquets envoyes de suite
// pouvaient arriver colles, ou le debut du second etre perdu (bandes refusees,
// restes de l'image precedente sur la television).
static void IRAM_ATTR link_post_cb(spi_transaction_t *t) {
  (void)t;
  esp_rom_delay_us(VIDEO_LINK_GAP_US);
}

void video_link_init() {
  // PAL 60 compte comme "PAL" pour g_video_pal_mode - une norme deja choisie
  // (video_link_set_video_norm) n'est pas ecrasee si elle concorde.
  if ((s_norm != TVLINK_NORM_NTSC) != g_video_pal_mode)
    s_norm = g_video_pal_mode ? TVLINK_NORM_PAL : TVLINK_NORM_NTSC;

  for (int i = 0; i < AUDIO_TX_BUFFERS; i++) {
    s_audio_slots[i].buf = dma_alloc(TVLINK_AUDIO_PACKET_BYTES);
    s_audio_slots[i].busy = false;
  }
  s_config_slot.buf = dma_alloc(TVLINK_CONFIG_PACKET_BYTES);
  s_config_slot.busy = false;
  s_palette_slot.buf = dma_alloc(TVLINK_PALETTE_PACKET_BYTES);
  s_palette_slot.busy = false;
  bool ok = s_config_slot.buf != NULL && s_palette_slot.buf != NULL;
  for (int i = 0; i < AUDIO_TX_BUFFERS; i++) ok = ok && s_audio_slots[i].buf != NULL;
  if (!ok) {
    Serial.println("[VIDEO-LINK] memoire DMA insuffisante : liaison desactivee.");
    return;
  }

  spi_bus_config_t buscfg;
  memset(&buscfg, 0, sizeof(buscfg));
  buscfg.mosi_io_num = VIDEO_LINK_PIN_MOSI;
  buscfg.miso_io_num = -1;   // sens unique
  buscfg.sclk_io_num = VIDEO_LINK_PIN_SCLK;
  buscfg.quadwp_io_num = -1;
  buscfg.quadhd_io_num = -1;
  buscfg.max_transfer_sz = TVLINK_MAX_PACKET_BYTES;

  esp_err_t err = spi_bus_initialize(VIDEO_LINK_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
  if (err != ESP_OK) {
    Serial.printf("[VIDEO-LINK] spi_bus_initialize a echoue (%d) : liaison desactivee.\n", (int)err);
    return;
  }

  spi_device_interface_config_t devcfg;
  memset(&devcfg, 0, sizeof(devcfg));
  devcfg.clock_speed_hz = VIDEO_LINK_SPI_HZ;
  devcfg.mode = 0;
  devcfg.spics_io_num = VIDEO_LINK_PIN_CS;
  devcfg.queue_size = VIDEO_LINK_TX_BUFFERS + AUDIO_TX_BUFFERS + 2;   // + configuration + palette
  devcfg.post_cb = link_post_cb;   // Pause entre deux paquets
  err = spi_bus_add_device(VIDEO_LINK_SPI_HOST, &devcfg, &s_spi);
  if (err != ESP_OK) {
    Serial.printf("[VIDEO-LINK] spi_bus_add_device a echoue (%d) : liaison desactivee.\n", (int)err);
    spi_bus_free(VIDEO_LINK_SPI_HOST);
    return;
  }

  // Fronts plus doux sur les fils de la liaison (voir video_link.h).
  const gpio_drive_cap_t cap = (gpio_drive_cap_t)VIDEO_LINK_DRIVE_CAP;
  gpio_set_drive_capability((gpio_num_t)VIDEO_LINK_PIN_SCLK, cap);
  gpio_set_drive_capability((gpio_num_t)VIDEO_LINK_PIN_CS, cap);
  gpio_set_drive_capability((gpio_num_t)VIDEO_LINK_PIN_MOSI, cap);

  s_ready = true;
  Serial.printf("[VIDEO-LINK] pret (protocole v%d) : SCLK=GPIO%d CS=GPIO%d MOSI=GPIO%d, %d Hz, sorties %d mA, "
                "%d us entre deux paquets. Tampons image pris a la premiere image TV.\n",
                TVLINK_PROTOCOL_VERSION, VIDEO_LINK_PIN_SCLK, VIDEO_LINK_PIN_CS, VIDEO_LINK_PIN_MOSI,
                (int)VIDEO_LINK_SPI_HZ, 5 << VIDEO_LINK_DRIVE_CAP, VIDEO_LINK_GAP_US);
}

bool video_link_is_ready() {
  return s_ready;
}

// Tampons de l'image, pris a la premiere image TV.
// La premiere image peut venir des menus (liste des jeux sur la television),
// Wi-Fi et Bluetooth actifs : dans les menus, 2 tampons DMA seulement (7,7 Ko
// de memoire interne, une liste change peu) ; les suivants sont pris en
// partie. En cas de manque de memoire, nouvel essai 3 s plus tard au lieu
// d'abandonner l'image pour toute la session.
#define MENU_TX_BUFFERS 2
static bool s_tick_in_game = false;   // dernier video_link_tick() : partie (true) ou menus

static bool frame_buffers() {
  const int want = s_tick_in_game ? VIDEO_LINK_TX_BUFFERS : MENU_TX_BUFFERS;
  if (s_frame_buffers_ok && s_band_slot_count >= want) return true;
  if (s_frame_buffers_retry_at && (long)(millis() - s_frame_buffers_retry_at) < 0) return s_frame_buffers_ok;
  bool in_psram = true;
  if (!s_palette) {   // En PSRAM (la memoire interne est comptee pour le Wi-Fi)
    s_palette = (uint8_t *)heap_caps_malloc(TVLINK_PALETTE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_palette) s_palette = (uint8_t *)heap_caps_malloc(TVLINK_PALETTE_BYTES, MALLOC_CAP_8BIT);
    if (!s_palette) return false;
  }
  if (!s_snapshot) {
    const size_t frame_bytes = (size_t)TVLINK_WIDTH * TVLINK_HEIGHT;
    s_snapshot = (uint8_t *)heap_caps_malloc(frame_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    in_psram = s_snapshot != NULL;
    if (!s_snapshot) s_snapshot = (uint8_t *)heap_caps_malloc(frame_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  const int before = s_band_slot_count;
  if (s_snapshot) {
    while (s_band_slot_count < want) {
      TxSlot *slot = &s_band_slots[s_band_slot_count];
      slot->buf = dma_alloc(TVLINK_BAND_PACKET_BYTES);
      slot->busy = false;
      if (!slot->buf) break;
      s_band_slot_count++;
    }
  }
  if (!s_snapshot || s_band_slot_count < 2) {
    for (int i = 0; i < s_band_slot_count; i++) {
      heap_caps_free(s_band_slots[i].buf);
      s_band_slots[i].buf = NULL;
    }
    s_band_slot_count = 0;
    if (s_snapshot) heap_caps_free(s_snapshot);
    s_snapshot = NULL;
    s_frame_buffers_ok = false;
    s_frame_buffers_retry_at = millis() + 3000;
    Serial.printf("[VIDEO-LINK] memoire insuffisante pour l'image (interne DMA libre : %u o) : "
                  "nouvel essai dans 3 s.\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    return false;
  }
  if (s_band_slot_count < want) s_frame_buffers_retry_at = millis() + 3000;   // complement plus tard
  if (s_band_slot_count != before) {
    Serial.printf("[VIDEO-LINK] tampons image : 60 Ko%s + %d x %d o DMA internes (%s).\n",
                  in_psram ? " en PSRAM" : "", s_band_slot_count, TVLINK_BAND_PACKET_BYTES,
                  s_tick_in_game ? "partie" : "menus");
  }
  s_frame_buffers_ok = true;
  return true;
}

// Fige l'image courante et calcule l'empreinte de chaque bande.
static void capture_frame(uint8_t *const *lines) {
  for (int b = 0; b < TVLINK_NUM_BANDS; b++) {
    uint8_t *band = s_snapshot + (size_t)b * TVLINK_BAND_BYTES;
    for (int r = 0; r < TVLINK_BAND_ROWS; r++)
      memcpy(band + r * TVLINK_WIDTH, lines[b * TVLINK_BAND_ROWS + r], TVLINK_WIDTH);
    s_hash_cur[b] = tvlink_hash32(band, TVLINK_BAND_BYTES);
  }
}

// Met la palette en file (elle doit partir AVANT les bandes de l'image qui
// s'en sert : la file SPI garde l'ordre). Faux si son tampon est encore occupe
// par l'envoi precedent.
static bool queue_palette() {
  if (s_palette_slot.busy) return false;
  uint8_t *p = s_palette_slot.buf;
  tvlink_put_magic(p, TVLINK_TYPE_PALETTE);
  p[4] = s_palette_serial;
  p[5] = 0;
  const uint16_t ck = tvlink_fold16(tvlink_hash32(s_palette, TVLINK_PALETTE_BYTES));
  p[6] = (uint8_t)(ck & 0xFF);
  p[7] = (uint8_t)(ck >> 8);
  memcpy(p + TVLINK_HEADER_SIZE, s_palette, TVLINK_PALETTE_BYTES);
  if (!queue_slot(&s_palette_slot, TVLINK_PALETTE_PACKET_BYTES)) return false;
  s_last_palette_ms = millis();
  st_palettes++;
  return true;
}

// Met en file autant de bandes en attente que de tampons DMA libres.
// Palette d'abord si elle a change ; bandes compressees.
static void pump_bands() {
  if (s_palette_dirty) {
    if (!queue_palette()) return;   // les bandes attendent la palette
    s_palette_dirty = false;
  }
  while (s_pending) {
    TxSlot *slot = free_slot(s_band_slots, s_band_slot_count);
    if (!slot) return;
    int b = __builtin_ctz(s_pending);
    uint8_t *p = slot->buf;
    const uint8_t *src = s_snapshot + (size_t)b * TVLINK_BAND_BYTES;
    tvlink_put_magic(p, TVLINK_TYPE_BAND);
    p[4] = s_frame_id;
    uint8_t flags = (uint8_t)(b | (b == s_last_band ? TVLINK_BAND_LAST : 0));
    uint16_t ck = tvlink_fold16(s_hash_cur[b]);
    p[6] = (uint8_t)(ck & 0xFF);
    p[7] = (uint8_t)(ck >> 8);
    size_t len = TVLINK_BAND_PACKET_BYTES;
    size_t packed = 0;
    if (s_raw_skip[b]) {
      s_raw_skip[b]--;
    } else {
      const uint32_t t0 = micros();
      // Au plus 3840 - 64 octets : moins, ca ne vaut pas la peine.
      packed = tvlink_band_encode(src, TVLINK_BAND_BYTES, p + TVLINK_HEADER_SIZE, TVLINK_BAND_BYTES - 64);
      st_encode_us += micros() - t0;
      if (!packed) s_raw_skip[b] = 7;   // bande tres detaillee : brute pour les 7 prochains envois
    }
    size_t nibble = 0;
    if (!packed) {
      // Image tres detaillee, mais 16 couleurs au plus dans la bande (presque
      // toujours sur NES) : 4 bits par pixel, moitie moins d'octets.
      nibble = tvlink_band_pack4(src, TVLINK_BAND_BYTES, p + TVLINK_HEADER_SIZE, TVLINK_BAND_BYTES);
    }
    if (packed) {
      flags |= TVLINK_BAND_PACKED;
      len = TVLINK_HEADER_SIZE + packed;
      while (len & 3) p[len++] = 0;     // bourrage nul jusqu'a un multiple de 4 (DMA de l'esclave)
    } else if (nibble) {
      flags |= TVLINK_BAND_NIBBLE;
      len = TVLINK_HEADER_SIZE + nibble;
      while (len & 3) p[len++] = 0;
      st_nibble_bands++;
    } else {
      memcpy(p + TVLINK_HEADER_SIZE, src, TVLINK_BAND_BYTES);
      st_raw_bands++;
    }
    p[5] = flags;
    if (!queue_slot(slot, len)) return;
    s_hash_sent[b] = s_hash_cur[b];
    s_pending &= ~(1u << b);
    st_bands++;
    st_band_bytes += (uint32_t)len;
  }
}

static void pump_audio() {
  while (s_ring_count >= TVLINK_AUDIO_SAMPLES) {
    TxSlot *slot = free_slot(s_audio_slots, AUDIO_TX_BUFFERS);
    if (!slot) return;
    uint8_t *p = slot->buf;
    tvlink_put_magic(p, TVLINK_TYPE_AUDIO);
    p[4] = (uint8_t)(TVLINK_AUDIO_SAMPLES & 0xFF);
    p[5] = (uint8_t)(TVLINK_AUDIO_SAMPLES >> 8);
    int tail = s_ring_tail;
    for (int i = 0; i < TVLINK_AUDIO_SAMPLES; i++) {
      int16_t s = s_ring[tail];
      p[TVLINK_HEADER_SIZE + 2 * i] = (uint8_t)(s & 0xFF);
      p[TVLINK_HEADER_SIZE + 2 * i + 1] = (uint8_t)((uint16_t)s >> 8);
      tail = (tail + 1) % AUDIO_RING_SIZE;
    }
    uint16_t ck = tvlink_fold16(tvlink_hash32(p + TVLINK_HEADER_SIZE, TVLINK_AUDIO_SAMPLES * 2));
    p[6] = (uint8_t)(ck & 0xFF);
    p[7] = (uint8_t)(ck >> 8);
    if (!queue_slot(slot, TVLINK_AUDIO_PACKET_BYTES)) return;
    s_ring_tail = tail;               // echantillons retires seulement une fois en file
    s_ring_count -= TVLINK_AUDIO_SAMPLES;
    st_audio_packets++;
  }
}

static bool send_config() {
  if (s_config_slot.busy) return false;
  uint8_t *p = s_config_slot.buf;
  tvlink_put_magic(p, TVLINK_TYPE_CONFIG);
  p[4] = TVLINK_PROTOCOL_VERSION;
  p[5] = s_norm;
  p[6] = (uint8_t)((s_in_game ? TVLINK_FLAG_IN_GAME : 0) | (s_progressive ? TVLINK_FLAG_PROGRESSIVE : 0));
  p[7] = (uint8_t)(0xA5 ^ p[4] ^ p[5] ^ p[6]);
  if (!queue_slot(&s_config_slot, TVLINK_CONFIG_PACKET_BYTES)) return false;
  s_last_config_ms = millis();
  return true;
}

static void print_stats(unsigned long now) {
  if (now - st_last_ms < STATS_PERIOD_MS) return;
  float secs = (now - st_last_ms) / 1000.0f;
  if (st_last_ms != 0) {
    Serial.printf("[VIDEO-LINK] images/s : envoyees %.1f, identiques %.1f, sautees %.1f | bandes/s %.0f, "
                  "taille %.0f %% (%lu en 16 couleurs, %lu brutes), compression %.2f ms/image | palettes %lu | "
                  "son : %lu paquets, %lu ech. perdus | erreurs de file %lu\n",
                  st_frames_sent / secs, st_frames_same / secs, st_frames_busy / secs, st_bands / secs,
                  st_bands ? 100.0f * st_band_bytes / ((float)st_bands * TVLINK_BAND_PACKET_BYTES) : 0.0f,
                  (unsigned long)st_nibble_bands, (unsigned long)st_raw_bands,
                  st_frames_sent ? st_encode_us / 1000.0f / st_frames_sent : 0.0f, (unsigned long)st_palettes,
                  (unsigned long)st_audio_packets, (unsigned long)st_audio_dropped,
                  (unsigned long)st_queue_errors);
  }
  st_frames_sent = st_frames_same = st_frames_busy = st_bands = 0;
  st_band_bytes = st_raw_bands = st_nibble_bands = st_palettes = st_encode_us = 0;
  st_last_ms = now;
}

bool video_link_submit_frame(uint8_t *const *lines, const uint8_t *palette_rgb888, uint32_t full_refresh_ms) {
  if (!s_ready || !lines || !palette_rgb888) return false;
  unsigned long now = millis();
  s_in_game = true;
  if (now - s_last_submit_ms > 500) s_force_full = true;   // (re)debut de partie en mode TV
  s_last_submit_ms = now;
  poll_completed();
  if (!frame_buffers()) return false;

  bool taken = true;
  if (s_pending == 0 && !s_palette_dirty) {
    capture_frame(lines);
    // Palette de cette image. Nouvelle palette : elle part avant les bandes,
    // et toutes les bandes repartent (leurs indices changent de sens). Sinon,
    // rappel chaque seconde (carte video redemarree...).
    const uint32_t ph = tvlink_hash32(palette_rgb888, TVLINK_PALETTE_BYTES);
    if (!s_palette_known || ph != s_palette_hash ||
        memcmp(s_palette, palette_rgb888, TVLINK_PALETTE_BYTES) != 0) {
      memcpy(s_palette, palette_rgb888, TVLINK_PALETTE_BYTES);
      s_palette_hash = ph;
      s_palette_known = true;
      s_palette_serial++;
      s_palette_dirty = true;
      s_force_full = true;
    } else if (now - s_last_palette_ms >= PALETTE_PERIOD_MS) {
      s_palette_dirty = true;
    }
    if (now - s_last_full_ms >= full_refresh_ms) s_force_full = true;
    uint32_t mask = 0;
    for (int b = 0; b < TVLINK_NUM_BANDS; b++)
      if (s_force_full || s_hash_cur[b] != s_hash_sent[b]) mask |= 1u << b;
    if (s_force_full) s_last_full_ms = now;
    s_force_full = false;
    if (mask) {
      s_pending = mask;
      s_last_band = 31 - __builtin_clz(mask);
      s_frame_id++;
      st_frames_sent++;
    } else {
      st_frames_same++;   // rien n'a bouge : la TV garde l'image precedente
    }
  } else {
    st_frames_busy++;     // image precedente pas encore partie : celle-ci est sautee
    taken = false;
  }
  pump_bands();
  print_stats(now);
  return taken;
}

void video_link_queue_audio(const int16_t *samples, int count) {
  if (!s_ready || !samples || count <= 0) return;
  for (int i = 0; i < count; i++) {
    s_ring[s_ring_head] = samples[i];
    s_ring_head = (s_ring_head + 1) % AUDIO_RING_SIZE;
    if (s_ring_count < AUDIO_RING_SIZE) {
      s_ring_count++;
    } else {
      s_ring_tail = (s_ring_tail + 1) % AUDIO_RING_SIZE;   // on perd le plus ancien
      st_audio_dropped++;
    }
  }
  poll_completed();
  pump_audio();
  pump_bands();
}

void video_link_set_video_standard(bool pal) {
  video_link_set_video_norm(pal ? TVLINK_NORM_PAL : TVLINK_NORM_NTSC);
}

uint8_t video_link_video_norm() {
  return s_norm;
}

void video_link_set_video_norm(uint8_t norm) {
  if (norm > TVLINK_NORM_MAX) norm = TVLINK_NORM_PAL;
  if (norm != s_norm) s_config_dirty = true;
  s_norm = norm;
  if (!s_ready) return;
  poll_completed();
  if (send_config()) s_config_dirty = false;
}

void video_link_set_progressive(bool progressive) {
  s_progressive = progressive;
  if (!s_ready) return;
  s_config_dirty = true;
  poll_completed();
  if (send_config()) s_config_dirty = false;
}

const uint8_t *video_link_rgb332_palette() {
  // En PSRAM, prise au premier usage (sortie Televiseur seulement).
  static uint8_t *pal = NULL;
  if (!pal) {
    pal = (uint8_t *)heap_caps_malloc(TVLINK_PALETTE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pal) pal = (uint8_t *)heap_caps_malloc(TVLINK_PALETTE_BYTES, MALLOC_CAP_8BIT);
    if (!pal) return NULL;   // video_link_submit_frame() refuse alors l'image
    for (int i = 0; i < 256; i++) tvlink_rgb332_to_888((uint8_t)i, &pal[i * 3]);
  }
  return pal;
}

void video_link_tick(bool in_game) {
  if (!s_ready) return;
  s_in_game = in_game;
  s_tick_in_game = in_game;   // Nombre de tampons d'image voulus
  poll_completed();
  unsigned long now = millis();
  if (s_config_dirty || now - s_last_config_ms >= CONFIG_PERIOD_MS) {
    if (send_config()) s_config_dirty = false;
  }
  if (in_game) pump_audio();
  // Les bandes en attente partent aussi depuis les menus (liste des jeux sur
  // la television) - avant, seules celles mises en file par
  // video_link_submit_frame() partaient, et une image des menus restait
  // incomplete jusqu'a l'envoi suivant.
  pump_bands();
}

// Attend que l'image en cours soit entierement partie (ecran "Chargement..."
// juste avant un chargement de jeu, pendant lequel plus rien n'entretient
// l'envoi). timeout_ms : attente maximale.
void video_link_flush(uint32_t timeout_ms) {
  if (!s_ready) return;
  const unsigned long t0 = millis();
  for (;;) {
    poll_completed();
    pump_bands();
    bool busy = s_pending != 0 || s_palette_dirty || s_palette_slot.busy;
    for (int i = 0; !busy && i < s_band_slot_count; i++) busy = s_band_slots[i].busy;
    if (!busy || millis() - t0 >= timeout_ms) return;
    delay(2);
  }
}

#else  // ENABLE_VIDEO_LINK == 0 : liaison absente, fonctions vides

void video_link_init() {}
bool video_link_is_ready() { return false; }
bool video_link_submit_frame(uint8_t *const *lines, const uint8_t *palette_rgb888, uint32_t full_refresh_ms) {
  (void)lines; (void)palette_rgb888; (void)full_refresh_ms;
  return false;
}
const uint8_t *video_link_rgb332_palette() { return NULL; }
void video_link_set_progressive(bool progressive) { (void)progressive; }
void video_link_queue_audio(const int16_t *samples, int count) { (void)samples; (void)count; }
void video_link_set_video_standard(bool pal) { (void)pal; }
void video_link_set_video_norm(uint8_t norm) { (void)norm; }
uint8_t video_link_video_norm() { return TVLINK_NORM_PAL; }
void video_link_tick(bool in_game) { (void)in_game; }
void video_link_flush(uint32_t timeout_ms) { (void)timeout_ms; }

#endif
