/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc_item_unq.h"

#ifdef SH_PC_PORT

#include "bodyprog/items.h"
#include "main/fileinfo.h" /* e_FsFile / FILE_ITEM_UNQ*_TMD */
#include "bodyprog/bodyprog.h"

/* Mirror of the switch in GameFs_UniqueItemModelLoad
 * (src/bodyprog/items/item_screens_3.c). Keep both mappings synchronized.
 *
 * Table rather than a switch so the relationship stays easy to review and a
 * future mechanical cross-check can parse it directly.
 */
typedef struct
{
    u8  itemId;
    s32 fileIdx;
} s_PcItemUnqEntry;

static const s_PcItemUnqEntry s_PcItemUnqTable[] =
{
    { InvItemId_HealthDrink         , FILE_ITEM_UNQ21_TMD },
    { InvItemId_Ampoule             , FILE_ITEM_UNQ22_TMD },
    { InvItemId_HouseKey            , FILE_ITEM_UNQ41_TMD },
    { InvItemId_KeyOfLion           , FILE_ITEM_UNQ42_TMD },
    { InvItemId_KeyOfWoodman        , FILE_ITEM_UNQ43_TMD },
    { InvItemId_KeyOfScarecrow      , FILE_ITEM_UNQ44_TMD },
    { InvItemId_LibraryReserveKey   , FILE_ITEM_UNQ45_TMD },
    { InvItemId_ClassroomKey        , FILE_ITEM_UNQ46_TMD },
    { InvItemId_KGordonKey          , FILE_ITEM_UNQ47_TMD },
    { InvItemId_DrawbridgeKey       , FILE_ITEM_UNQ48_TMD },
    { InvItemId_BasementKey         , FILE_ITEM_UNQ49_TMD },
    { InvItemId_BasementStoreroomKey, FILE_ITEM_UNQ4A_TMD },
    { InvItemId_ExaminationRoomKey  , FILE_ITEM_UNQ4B_TMD },
    { InvItemId_AntiqueShopKey      , FILE_ITEM_UNQ4C_TMD },
    { InvItemId_SewerKey            , FILE_ITEM_UNQ4D_TMD },
    { InvItemId_SewerExitKey        , FILE_ITEM_UNQ4D_TMD },
    { InvItemId_KeyOfOphiel         , FILE_ITEM_UNQ4E_TMD },
    { InvItemId_KeyOfHagith         , FILE_ITEM_UNQ4F_TMD },
    { InvItemId_KeyOfPhaleg         , FILE_ITEM_UNQ50_TMD },
    { InvItemId_KeyOfBethor         , FILE_ITEM_UNQ51_TMD },
    { InvItemId_KeyOfAratron        , FILE_ITEM_UNQ52_TMD },
    { InvItemId_NoteToSchool        , FILE_ITEM_UNQ53_TMD },
    { InvItemId_NoteDoghouse        , FILE_ITEM_UNQ54_TMD },
    { InvItemId_PictureCard         , FILE_ITEM_UNQ55_TMD },
    { InvItemId_ChannelingStone     , FILE_ITEM_UNQ56_TMD },
    { InvItemId_Chemical            , FILE_ITEM_UNQ60_TMD },
    { InvItemId_GoldMedallion       , FILE_ITEM_UNQ61_TMD },
    { InvItemId_SilverMedallion     , FILE_ITEM_UNQ62_TMD },
    { InvItemId_RubberBall          , FILE_ITEM_UNQ63_TMD },
    { InvItemId_Flauros             , FILE_ITEM_UNQ64_TMD },
    { InvItemId_PlasticBottle       , FILE_ITEM_UNQ65_TMD },
    { InvItemId_UnknownLiquid       , FILE_ITEM_UNQ66_TMD },
    { InvItemId_PlateOfHatter       , FILE_ITEM_UNQ67_TMD },
    { InvItemId_PlateOfCat          , FILE_ITEM_UNQ68_TMD },
    { InvItemId_PlateOfQueen        , FILE_ITEM_UNQ69_TMD },
    { InvItemId_PlateOfTurtle       , FILE_ITEM_UNQ6A_TMD },
    { InvItemId_BloodPack           , FILE_ITEM_UNQ6B_TMD },
    { InvItemId_DisinfectingAlcohol , FILE_ITEM_UNQ6C_TMD },
    { InvItemId_Lighter             , FILE_ITEM_UNQ6D_TMD },
    { InvItemId_VideoTape           , FILE_ITEM_UNQ6E_TMD },
    { InvItemId_KaufmannKey         , FILE_ITEM_UNQ70_TMD },
    { InvItemId_Receipt             , FILE_ITEM_UNQ71_TMD },
    { InvItemId_SafeKey             , FILE_ITEM_UNQ72_TMD },
    { InvItemId_Magnet              , FILE_ITEM_UNQ73_TMD },
    { InvItemId_MotorcycleKey       , FILE_ITEM_UNQ74_TMD },
    { InvItemId_BirdCageKey         , FILE_ITEM_UNQ75_TMD },
    { InvItemId_Pliers              , FILE_ITEM_UNQ76_TMD },
    { InvItemId_Screwdriver         , FILE_ITEM_UNQ77_TMD },
    { InvItemId_Camera              , FILE_ITEM_UNQ78_TMD },
    { InvItemId_RingOfContract      , FILE_ITEM_UNQ79_TMD },
    { InvItemId_StoneOfTime         , FILE_ITEM_UNQ7A_TMD },
    { InvItemId_AmuletOfSolomon     , FILE_ITEM_UNQ7B_TMD },
    { InvItemId_CrestOfMercury      , FILE_ITEM_UNQ7C_TMD },
    { InvItemId_Ankh                , FILE_ITEM_UNQ7D_TMD },
    { InvItemId_DaggerOfMelchior    , FILE_ITEM_UNQ7E_TMD },
    { InvItemId_DiskOfOuroboros     , FILE_ITEM_UNQ7F_TMD },
    { InvItemId_KitchenKnife        , FILE_ITEM_UNQ80_TMD },
    { InvItemId_SteelPipe           , FILE_ITEM_UNQ81_TMD },
    { InvItemId_Hammer              , FILE_ITEM_UNQ82_TMD },
    { InvItemId_Chainsaw            , FILE_ITEM_UNQ83_TMD },
    { InvItemId_Axe                 , FILE_ITEM_UNQ84_TMD },
    { InvItemId_RockDrill           , FILE_ITEM_UNQ85_TMD },
    { InvItemId_Katana              , FILE_ITEM_UNQ86_TMD },
    { InvItemId_Handgun             , FILE_ITEM_UNQA0_TMD },
    { InvItemId_HuntingRifle        , FILE_ITEM_UNQA1_TMD },
    { InvItemId_Shotgun             , FILE_ITEM_UNQA2_TMD },
    { InvItemId_HyperBlaster        , FILE_ITEM_UNQA3_TMD },
    { InvItemId_HandgunBullets      , FILE_ITEM_UNQC0_TMD },
    { InvItemId_RifleShells         , FILE_ITEM_UNQC1_TMD },
    { InvItemId_ShotgunShells       , FILE_ITEM_UNQC2_TMD },
    { InvItemId_Flashlight          , FILE_ITEM_UNQE0_TMD },
    { InvItemId_PocketRadio         , FILE_ITEM_UNQE1_TMD },
    { InvItemId_GasolineTank        , FILE_ITEM_UNQE2_TMD },
};

s32 Pc_ItemUnq_FromItemId(u8 itemId)
{
    int i;

    for (i = 0; i < (int)(sizeof(s_PcItemUnqTable) / sizeof(s_PcItemUnqTable[0])); i++)
    {
        if (s_PcItemUnqTable[i].itemId == itemId)
        {
            return s_PcItemUnqTable[i].fileIdx;
        }
    }

    return FILE_ITEM_UNQ20_TMD;
}

/* See header. Unequipped doubles as "no latch held". */
static u8 s_PcItemUnqRequestedItemId = InvItemId_Unequipped;

void Pc_ItemUnq_SetRequestedItemId(u8 itemId)
{
    s_PcItemUnqRequestedItemId = itemId;
}

u8 Pc_ItemUnq_TakeRequestedItemId(s32 loadableItemIdx)
{
    u8 latched = s_PcItemUnqRequestedItemId;

    s_PcItemUnqRequestedItemId = InvItemId_Unequipped;

    if (latched != InvItemId_Unequipped)
    {
        return latched;
    }

    /* Retail paths: the caller already proved this equality before calling. */
    return g_MapOverlayHdr.loadableItems[loadableItemIdx];
}

#endif /* SH_PC_PORT */
