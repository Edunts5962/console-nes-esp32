// tv_menu.cpp - liste des jeux sur la television (voir tv_menu.h). Dessin
// direct en RGB332 dans une image de 256 x 240 en PSRAM, envoyee par la
// liaison video comme une image de jeu.
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <lvgl.h>
#include <string.h>
#include "tv_menu.h"
#include "video_link.h"

LV_FONT_DECLARE(ui_font_12);
LV_FONT_DECLARE(ui_font_14);
LV_FONT_DECLARE(ui_font_20);

#define W TVLINK_WIDTH
#define H TVLINK_HEIGHT

// Marges : un televiseur cache souvent le bord de l'image (surbalayage).
// En NTSC, la carte video ne montre pas les 8 colonnes de chaque bord (3
// echantillons par pixel, image nette) : marge elargie d'autant.
static int s_margin_x = 12;
#define MARGIN_X   s_margin_x
// Contenu resserre vers le centre.
#define TITLE_Y    15
#define SUB_Y      38
#define LIST_Y     58
#define ROW_H      21
#define FOOT_Y     (LIST_Y + TVM_VISIBLE_ROWS * ROW_H + 4)

// Couleurs RGB332 : rrrgggbb
static inline uint8_t rgb332(int r, int g, int b) { return (uint8_t)((r << 5) | (g << 2) | b); }
// Couleurs choisies pour la video composite. Le signal composite melange la
// couleur et la luminosite : un aplat tres sature bave sur ses voisins et rend
// le texte flou, du texte clair sur fond colore "scintille". La lisibilite
// vient surtout de la LUMINOSITE : fond noir, gris presque neutres, barre de
// selection claire avec texte noir.
#define COL_BG      rgb332(0, 0, 0)   // noir
#define COL_BAND    rgb332(2, 2, 1)   // gris fonce (bandeau du titre, encadres)
#define COL_LINE    rgb332(3, 3, 1)   // gris moyen
#define COL_TITLE   rgb332(7, 7, 3)   // blanc
#define COL_TEXT    rgb332(6, 6, 3)   // gris clair
#define COL_DIM     rgb332(4, 4, 2)   // gris
#define COL_CURSOR  rgb332(6, 6, 3)   // barre de selection claire...
#define COL_CUR_TXT rgb332(0, 0, 0)   // ...et texte noir dessus
#define COL_BAD     rgb332(7, 5, 0)   // orange
#define COL_OK      rgb332(3, 7, 1)   // vert clair ("EN COURS")
#define COL_ERR     rgb332(7, 3, 1)
#define COL_INFO    rgb332(7, 7, 1)   // jaune pale

static uint8_t *s_fb = NULL;
static uint8_t *s_lines[H];
static int s_top = 0;   // premiere ligne de la liste a l'ecran
static int s_drawn_count = 0;   // Lignes de la derniere liste dessinee

static bool fb_ready() {
  static uint32_t retry_at = 0;
  if (s_fb) return true;
  if (retry_at && (int32_t)(millis() - retry_at) < 0) return false;   // nouvel essai toutes les 3 s
  s_fb = (uint8_t *)heap_caps_malloc((size_t)W * H, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_fb) s_fb = (uint8_t *)heap_caps_malloc((size_t)W * H, MALLOC_CAP_8BIT);
  if (!s_fb) {
    Serial.println("[TV-MENU] Pas de memoire pour l'image de la liste (nouvel essai dans 3 s).");
    retry_at = millis() + 3000;
    return false;
  }
  for (int y = 0; y < H; y++) s_lines[y] = s_fb + (size_t)y * W;
  return true;
}

// ---------------------------------------------------------------------------
// Dessin de base
// ---------------------------------------------------------------------------
static void fill_rect(int x, int y, int w, int h, uint8_t c) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > W) w = W - x;
  if (y + h > H) h = H - y;
  if (w <= 0 || h <= 0) return;
  for (int j = y; j < y + h; j++) memset(s_lines[j] + x, c, (size_t)w);
}

// Rectangle aux coins arrondis (rayon 3) : coins en couleur du fond laisses.
static void fill_round(int x, int y, int w, int h, uint8_t c) {
  fill_rect(x + 3, y, w - 6, h, c);
  fill_rect(x, y + 3, 3, h - 6, c);
  fill_rect(x + w - 3, y + 3, 3, h - 6, c);
  fill_rect(x + 1, y + 1, 2, 2, c);
  fill_rect(x + w - 3, y + 1, 2, 2, c);
  fill_rect(x + 1, y + h - 3, 2, 2, c);
  fill_rect(x + w - 3, y + h - 3, 2, 2, c);
}

// Melange de deux couleurs RGB332 (a = 0..15 : part de fg).
static uint8_t mix(uint8_t fg, uint8_t bg, int a) {
  if (a >= 15) return fg;
  if (a <= 0) return bg;
  const int r = (((fg >> 5) & 7) * a + ((bg >> 5) & 7) * (15 - a) + 7) / 15;
  const int g = (((fg >> 2) & 7) * a + ((bg >> 2) & 7) * (15 - a) + 7) / 15;
  const int b = ((fg & 3) * a + (bg & 3) * (15 - a) + 7) / 15;
  return rgb332(r, g, b);
}

// Fond : image du jeu assombrie (au quart) ou couleur unie.
static void draw_background(uint8_t *const *bg_lines, const uint8_t *bg_lut) {
  if (!bg_lines || !bg_lut) {
    memset(s_fb, COL_BG, (size_t)W * H);
    return;
  }
  for (int y = 0; y < H; y++) {
    const uint8_t *src = bg_lines[y];
    uint8_t *dst = s_lines[y];
    for (int x = 0; x < W; x++) {
      const uint8_t c = bg_lut[src[x]];
      // Plus sombre (bleu compris), l'image du jeu ne se lit plus comme une
      // remanence derriere le texte.
      dst[x] = rgb332(((c >> 5) & 7) >> 2, ((c >> 2) & 7) >> 2, (c & 3) >> 2);
    }
  }
}

// ---------------------------------------------------------------------------
// Texte (polices LVGL lues directement, 4 bits par pixel)
// ---------------------------------------------------------------------------
static uint32_t utf8_next(const char *s, int *i) {
  const uint8_t c = (uint8_t)s[*i];
  if (c < 0x80) { (*i)++; return c; }
  int n = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0;
  uint32_t cp = (n == 3) ? (c & 0x07) : (n == 2) ? (c & 0x0F) : (c & 0x1F);
  (*i)++;
  for (int k = 0; k < n; k++) {
    const uint8_t d = (uint8_t)s[*i];
    if ((d & 0xC0) != 0x80) return '?';
    cp = (cp << 6) | (d & 0x3F);
    (*i)++;
  }
  return n ? cp : '?';
}

static int glyph_adv(const lv_font_t *f, uint32_t cp, uint32_t next) {
  lv_font_glyph_dsc_t g;
  if (!lv_font_get_glyph_dsc(f, &g, cp, next)) {
    if (!lv_font_get_glyph_dsc(f, &g, '?', next)) return 0;
  }
  return g.adv_w;
}

static int text_width(const lv_font_t *f, const char *s) {
  int w = 0, i = 0;
  while (s[i]) {
    const uint32_t cp = utf8_next(s, &i);
    int j = i;
    const uint32_t nx = s[j] ? utf8_next(s, &j) : 0;
    w += glyph_adv(f, cp, nx);
  }
  return w;
}

// Dessine un caractere ; renvoie son avance.
static int draw_glyph(const lv_font_t *f, int x, int y_top, uint32_t cp, uint32_t next, uint8_t color) {
  lv_font_glyph_dsc_t g;
  if (!lv_font_get_glyph_dsc(f, &g, cp, next)) {
    cp = '?';
    if (!lv_font_get_glyph_dsc(f, &g, cp, next)) return 0;
  }
  const uint8_t *bmp = lv_font_get_glyph_bitmap(f, cp);
  if (!bmp || g.bpp != 4) return g.adv_w;
  const int baseline = y_top + f->line_height - f->base_line;
  const int gx = x + g.ofs_x;
  const int gy = baseline - g.box_h - g.ofs_y;
  for (int r = 0; r < g.box_h; r++) {
    const int py = gy + r;
    if (py < 0 || py >= H) continue;
    uint8_t *line = s_lines[py];
    for (int c = 0; c < g.box_w; c++) {
      const int px = gx + c;
      if (px < 0 || px >= W) continue;
      const int bit = (r * g.box_w + c) * 4;
      const int a = (bmp[bit >> 3] >> (4 - (bit & 7))) & 0x0F;
      if (a) line[px] = mix(color, line[px], a);
    }
  }
  return g.adv_w;
}

// Texte sur une ligne, coupe avec "..." au-dela de max_w (0 : sans limite).
// Renvoie la largeur dessinee.
static int draw_text(const lv_font_t *f, int x, int y, const char *s, uint8_t color, int max_w) {
  if (!s) return 0;
  const int ell_w = glyph_adv(f, 0x2026, 0);
  const bool cut = max_w > 0 && text_width(f, s) > max_w;
  const int limit = cut ? max_w - ell_w : 0x7FFF;
  int cx = x, i = 0;
  while (s[i]) {
    const int i0 = i;
    const uint32_t cp = utf8_next(s, &i);
    int j = i;
    const uint32_t nx = s[j] ? utf8_next(s, &j) : 0;
    if (cut && cx - x + glyph_adv(f, cp, nx) > limit) {
      (void)i0;
      break;
    }
    cx += draw_glyph(f, cx, y, cp < 0x20 ? ' ' : cp, nx, color);
  }
  if (cut) cx += draw_glyph(f, cx, y, 0x2026, 0, color);
  return cx - x;
}

static void draw_text_centered(const lv_font_t *f, int y, const char *s, uint8_t color) {
  const int max_w = W - 2 * MARGIN_X;
  int w = text_width(f, s);
  if (w > max_w) w = max_w;
  draw_text(f, (W - w) / 2, y, s, color, max_w);
}

static void draw_triangle(int cx, int y, int dir, uint8_t color) {   // dir -1 : vers le haut
  for (int k = 0; k < 4; k++) {
    const int yy = (dir < 0) ? y + k : y + 3 - k;
    fill_rect(cx - k, yy, 2 * k + 1, 1, color);
  }
}

// ---------------------------------------------------------------------------
// Ecrans
// ---------------------------------------------------------------------------
void tv_menu_reset_scroll() {
  s_top = 0;
}

bool tv_menu_render(const TvMenuView &v, uint8_t *const *bg_lines, const uint8_t *bg_lut) {
  if (!fb_ready()) return false;
  s_margin_x = (video_link_video_norm() == TVLINK_NORM_NTSC) ? 20 : 12;
  draw_background(bg_lines, bg_lut);
  const bool over_game = bg_lines && bg_lut;

  // Titre
  if (!over_game) fill_rect(0, 0, W, LIST_Y - 6, COL_BAND);
  const int badge_w = v.badge ? text_width(&ui_font_12, v.badge) : 0;
  draw_text(&ui_font_20, MARGIN_X, TITLE_Y - 2, v.title ? v.title : "", COL_TITLE,
            W - 2 * MARGIN_X - badge_w - 8);
  if (v.badge) draw_text(&ui_font_12, W - MARGIN_X - badge_w, TITLE_Y + 6, v.badge, COL_DIM, 0);
  if (v.subtitle) draw_text(&ui_font_12, MARGIN_X, SUB_Y, v.subtitle, COL_TEXT, W - 2 * MARGIN_X);
  fill_rect(MARGIN_X, LIST_Y - 5, W - 2 * MARGIN_X, 1, COL_LINE);

  // Liste
  s_drawn_count = v.count;
  if (v.count <= 0) {
    draw_text_centered(&ui_font_14, LIST_Y + 2 * ROW_H, "Aucun jeu", COL_DIM);
  } else {
    if (v.cursor >= 0) {
      if (v.cursor < s_top) s_top = v.cursor;
      if (v.cursor >= s_top + TVM_VISIBLE_ROWS) s_top = v.cursor - TVM_VISIBLE_ROWS + 1;
    }
    if (s_top > v.count - TVM_VISIBLE_ROWS) s_top = v.count - TVM_VISIBLE_ROWS;
    if (s_top < 0) s_top = 0;
    const int text_x = MARGIN_X + 6;
    const int right = W - MARGIN_X - 10;   // place des fleches de defilement
    for (int k = 0; k < TVM_VISIBLE_ROWS && s_top + k < v.count; k++) {
      const int row = s_top + k;
      const int y = LIST_Y + k * ROW_H;
      uint8_t flags = 0;
      const char *name = v.row ? v.row(row, &flags) : "";
      const bool cur = (row == v.cursor);
      if (cur) fill_round(MARGIN_X, y, right - MARGIN_X, ROW_H - 1, COL_CURSOR);
      // Etiquettes a droite
      int tag_x = right - 4;
      if (v.value) {   // Valeur (menu pause)
        const char *val = v.value(row);
        if (val && val[0]) {
          tag_x -= text_width(&ui_font_14, val);
          draw_text(&ui_font_14, tag_x, y + 1, val, cur ? COL_CUR_TXT : COL_INFO, 0);
          tag_x -= 8;
        }
      }
      if (flags & TVM_CURRENT) {
        const char *t = "EN COURS";
        tag_x -= text_width(&ui_font_12, t);
        draw_text(&ui_font_12, tag_x, y + 2, t, cur ? COL_CUR_TXT : COL_OK, 0);
        tag_x -= 6;
      }
      if (flags & TVM_FLASH) {
        const char *t = "INT";
        tag_x -= text_width(&ui_font_12, t);
        draw_text(&ui_font_12, tag_x, y + 2, t, cur ? COL_CUR_TXT : COL_DIM, 0);
        tag_x -= 6;
      }
      if (flags & TVM_FAV) {
        const char *t = TVM_SYM_STAR;
        tag_x -= text_width(&ui_font_12, t);
        draw_text(&ui_font_12, tag_x, y + 3, t, cur ? COL_CUR_TXT : COL_INFO, 0);
        tag_x -= 6;
      }
      if (flags & TVM_BAD) {
        const char *t = "!";
        tag_x -= text_width(&ui_font_14, t) + 2;
        draw_text(&ui_font_14, tag_x, y + 1, t, cur ? COL_CUR_TXT : COL_BAD, 0);
        tag_x -= 4;
      }
      draw_text(&ui_font_14, text_x, y + 1, name,
                cur ? COL_CUR_TXT : ((flags & TVM_DIM) ? COL_DIM : COL_TEXT), tag_x - text_x);
    }
    if (s_top > 0) draw_triangle(W - MARGIN_X - 3, LIST_Y + 2, -1, COL_TEXT);
    if (s_top + TVM_VISIBLE_ROWS < v.count)
      draw_triangle(W - MARGIN_X - 3, LIST_Y + TVM_VISIBLE_ROWS * ROW_H - 7, +1, COL_TEXT);
  }

  // Bas de l'ecran : message ou indication des touches
  if (v.message) {
    draw_text_centered(&ui_font_12, FOOT_Y, v.message, v.message_error ? COL_ERR : COL_INFO);
  } else if (v.hint) {
    draw_text_centered(&ui_font_12, FOOT_Y, v.hint, COL_DIM);
  }
  return true;
}

bool tv_menu_render_notice(const char *line1, const char *line2, uint8_t *const *bg_lines,
                           const uint8_t *bg_lut) {
  if (!fb_ready()) return false;
  s_margin_x = (video_link_video_norm() == TVLINK_NORM_NTSC) ? 20 : 12;
  draw_background(bg_lines, bg_lut);
  const int box_h = 64, box_y = (H - box_h) / 2;
  fill_round(MARGIN_X + 8, box_y, W - 2 * (MARGIN_X + 8), box_h, COL_BAND);
  if (line1) draw_text_centered(&ui_font_20, box_y + 10, line1, COL_INFO);
  if (line2) draw_text_centered(&ui_font_14, box_y + 38, line2, COL_TEXT);
  return true;
}

bool tv_menu_submit() {
  if (!s_fb || !video_link_is_ready()) return false;
  // Image dessinee en RGB332 -> palette "RGB332" (protocole v3).
  return video_link_submit_frame(s_lines, video_link_rgb332_palette(), 300);   // ecran fixe : tout renvoye 3 fois par seconde
}

// Voir tv_menu.h
uint8_t *const *tv_menu_lines() {
  return s_fb ? s_lines : NULL;
}

int tv_menu_row_at(int x, int y) {
  if (x < 0 || x >= W || y < LIST_Y || y >= LIST_Y + TVM_VISIBLE_ROWS * ROW_H) return -1;
  const int row = s_top + (y - LIST_Y) / ROW_H;
  return (row >= 0 && row < s_drawn_count) ? row : -1;
}
