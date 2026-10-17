/* map069.c - Sunsoft FME-7 (Mr. Gimmick, Batman - Return of the Joker,
 * Hebereke...).
 *   $8000 : numero de commande (0-15) ; $A000 : valeur.
 *   0-7 : banques graphiques de 1 Ko ; 8 : $6000 (bit 6 : RAM, sinon ROM) ;
 *   9-B : banques de 8 Ko en $8000, $A000, $C000 ($E000 : derniere, fixe) ;
 *   C : miroirs ; D : interruptions (bit 0 : permises, bit 7 : compteur en
 *   marche ; toute ecriture acquitte) ; E/F : compteur 16 bits.
 *   Le compteur diminue a chaque cycle du processeur : ici par ligne (113
 *   ou 114 cycles), l'interruption part a la ligne ou il passe sous 0.
 *   Son Sunsoft 5B (Gimmick! japonais seulement) : non fait.
 */
#include "noftypes.h"
#include "nes_mmc.h"
#include "nes_ppu.h"
#include "nes.h"
#include "nes6502.h"
#include "log.h"
#include "savestate.h"

static uint8 command;
static uint8 irq_ctrl;
static int irq_counter;
static int cycle_frac;   /* fraction de cycle (1/3) par ligne */

static void set_cpu_pages(uint32 address, uint8 *p, int pages)
{
   nes6502_context ctx;
   int i, page = address >> 12;
   nes6502_getcontext(&ctx);
   for (i = 0; i < pages; i++)
      ctx.mem_page[page + i] = p + (i << 12);
   nes6502_setcontext(&ctx);
}

static void map69_write(uint32 address, uint8 value)
{
   if (address < 0xA000)
   {
      command = value & 0x0F;
      return;
   }
   if (address >= 0xC000)
      return;   /* $C000/$E000 : son 5B, non fait */

   switch (command)
   {
   case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7:
      mmc_bankvrom(1, command << 10, value);
      break;
   case 8:
      if (value & 0x40)
      {
         if (mmc_getinfo()->sram)
            set_cpu_pages(0x6000, mmc_getinfo()->sram, 2);
      }
      else
      {
         /* ROM en $6000 : meme calcul que mmc_bankrom(8, ...) */
         const int banks = mmc_getinfo()->rom_banks * 2;
         uint8 *p = mmc_getinfo()->rom + (((value & 0x3F) % banks) << 13);
         set_cpu_pages(0x6000, p, 2);
      }
      break;
   case 9:  mmc_bankrom(8, 0x8000, value & 0x3F); break;
   case 10: mmc_bankrom(8, 0xA000, value & 0x3F); break;
   case 11: mmc_bankrom(8, 0xC000, value & 0x3F); break;
   case 12:
      switch (value & 3)
      {
      case 0: ppu_mirror(0, 1, 0, 1); break;   /* vertical */
      case 1: ppu_mirror(0, 0, 1, 1); break;   /* horizontal */
      case 2: ppu_mirror(0, 0, 0, 0); break;
      default: ppu_mirror(1, 1, 1, 1); break;
      }
      break;
   case 13:
      irq_ctrl = value;
      nes6502_irq_clear();
      break;
   case 14: irq_counter = (irq_counter & 0xFF00) | value; break;
   case 15: irq_counter = (irq_counter & 0x00FF) | (value << 8); break;
   }
}

static void map69_hblank(int vblank)
{
   int cycles;
   UNUSED(vblank);
   if (!(irq_ctrl & 0x80))
      return;
   /* 113 2/3 cycles par ligne */
   cycles = 113;
   cycle_frac += 2;
   if (cycle_frac >= 3)
   {
      cycle_frac -= 3;
      cycles++;
   }
   irq_counter -= cycles;
   if (irq_counter < 0)
   {
      irq_counter &= 0xFFFF;
      if (irq_ctrl & 0x01)
         nes_irq();
   }
}

static void map69_init(void)
{
   /* Etat pour la sauvegarde instantanee (savestate.h) */
   ss_var(&command, sizeof(command));
   ss_var(&irq_ctrl, sizeof(irq_ctrl));
   ss_var(&irq_counter, sizeof(irq_counter));
   ss_var(&cycle_frac, sizeof(cycle_frac));

   command = 0;
   irq_ctrl = 0;
   irq_counter = 0;
   cycle_frac = 0;
   mmc_bankrom(8, 0xE000, MMC_LASTBANK);
}

static map_memwrite map69_memwrite[] =
    {
        {0x8000, 0xFFFF, map69_write},
        {-1, -1, NULL}};

mapintf_t map69_intf =
    {
        69,             /* mapper number */
        "Sunsoft FME-7", /* mapper name */
        map69_init,     /* init routine */
        NULL,           /* vblank callback */
        map69_hblank,   /* hblank callback */
        NULL,           /* get state (snss) */
        NULL,           /* set state (snss) */
        NULL,           /* memory read structure */
        map69_memwrite, /* memory write structure */
        NULL            /* external sound device */
};
