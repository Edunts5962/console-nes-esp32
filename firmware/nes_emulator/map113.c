/* map113.c - NINA-03/06 multi-jeux (HES, Sachen) : Rad Racket, Deathbots, HES
 * 6-in-1...
 *   ecriture en $4100-$5FFF (adresse & $4100 = $4100) :
 *   bits 3-5 = 32 Ko de programme ; bits 0-2 + bit 6 = 8 Ko graphiques ;
 *   bit 7 = miroirs verticaux.
 */
#include "noftypes.h"
#include "nes_mmc.h"
#include "nes_ppu.h"
#include "log.h"
#include "savestate.h"

static void map113_write(uint32 address, uint8 value)
{
   if ((address & 0x4100) != 0x4100)
      return;
   mmc_bankrom(32, 0x8000, (value >> 3) & 7);
   mmc_bankvrom(8, 0x0000, (value & 7) | ((value >> 3) & 8));
   if (value & 0x80)
      ppu_mirror(0, 1, 0, 1);
   else
      ppu_mirror(0, 0, 1, 1);
}

static void map113_init(void)
{
   ss_mapper_stateless();   /* Banques seulement (savestate.h) */

   mmc_bankrom(32, 0x8000, 0);
   mmc_bankvrom(8, 0x0000, 0);
}

static map_memwrite map113_memwrite[] =
    {
        {0x4100, 0x5FFF, map113_write},
        {-1, -1, NULL}};

mapintf_t map113_intf =
    {
        113,             /* mapper number */
        "NINA-03/06 multi", /* mapper name */
        map113_init,     /* init routine */
        NULL,            /* vblank callback */
        NULL,            /* hblank callback */
        NULL,            /* get state (snss) */
        NULL,            /* set state (snss) */
        NULL,            /* memory read structure */
        map113_memwrite, /* memory write structure */
        NULL             /* external sound device */
};
