/* savestate_int.h - ecriture/lecture de l'etat, partagees par savestate.c et
 * les modules du coeur (nes_ppu.c, nes_apu.c) qui gardent leur etat en
 * variables statiques. Voir savestate.h.
 */
#ifndef _SAVESTATE_INT_H_
#define _SAVESTATE_INT_H_

#include <stddef.h>
#include "noftypes.h"

typedef struct
{
   uint8 *buf;
   size_t len, cap;
   int err;
} ss_w;

typedef struct
{
   const uint8 *buf;
   size_t len, pos;
   int err;
   int dry;   /* 1 : lire et verifier seulement, ne rien changer */
} ss_r;

void ssw_bytes(ss_w *w, const void *p, size_t n);
void ssw_u8(ss_w *w, uint8 v);
void ssw_u32(ss_w *w, uint32 v);
/* pointeur reel (non decale) -> (zone, decalage) ; NULL accepte */
void ssw_ptr(ss_w *w, const uint8 *p);

void ssr_bytes(ss_r *r, void *p, size_t n);
uint8 ssr_u8(ss_r *r);
uint32 ssr_u32(ss_r *r);
uint8 *ssr_ptr(ss_r *r);

/* nes_ppu.c / nes_apu.c */
void ppu_ss_save(ss_w *w);
void ppu_ss_load(ss_r *r);
uint8 *ppu_ss_nametab(void);
void apu_ss_save(ss_w *w);
void apu_ss_load(ss_r *r);
/* nesinput.c */
void input_ss_get(int v[4]);
void input_ss_set(const int v[4]);
/* nes6502.c */
uint8 *nes6502_nullpage(void);

#endif
