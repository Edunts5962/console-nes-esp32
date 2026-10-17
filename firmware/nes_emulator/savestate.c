/* savestate.c - voir savestate.h. */
#include <stdlib.h>
#include <string.h>
#include "noftypes.h"
#include "nes.h"
#include "nes6502.h"
#include "nes_ppu.h"
#include "nes_apu.h"
#include "nes_mmc.h"
#include "nes_rom.h"
#include "memguard.h"
#include "log.h"
#include "savestate.h"
#include "savestate_int.h"

#define SS_MAGIC     0x5353454Eu   /* "NESS" */
#define SS_VERSION   1
#define NES_RAMSIZE  0x800   /* nes.c */

/* ---------------------------------------------------------------------
   Liste tenue par le mapper du jeu en cours (refaite a chaque init)
   --------------------------------------------------------------------- */
#define SS_MAX_VARS     24
#define SS_MAX_REGIONS  4

static struct { void *addr; size_t size; } s_vars[SS_MAX_VARS];
static uint8 **s_ptrs[SS_MAX_VARS];
static struct { uint8 *base; uint32 size; bool content; } s_mregions[SS_MAX_REGIONS];
static int s_nvars, s_nptrs, s_nmregions;
static bool s_declared, s_overflow;
static void (*s_post_load)(void);
static uint8 *s_batt_base;
static uint32 s_batt_size;

void ss_registry_reset(void)
{
   s_nvars = s_nptrs = s_nmregions = 0;
   s_declared = s_overflow = false;
   s_post_load = NULL;
   s_batt_base = NULL;
   s_batt_size = 0;
}

void ss_mapper_stateless(void) { s_declared = true; }

void ss_var(void *addr, size_t size)
{
   s_declared = true;
   if (s_nvars >= SS_MAX_VARS) { s_overflow = true; return; }
   s_vars[s_nvars].addr = addr;
   s_vars[s_nvars].size = size;
   s_nvars++;
}

void ss_ptr(uint8 **pp)
{
   s_declared = true;
   if (s_nptrs >= SS_MAX_VARS) { s_overflow = true; return; }
   s_ptrs[s_nptrs++] = pp;
}

void ss_region(uint8 *base, uint32 size, bool save_content)
{
   if (!base || !size) return;
   if (s_nmregions >= SS_MAX_REGIONS) { s_overflow = true; return; }
   s_mregions[s_nmregions].base = base;
   s_mregions[s_nmregions].size = size;
   s_mregions[s_nmregions].content = save_content;
   s_nmregions++;
}

void ss_post_load(void (*fn)(void)) { s_post_load = fn; }

void ss_battery_region(uint8 *base, uint32 size)
{
   s_batt_base = base;
   s_batt_size = size;
}

/* ---------------------------------------------------------------------
   Zones connues : un pointeur s'ecrit (zone, decalage)
   --------------------------------------------------------------------- */
enum { Z_NULL = 0, Z_RAM, Z_ROM, Z_VROM, Z_SRAM, Z_VRAM, Z_NAMETAB, Z_NULLPAGE, Z_MAPPER = 16 };
#define SS_MAX_ZONES (Z_MAPPER + SS_MAX_REGIONS)
static uint8 *z_base[SS_MAX_ZONES];
static uint32 z_size[SS_MAX_ZONES];

static void zones_build(void)
{
   nes_t *nes = nes_getcontextptr();
   rominfo_t *ri = nes->rominfo;
   nes6502_context ctx;
   int i;
   memset(z_base, 0, sizeof(z_base));
   memset(z_size, 0, sizeof(z_size));
   nes6502_getcontext(&ctx);
   z_base[Z_RAM] = ctx.mem_page[0];         z_size[Z_RAM] = NES_RAMSIZE;
   z_base[Z_ROM] = ri->rom;                 z_size[Z_ROM] = (uint32)ri->rom_banks * 0x4000;
   z_base[Z_VROM] = ri->vrom;               z_size[Z_VROM] = ri->vrom ? (uint32)ri->vrom_banks * 0x2000 : 0;
   z_base[Z_SRAM] = ri->sram;               z_size[Z_SRAM] = ri->sram ? (uint32)ri->sram_banks * 0x400 : 0;
   z_base[Z_VRAM] = ri->vram;               z_size[Z_VRAM] = ri->vram ? 0x2000 : 0;
   z_base[Z_NAMETAB] = ppu_ss_nametab();    z_size[Z_NAMETAB] = 0x1000;
   z_base[Z_NULLPAGE] = nes6502_nullpage(); z_size[Z_NULLPAGE] = NES6502_BANKSIZE;
   for (i = 0; i < s_nmregions; i++)
   {
      z_base[Z_MAPPER + i] = s_mregions[i].base;
      z_size[Z_MAPPER + i] = s_mregions[i].size;
   }
}

/* ---------------------------------------------------------------------
   Ecriture / lecture
   --------------------------------------------------------------------- */
void ssw_bytes(ss_w *w, const void *p, size_t n)
{
   if (w->err) return;
   if (w->len + n > w->cap)
   {
      size_t cap = w->cap ? w->cap : 16384;
      uint8 *nb;
      while (cap < w->len + n) cap *= 2;
      nb = (uint8 *)nes_mem_malloc(cap);
      if (!nb) { w->err = -1; return; }
      if (w->buf) { memcpy(nb, w->buf, w->len); nes_mem_free(w->buf); }
      w->buf = nb;
      w->cap = cap;
   }
   memcpy(w->buf + w->len, p, n);
   w->len += n;
}

void ssw_u8(ss_w *w, uint8 v) { ssw_bytes(w, &v, 1); }

void ssw_u32(ss_w *w, uint32 v)
{
   uint8 b[4] = {(uint8)v, (uint8)(v >> 8), (uint8)(v >> 16), (uint8)(v >> 24)};
   ssw_bytes(w, b, 4);
}

void ssw_ptr(ss_w *w, const uint8 *p)
{
   int z;
   if (NULL == p) { ssw_u8(w, Z_NULL); ssw_u32(w, 0); return; }
   for (z = 1; z < SS_MAX_ZONES; z++)
   {
      if (z_base[z] && p >= z_base[z] && p < z_base[z] + z_size[z])
      {
         ssw_u8(w, (uint8)z);
         ssw_u32(w, (uint32)(p - z_base[z]));
         return;
      }
   }
   w->err = -2;   /* pointeur hors des zones connues : etat impossible */
}

void ssr_bytes(ss_r *r, void *p, size_t n)
{
   if (r->err) { if (p) memset(p, 0, n); return; }
   if (r->pos + n > r->len) { r->err = -3; if (p) memset(p, 0, n); return; }
   if (p) memcpy(p, r->buf + r->pos, n);
   r->pos += n;
}

uint8 ssr_u8(ss_r *r) { uint8 v = 0; ssr_bytes(r, &v, 1); return v; }

uint32 ssr_u32(ss_r *r)
{
   uint8 b[4];
   ssr_bytes(r, b, 4);
   return (uint32)b[0] | ((uint32)b[1] << 8) | ((uint32)b[2] << 16) | ((uint32)b[3] << 24);
}

uint8 *ssr_ptr(ss_r *r)
{
   const uint8 z = ssr_u8(r);
   const uint32 off = ssr_u32(r);
   if (r->err || Z_NULL == z) return NULL;
   if (z >= SS_MAX_ZONES || !z_base[z] || off >= z_size[z]) { r->err = -4; return NULL; }
   return z_base[z] + off;
}

/* ---------------------------------------------------------------------
   Empreinte de la ROM (CRC-32), signature de la version du format
   --------------------------------------------------------------------- */
static uint32 s_crc_cache;
static const uint8 *s_crc_rom;

static uint32 crc32_buf(uint32 crc, const uint8 *p, size_t n)
{
   crc = ~crc;
   while (n--)
   {
      int k;
      crc ^= *p++;
      for (k = 0; k < 8; k++)
         crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
   }
   return ~crc;
}

uint32 ss_rom_crc(void)
{
   rominfo_t *ri = nes_getcontextptr()->rominfo;
   if (!ri || !ri->rom) return 0;
   if (s_crc_rom != ri->rom)
   {
      s_crc_cache = crc32_buf(0, ri->rom, (size_t)ri->rom_banks * 0x4000);
      if (ri->vrom)
         s_crc_cache = crc32_buf(s_crc_cache, ri->vrom, (size_t)ri->vrom_banks * 0x2000);
      s_crc_rom = ri->rom;
   }
   return s_crc_cache;
}

/* tailles des structures ecrites telles quelles : un etat d'une autre
   version du programme (structures differentes) est refuse */
static uint32 layout_sig(void)
{
   return (uint32)(sizeof(rectangle_t) * 131 + sizeof(triangle_t) * 137 + sizeof(noise_t) * 139 +
                   sizeof(dmc_t) * 149 + sizeof(nes6502_context) * 151);
}

/* ---------------------------------------------------------------------
   API
   --------------------------------------------------------------------- */
bool ss_supported(void)
{
   nes_t *nes = nes_getcontextptr();
   return nes && nes->rominfo && s_declared && !s_overflow;
}

uint8 *ss_battery_ram(uint32 *size)
{
   nes_t *nes = nes_getcontextptr();
   if (s_batt_base) { if (size) *size = s_batt_size; return s_batt_base; }
   if (nes && nes->rominfo && (nes->rominfo->flags & ROM_FLAG_BATTERY) && nes->rominfo->sram)
   {
      if (size) *size = (uint32)nes->rominfo->sram_banks * 0x400;
      return nes->rominfo->sram;
   }
   if (size) *size = 0;
   return NULL;
}

void ss_free(uint8 *buf)
{
   if (buf) nes_mem_free(buf);
}

uint8 *ss_save(size_t *len)
{
   nes_t *nes = nes_getcontextptr();
   rominfo_t *ri;
   nes6502_context ctx;
   ss_w w = {NULL, 0, 0, 0};
   int i;

   if (!ss_supported()) return NULL;
   ri = nes->rominfo;
   zones_build();

   ssw_u32(&w, SS_MAGIC);
   ssw_u32(&w, SS_VERSION);
   ssw_u32(&w, layout_sig());
   ssw_u32(&w, (uint32)ri->mapper_number);
   ssw_u32(&w, ss_rom_crc());
   ssw_u32(&w, 0);   /* longueur totale, remplie a la fin */

   /* processeur */
   nes6502_getcontext(&ctx);
   ssw_u32(&w, ctx.pc_reg);
   ssw_u8(&w, ctx.a_reg); ssw_u8(&w, ctx.p_reg); ssw_u8(&w, ctx.x_reg);
   ssw_u8(&w, ctx.y_reg); ssw_u8(&w, ctx.s_reg); ssw_u8(&w, ctx.jammed);
   ssw_u8(&w, ctx.int_pending); ssw_u8(&w, ctx.int_latency);
   ssw_u32(&w, (uint32)ctx.total_cycles); ssw_u32(&w, (uint32)ctx.burn_cycles);
   for (i = 0; i < NES6502_NUMBANKS; i++)
      ssw_ptr(&w, ctx.mem_page[i]);
   ssw_bytes(&w, ctx.mem_page[0], NES_RAMSIZE);

   /* machine (nes.c) */
   ssw_u8(&w, nes->fiq_occurred); ssw_u8(&w, nes->fiq_state);
   ssw_u32(&w, (uint32)nes->fiq_cycles); ssw_u32(&w, (uint32)nes->scanline);
   ssw_bytes(&w, &nes->scanline_cycles, sizeof(float));
   {
      int rc[4];
      input_ss_get(rc);
      for (i = 0; i < 4; i++) ssw_u32(&w, (uint32)rc[i]);
   }

   ppu_ss_save(&w);
   apu_ss_save(&w);

   /* memoires de la cartouche */
   ssw_u32(&w, z_size[Z_SRAM]);
   if (z_size[Z_SRAM]) ssw_bytes(&w, ri->sram, z_size[Z_SRAM]);
   ssw_u32(&w, z_size[Z_VRAM]);
   if (z_size[Z_VRAM]) ssw_bytes(&w, ri->vram, z_size[Z_VRAM]);

   /* mapper */
   ssw_u32(&w, (uint32)s_nmregions);
   for (i = 0; i < s_nmregions; i++)
   {
      ssw_u32(&w, s_mregions[i].size);
      ssw_u8(&w, s_mregions[i].content);
      if (s_mregions[i].content) ssw_bytes(&w, s_mregions[i].base, s_mregions[i].size);
   }
   ssw_u32(&w, (uint32)s_nvars);
   for (i = 0; i < s_nvars; i++)
   {
      ssw_u32(&w, (uint32)s_vars[i].size);
      ssw_bytes(&w, s_vars[i].addr, s_vars[i].size);
   }
   ssw_u32(&w, (uint32)s_nptrs);
   for (i = 0; i < s_nptrs; i++)
      ssw_ptr(&w, *s_ptrs[i]);
   ssw_u32(&w, SS_MAGIC);   /* fin */

   if (w.err)
   {
      log_printf("savestate : etat impossible (%d)\n", w.err);
      ss_free(w.buf);
      return NULL;
   }
   w.buf[20] = (uint8)w.len; w.buf[21] = (uint8)(w.len >> 8);
   w.buf[22] = (uint8)(w.len >> 16); w.buf[23] = (uint8)(w.len >> 24);
   if (len) *len = w.len;
   return w.buf;
}

/* Lecture en deux passes : la premiere verifie tout sans rien changer, la
   seconde applique. */
static int load_pass(const uint8 *buf, size_t len, bool apply)
{
   nes_t *nes = nes_getcontextptr();
   rominfo_t *ri = nes->rominfo;
   ss_r r = {buf, len, 0, 0, apply ? 0 : 1};
   nes6502_context ctx;
   uint8 *pages[NES6502_NUMBANKS];
   uint8 ram[NES_RAMSIZE];
   uint32 n;
   int i;

   if (ssr_u32(&r) != SS_MAGIC) return -10;
   if (ssr_u32(&r) != SS_VERSION) return -11;
   if (ssr_u32(&r) != layout_sig()) return -11;
   if (ssr_u32(&r) != (uint32)ri->mapper_number) return -12;
   if (ssr_u32(&r) != ss_rom_crc()) return -12;
   if (ssr_u32(&r) != len) return -13;

   nes6502_getcontext(&ctx);
   ctx.pc_reg = ssr_u32(&r);
   ctx.a_reg = ssr_u8(&r); ctx.p_reg = ssr_u8(&r); ctx.x_reg = ssr_u8(&r);
   ctx.y_reg = ssr_u8(&r); ctx.s_reg = ssr_u8(&r); ctx.jammed = ssr_u8(&r);
   ctx.int_pending = ssr_u8(&r); ctx.int_latency = ssr_u8(&r);
   ctx.total_cycles = (int32)ssr_u32(&r); ctx.burn_cycles = (int32)ssr_u32(&r);
   for (i = 0; i < NES6502_NUMBANKS; i++)
      pages[i] = ssr_ptr(&r);
   ssr_bytes(&r, ram, NES_RAMSIZE);
   if (r.err) return -14;
   if (apply)
   {
      memcpy(ctx.mem_page[0], ram, NES_RAMSIZE);
   }

   {
      uint8 fo = ssr_u8(&r), fs = ssr_u8(&r);
      int fc = (int)ssr_u32(&r), sl = (int)ssr_u32(&r);
      float sc;
      int rc[4], k;
      ssr_bytes(&r, &sc, sizeof(float));
      for (k = 0; k < 4; k++) rc[k] = (int)ssr_u32(&r);
      if (apply)
      {
         nes->fiq_occurred = fo; nes->fiq_state = fs; nes->fiq_cycles = fc;
         nes->scanline = sl; nes->scanline_cycles = sc;
         input_ss_set(rc);
      }
   }

   ppu_ss_load(&r);
   apu_ss_load(&r);
   if (r.err) return -14;

   n = ssr_u32(&r);
   if (n != z_size[Z_SRAM]) return -16;
   if (n) ssr_bytes(&r, apply ? ri->sram : NULL, n);
   n = ssr_u32(&r);
   if (n != z_size[Z_VRAM]) return -16;
   if (n) ssr_bytes(&r, apply ? ri->vram : NULL, n);

   n = ssr_u32(&r);
   if (n != (uint32)s_nmregions) return -17;
   for (i = 0; i < s_nmregions; i++)
   {
      uint32 sz = ssr_u32(&r);
      uint8 c = ssr_u8(&r);
      if (sz != s_mregions[i].size || c != (uint8)s_mregions[i].content) return -17;
      if (c) ssr_bytes(&r, apply ? s_mregions[i].base : NULL, sz);
   }
   n = ssr_u32(&r);
   if (n != (uint32)s_nvars) return -17;
   for (i = 0; i < s_nvars; i++)
   {
      uint32 sz = ssr_u32(&r);
      if (sz != (uint32)s_vars[i].size) return -17;
      ssr_bytes(&r, apply ? s_vars[i].addr : NULL, sz);
   }
   n = ssr_u32(&r);
   if (n != (uint32)s_nptrs) return -17;
   for (i = 0; i < s_nptrs; i++)
   {
      uint8 *p = ssr_ptr(&r);
      if (apply) *s_ptrs[i] = p;
   }
   if (ssr_u32(&r) != SS_MAGIC || r.err) return -14;

   if (apply)
   {
      if (s_post_load) s_post_load();
      /* banques du processeur en dernier (le mapper a pu les recalculer :
         elles doivent rester celles de l'etat) */
      for (i = 0; i < NES6502_NUMBANKS; i++)
         ctx.mem_page[i] = pages[i];
      nes6502_setcontext(&ctx);
   }
   return 0;
}

int ss_load(const uint8 *buf, size_t len)
{
   int e;
   if (!ss_supported()) return -1;
   if (!buf || len < 32) return -13;
   zones_build();
   e = load_pass(buf, len, false);
   if (e) { log_printf("savestate : etat refuse (%d)\n", e); return e; }
   e = load_pass(buf, len, true);
   if (e) log_printf("savestate : ERREUR au chargement (%d)\n", e);
   return e;
}

const char *ss_error_text(int code)
{
   switch (code)
   {
   case 0: return "";
   case -1: return "Sauvegarde impossible pour ce jeu";
   case -11: return "Sauvegarde d'une autre version";
   case -12: return "Sauvegarde d'un autre jeu";
   default: return "Sauvegarde illisible";
   }
}
