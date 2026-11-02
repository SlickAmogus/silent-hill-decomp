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

void Pc_ItemUnq_FixLiquidUvs(u8 itemId, struct TMD_STRUCT* model)
{
    /* Four UNQ66 liquid triangles sample (128,255), outside the small
     * red patch used by the neighbouring faces. One is opaque and three are
     * translucent reverse faces. This is present in retail USA and JAP0 data,
     * and exposes grey triangles when the bottle rotates.
     * Match the original primitive, including its vertex indices, so custom
     * models and already-corrected data are left alone. CLUT/tpage may have
     * been relocated by the loader, so exclude those four bytes from matching. */
    static const int indices[] = { 1, 109, 118, 214 };
    static const u8 original[4][28] = {
        {9,6,0,52,255,170,0,0,128,255,0,0,128,255,0,0,2,0,3,0,3,0,4,0,3,0,5,0},
        {9,6,0,54,128,255,0,0,255,170,0,0,255,170,0,0,91,0,4,0,14,0,3,0,14,0,7,0},
        {9,6,0,54,255,170,0,0,255,170,0,0,128,255,0,0,2,0,7,0,2,0,3,0,3,0,5,0},
        {9,6,0,54,128,255,0,0,128,255,0,0,255,170,0,0,91,0,5,0,91,0,4,0,14,0,7,0}
    };
    static const u8 corrected[4][6] = {
        {255,191,247,191,247,168},
        {247,191,255,191,255,168},
        {255,168,255,191,247,168},
        {247,168,247,191,255,168}
    };
    /* Additional liquid faces share the shell's ABE mode. Treat them as the
     * same opaque liquid volume as faces 0..7, rather than blending several
     * overlapping red triangles. The plastic shell retains its transparency. */
    static const int liquidIndices[16] = {
        108,109,110,111,112,113,114,115,118,119,120,121,214,215,216,217
    };
    static const u8 liquidOriginal[16][28] = {
        {9,6,0,54,247,168,0,0,255,191,0,0,255,168,0,0,90,0,1,0,52,0,0,0,52,0,2,0},
        {9,6,0,54,128,255,0,0,255,170,0,0,255,170,0,0,91,0,4,0,14,0,3,0,14,0,7,0},
        {9,6,0,54,247,191,0,0,255,191,0,0,255,168,0,0,52,0,0,0,91,0,4,0,91,0,5,0},
        {9,6,0,54,247,168,0,0,255,191,0,0,255,168,0,0,92,0,3,0,92,0,6,0,92,0,7,0},
        {9,6,0,54,247,168,0,0,255,191,0,0,255,168,0,0,9,0,7,0,9,0,2,0,9,0,5,0},
        {9,6,0,54,247,168,0,0,255,191,0,0,255,168,0,0,93,0,0,0,93,0,3,0,94,0,4,0},
        {9,6,0,54,247,191,0,0,255,191,0,0,255,168,0,0,93,0,3,0,93,0,0,0,95,0,1,0},
        {9,6,0,54,247,191,0,0,255,191,0,0,255,168,0,0,9,0,2,0,9,0,7,0,9,0,6,0},
        {9,6,0,54,255,170,0,0,255,170,0,0,128,255,0,0,2,0,7,0,2,0,3,0,3,0,5,0},
        {9,6,0,54,255,168,0,0,255,191,0,0,247,168,0,0,3,0,5,0,3,0,4,0,0,0,2,0},
        {9,6,0,54,255,168,0,0,255,191,0,0,247,168,0,0,8,0,1,0,6,0,0,0,8,0,6,0},
        {9,6,0,54,255,168,0,0,255,191,0,0,247,168,0,0,5,0,6,0,5,0,7,0,5,0,1,0},
        {9,6,0,54,128,255,0,0,128,255,0,0,255,170,0,0,91,0,5,0,91,0,4,0,14,0,7,0},
        {9,6,0,54,247,168,0,0,247,191,0,0,255,168,0,0,52,0,2,0,52,0,0,0,91,0,5,0},
        {9,6,0,54,247,168,0,0,247,191,0,0,255,168,0,0,95,0,6,0,93,0,3,0,95,0,1,0},
        {9,6,0,54,247,168,0,0,247,191,0,0,255,168,0,0,9,0,1,0,9,0,2,0,9,0,6,0}
    };
    u8* prim;
    int i;
    int j;

    if (itemId != InvItemId_UnknownLiquid || model == NULL ||
        model->vern != 59 || model->primn != 220 || model->primtop == NULL)
        return;

    /* Validate the complete fixed-size layout before using primitive indices. */
    for (i = 0; i < 220; i++)
    {
        if (((u8*)model->primtop)[i * 28 + 1] != 6)
            return;
    }
    for (j = 0; j < 4; j++)
    {
        prim = (u8*)model->primtop + indices[j] * 28;
        for (i = 0; i < 28; i++)
        {
            if (i == 6 || i == 7 || i == 10 || i == 11)
                continue;
            if (prim[i] != original[j][i])
                return;
        }
    }
    for (j = 0; j < 16; j++)
    {
        prim = (u8*)model->primtop + liquidIndices[j] * 28;
        for (i = 0; i < 28; i++)
        {
            if (i == 6 || i == 7 || i == 10 || i == 11)
                continue;
            if (prim[i] != liquidOriginal[j][i])
                return;
        }
    }
    /* Validate all affected faces before making either correction. */
    for (j = 0; j < 16; j++)
        ((u8*)model->primtop)[liquidIndices[j] * 28 + 3] &= ~2;

    /* Keep the same orientation as neighbouring faces, including reverse sides. */
    for (j = 0; j < 4; j++)
    {
        prim = (u8*)model->primtop + indices[j] * 28;
        for (i = 0; i < 3; i++)
        {
            prim[4 + i * 4] = corrected[j][i * 2];
            prim[5 + i * 4] = corrected[j][i * 2 + 1];
        }
    }
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
