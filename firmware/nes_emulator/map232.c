/* map232.c - Camerica BF9096 (cartouches "Quattro" : 4 jeux)
 *   $8000-$BFFF : bits 3-4 = bloc de 64 Ko ;
 *   $C000-$FFFF : bits 0-1 = banque de 16 Ko dans le bloc ($8000) ;
 *   $C000 : derniere banque du bloc.
 */
#include "noftypes.h"
#include "nes_mmc.h"
#include "log.h"
#include "savestate.h"

static int block, page;

static void map232_sync(void)
{
   mmc_bankrom(16, 0x8000, (block << 2) | page);
   mmc_bankrom(16, 0xC000, (block << 2) | 3);
}

static void map232_write(uint32 address, uint8 value)
{
   if (address < 0xC000)
      block = (value >> 3) & 3;
   else
      page = value & 3;
   map232_sync();
}

static void map232_init(void)
{
   /* Etat pour la sauvegarde instantanee (savestate.h) */
   ss_var(&block, sizeof(block));
   ss_var(&page, sizeof(page));

   block = 0;
   page = 0;
   map232_sync();
}

static map_memwrite map232_memwrite[] =
    {
        {0x8000, 0xFFFF, map232_write},
        {-1, -1, NULL}};

mapintf_t map232_intf =
    {
        232,             /* mapper number */
        "Camerica BF9096", /* mapper name */
        map232_init,     /* init routine */
        NULL,            /* vblank callback */
        NULL,            /* hblank callback */
        NULL,            /* get state (snss) */
        NULL,            /* set state (snss) */
        NULL,            /* memory read structure */
        map232_memwrite, /* memory write structure */
        NULL             /* external sound device */
};
