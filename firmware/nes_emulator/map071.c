/* map071.c - Camerica / Codemasters (BF9093, BF9097) Jeux : Micro Machines,
 * Dizzy, Big Nose, Bee 52, Fire Hawk, MiG-29, Linus Spacehead, Stunt Kids...
 * (absents du nofrendo d'origine).
 *   $C000-$FFFF : banque de 16 Ko en $8000 ($C000 : derniere banque, fixe) ;
 *   $9000-$9FFF : (BF9097, Fire Hawk) bit 4 = ecran unique bas/haut.
 */
#include "noftypes.h"
#include "nes_mmc.h"
#include "nes_ppu.h"
#include "log.h"
#include "savestate.h"

static void map71_write(uint32 address, uint8 value)
{
   if (address >= 0xC000)
   {
      mmc_bankrom(16, 0x8000, value);
   }
   else if (address >= 0x9000 && address < 0xA000)
   {
      const int nt = (value >> 4) & 1;
      ppu_mirror(nt, nt, nt, nt);
   }
}

static void map71_init(void)
{
   ss_mapper_stateless();   /* Banques seulement (savestate.h) */

   mmc_bankrom(16, 0x8000, 0);
   mmc_bankrom(16, 0xC000, MMC_LASTBANK);
}

static map_memwrite map71_memwrite[] =
    {
        {0x8000, 0xFFFF, map71_write},
        {-1, -1, NULL}};

mapintf_t map71_intf =
    {
        71,             /* mapper number */
        "Camerica",     /* mapper name */
        map71_init,     /* init routine */
        NULL,           /* vblank callback */
        NULL,           /* hblank callback */
        NULL,           /* get state (snss) */
        NULL,           /* set state (snss) */
        NULL,           /* memory read structure */
        map71_memwrite, /* memory write structure */
        NULL            /* external sound device */
};
