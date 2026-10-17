// lcd_fullscreen.h
//
// Image du jeu en PLEIN ECRAN sur l'ecran LCD (320x240) au lieu de l'affichage
// habituel (256x240 au centre, entre les deux bandes tactiles de 32 px).
// Reglages (hw_config.h) : LCD_FULLSCREEN_MODE, LCD_FULLSCREEN_CROP_LINES,
// LCD_FULLSCREEN_EXIT_DELAY_MS.
//
// Mise a l'echelle "plus proche voisin" (pixels nets, pas de flou) :
//   - largeur : 256 -> 320 (x 1,25) : chaque groupe de 4 pixels NES donne
//     5 pixels a l'ecran (le 3e est double) ;
//   - hauteur : les LCD_FULLSCREEN_CROP_LINES lignes du haut et du bas sont
//     retirees (bord que les televiseurs cachaient deja), les lignes
//     restantes sont etirees sur 240 (8 -> 224 lignes sur 240 : une ligne
//     doublee sur 14). Resultat : les proportions d'une NES sur un
//     televiseur (pixels 7:6 au lieu de 8:7, environ 2 % d'ecart).
//
// Fonctions sans dependance a l'Arduino : le meme fichier est compile sur PC
// pour verifier la mise a l'echelle.
#ifndef LCD_FULLSCREEN_H
#define LCD_FULLSCREEN_H

#include <stdint.h>

#define LCD_FS_SRC_W 256   // largeur de l'image NES
#define LCD_FS_SRC_H 240   // hauteur de l'image NES
#define LCD_FS_DST_W 320   // ecran LCD (paysage)
#define LCD_FS_DST_H 240

// Remplit row_map[0..239] : ligne NES a lire pour chaque ligne de l'ecran
// (echantillon pris au centre de chaque ligne de l'ecran).
static inline void lcd_fs_build_row_map(uint8_t *row_map, int crop_lines) {
  if (crop_lines < 0) crop_lines = 0;
  if (crop_lines > 16) crop_lines = 16;
  const int src_h = LCD_FS_SRC_H - 2 * crop_lines;
  for (int y = 0; y < LCD_FS_DST_H; y++) {
    row_map[y] = (uint8_t)(crop_lines + ((2 * y + 1) * src_h) / (2 * LCD_FS_DST_H));
  }
}

// Une ligne : 256 pixels NES (index de palette) -> 320 pixels RGB565.
// Pixel ecran x = pixel NES (x + 0,5) x 0,8 arrondi en dessous, soit pour
// chaque groupe de 4 : s0 s1 s2 s2 s3.
static inline void lcd_fs_scale_line(const uint8_t *src, uint16_t *dst, const uint16_t *pal) {
  for (int i = 0; i < LCD_FS_SRC_W / 4; i++) {
    const uint16_t c2 = pal[src[2]];
    dst[0] = pal[src[0]];
    dst[1] = pal[src[1]];
    dst[2] = c2;
    dst[3] = c2;
    dst[4] = pal[src[3]];
    src += 4;
    dst += 5;
  }
}

#endif
