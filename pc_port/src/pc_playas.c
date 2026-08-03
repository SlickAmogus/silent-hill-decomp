/* Play as another character — groundwork for character selection.
 *
 * Route: retarget CHARA_FILE_INFOS[Chara_Harry].modelFileIdx/textureFileIdx
 * (NEVER animFileIdx) and rerun the vanilla Harry load. Everything downstream
 * follows the row live — the ILM/TIM reads, the material bake (which matches
 * materials by the TIM basename, so "LISA" binds automatically), loose-file
 * overrides, big-lm/v7 redirects and hires texture registration — while the
 * animation side (HB_BASE.ANM in FS_BUFFER_0, weapon/map keyframe banks,
 * playerBoneCoords, the upper/lower body split) never changes, so the skin is
 * driven by every Harry animation. Verified against the disc data: Cybil,
 * Kaufmann and Dahlia rigs are the same 18-bone skeleton class as Harry
 * (identical parent chain, kfSize 156, part->bone name scheme); Lisa adds 3
 * hair bones (18..20) handled as rigid head-followers below.
 *
 * Why the player ILM needs a PC-owned buffer whenever the row is retargeted:
 * disc reads are sector-granular — every CHARA ILM read writes
 * ALIGN(fileSize, 2048) bytes, and for all humanoid ILMs that is 16 KiB.
 * HELD_ITEM_LM_BUFFER sits at HARRY_LM_BUFFER + 14592 (HERO.ILM's exact
 * size), so ANY swapped read through the slab would stomp the equipped
 * weapon's PLM window (and a later weapon load would stomp the body's tail).
 */

#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/chara/chara.h"
#include "bodyprog/math/fixed_point.h"
#include "main/fsqueue.h"
#include "main/fileinfo.h"

#include "pc_playas.h"
#include "pc_big_lm.h"
#include "pc_config.h"
#include "hires_override.h"
#include "sh_log.h"

typedef struct
{
    const char* name;  /* config value / console token (lowercase) */
    const char* label; /* display name for the amber panel / echo */
    u8          charaId;
    /* Embedded prop/variant meshes to hide so the skin looks like a bare
     * playable body: in-hand guns, bag, key, Flauros, duplicate hand
     * variants. NULL-terminated; exact 8-char ILM part names. */
    const char* hideParts[6];
    u8          hairBones; /* bones >17 posed as rigid head-followers (Lisa) */
} PcPlayAsChara;

static const PcPlayAsChara s_playable[] = {
    { "harry",    "HARRY",    Chara_Harry,    { NULL }, 0 },
    { "lisa",     "LISA",     Chara_Lisa,     { NULL }, 3 },
    { "cybil",    "CYBIL",    Chara_Cybil,    { "06LGUN", "10RGUN", NULL }, 0 },
    { "kaufmann", "KAUFMANN", Chara_Kaufmann, { "06LHAND2", "06LBAG", "10RHAND2", "10RGUN", "10RAGLA", NULL }, 0 },
    { "dahlia",   "DAHLIA",   Chara_Dahlia,   { "10RHAND2", "10RKEY", "10FLAURO", NULL }, 0 },
};
#define PLAYAS_COUNT ((int)(sizeof(s_playable) / sizeof(s_playable[0])))

static int s_current = 0;

/* PC-owned destination for any non-HERO player ILM. One buffer, sized for the
 * largest playable file, registered under the Chara_Harry key (the pool only
 * registers 2..43) so Pc_BigLm_DestCapacity lifts the loose byte-replace size
 * gate for it too. */
static u8*    s_playerIlmBuf;
static size_t s_playerIlmCap;

/* HERO.TIM registered once in virtual texture slot 256+Chara_Harry so the
 * HERO-textured weapon PLMs keep sampling Harry's sheet after the VRAM parcel
 * is overwritten by the skin's TIM. Slot 257 is free: the pool uses 256+id
 * for ids 2..43 only. */
static int           s_heroTimSlotQueued;
static s_FsImageDesc s_heroTimVirtualDesc;

static const PcPlayAsChara* PlayAs_Cur(void)
{
    return &s_playable[s_current];
}

int Pc_PlayAs_Count(void)
{
    return PLAYAS_COUNT;
}

int Pc_PlayAs_Current(void)
{
    return s_current;
}

const char* Pc_PlayAs_Label(int idx)
{
    if (idx < 0 || idx >= PLAYAS_COUNT)
    {
        return "?";
    }
    return s_playable[idx].label;
}

int Pc_PlayAs_SkinCharaId(void)
{
    return PlayAs_Cur()->charaId;
}

int Pc_PlayAs_SuppressHarryHandVariants(void)
{
    return PlayAs_Cur()->charaId != Chara_Harry;
}

static void PlayAs_RowApply(int idx)
{
    const PcPlayAsChara* p = &s_playable[idx];

    if (p->charaId == Chara_Harry)
    {
        CHARA_FILE_INFOS[Chara_Harry].modelFileIdx   = FILE_CHARA_HERO_ILM;
        CHARA_FILE_INFOS[Chara_Harry].textureFileIdx = FILE_CHARA_HERO_TIM;
    }
    else
    {
        CHARA_FILE_INFOS[Chara_Harry].modelFileIdx   = CHARA_FILE_INFOS[p->charaId].modelFileIdx;
        CHARA_FILE_INFOS[Chara_Harry].textureFileIdx = CHARA_FILE_INFOS[p->charaId].textureFileIdx;
    }
}

void* Pc_PlayAs_PlayerLmRedirect(int modelFileIdx, void* lmHdr)
{
    size_t need;
    s32    loose;

    if (modelFileIdx == FILE_CHARA_HERO_ILM)
    {
        return lmHdr;
    }

    /* Never pass through a fileIdx-keyed Pc_BigLm buffer here: an NPC load of
     * the same character redirects to the SAME buffer and its re-parse would
     * pull the live player model out from under harryModel's skeleton. The
     * player always gets this private buffer; a validated oversized/v7 loose
     * replacement freads into it via the capacity registration below. */
    need  = (size_t)Fs_GetFileSectorAlignedSize(modelFileIdx) + PC_BIGLM_TAIL_SLACK;
    loose = Pc_BigLm_LooseSize(modelFileIdx);
    if (loose > 0 && (size_t)loose + PC_BIGLM_TAIL_SLACK > need)
    {
        need = (size_t)loose + PC_BIGLM_TAIL_SLACK;
    }

    if (s_playerIlmBuf == NULL || s_playerIlmCap < need)
    {
        /* Drop the registration before the pointer dies (pool precedent:
         * a stale pointer would lift the size gate for whatever allocation
         * later recycles that address). */
        Pc_BigLm_RegisterExternal(Chara_Harry, NULL, 0);
        free(s_playerIlmBuf);
        s_playerIlmBuf = (u8*)calloc(1, need);
        s_playerIlmCap = (s_playerIlmBuf != NULL) ? need : 0;
        if (s_playerIlmBuf == NULL)
        {
            SH_DBG("[PLAYAS] calloc(%zu) failed — native path (held-item window at risk)", need);
            return lmHdr;
        }
    }
    else
    {
        /* Reused across skins: keep the tail beyond a smaller file zeroed. */
        memset(s_playerIlmBuf, 0, s_playerIlmCap);
    }

    /* Register the CURRENT file's extent, never the sticky buffer capacity —
     * an unvalidated loose file must not inherit a bigger byte-replace lift
     * than its own slot (mirrors PoolChara_Load). */
    Pc_BigLm_RegisterExternal(Chara_Harry, s_playerIlmBuf, need);
    return s_playerIlmBuf;
}

static int PlayAs_PartNameIs(const char* fieldStr, const char* want)
{
    int i;

    for (i = 0; i < 8 && want[i] != '\0'; i++)
    {
        if (fieldStr[i] != want[i])
        {
            return 0;
        }
    }
    for (; i < 8; i++)
    {
        if (fieldStr[i] != '\0' && fieldStr[i] != ' ')
        {
            return 0;
        }
    }
    return 1;
}

/* Full show/hide pass over the player skeleton from the skin's table —
 * self-healing, so it can re-run after anything scrambles visibility. */
void Pc_PlayAs_ApplySkinVisibility(void)
{
    const PcPlayAsChara* p     = PlayAs_Cur();
    s_CharaModel*        model = &g_WorldGfxWork.harryModel;
    s_Skeleton*          skel  = &model->skeleton;
    int                  i;
    int                  j;

    if (!model->isLoaded || model->lmHdr == NULL)
    {
        return;
    }

    for (i = 0; i < model->lmHdr->modelCount && i < 56; i++)
    {
        s_LinkedBone* bone = &skel->bones_8[i];
        int           hide = 0;

        if (bone->bone.modelInfo.modelHdr == NULL)
        {
            continue;
        }

        for (j = 0; p->hideParts[j] != NULL; j++)
        {
            if (PlayAs_PartNameIs(bone->bone.modelInfo.modelHdr->name.str, p->hideParts[j]))
            {
                hide = 1;
                break;
            }
        }

        if (hide)
        {
            bone->bone.modelInfo.field_0 |= 1 << 31;
        }
        else
        {
            bone->bone.modelInfo.field_0 &= ~(1 << 31);
        }
    }
}

/* Lisa's hair parts bind bones 18..20, which HB_BASE.ANM (18 bones) never
 * animates. Pose them as rigid children from LS.ANM's bind chain (verified
 * from the disc file: parents 2/2/19, translationInitial << scaleLog2(3)) —
 * they land in g_SysWork's unused spare coords directly after
 * playerBoneCoords[18], which the draw path indexes contiguously. */
static void PlayAs_HairInit(void)
{
    static const struct
    {
        u8  parent;
        s32 t[3];
    } HAIR_BIND[3] = {
        { 2,  { 0, 8, -8 } },
        { 2,  { -8, 0, 8 } },
        { 19, { 0, 16, 0 } },
    };

    int i;
    int r;
    int c;

    for (i = 0; i < 3; i++)
    {
        GsCOORDINATE2* coord  = &g_SysWork.unkCoords_E30[i];
        u8             parent = HAIR_BIND[i].parent;

        memset(coord, 0, sizeof(*coord));
        for (r = 0; r < 3; r++)
        {
            for (c = 0; c < 3; c++)
            {
                coord->coord.m[r][c] = (r == c) ? Q12_ANGLE(360.0f) : Q12_ANGLE(0.0f);
            }
        }
        coord->coord.t[0] = HAIR_BIND[i].t[0];
        coord->coord.t[1] = HAIR_BIND[i].t[1];
        coord->coord.t[2] = HAIR_BIND[i].t[2];
        coord->super      = (parent >= 18) ? &g_SysWork.unkCoords_E30[parent - 18]
                                           : &g_SysWork.playerBoneCoords[parent];
        coord->flg = 0;
    }
}

void Pc_PlayAs_PlayerAnimTick(void)
{
    int i;

    if (PlayAs_Cur()->hairBones == 0)
    {
        return;
    }

    /* super == NULL means a warm reset's SysWork_Clear wiped the coords. The
     * per-frame flg clear forces Vw_CoordHierarchyMatrixCompute to recompose
     * against the freshly-animated head — flg is its "already processed"
     * cache and nothing else resets it for bones no ANM touches. */
    if (g_SysWork.unkCoords_E30[0].super == NULL)
    {
        PlayAs_HairInit();
        return;
    }
    for (i = 0; i < 3; i++)
    {
        g_SysWork.unkCoords_E30[i].flg = 0;
    }
}

static void PlayAs_EnsureHeroTimSlot(void)
{
    s32 slotId = HIRES_POOL_CHARA_SLOT_BASE + Chara_Harry;

    s_heroTimVirtualDesc.tPage[0] = 0;
    s_heroTimVirtualDesc.tPage[1] = 28; /* GL override path keys off the clut word only */
    s_heroTimVirtualDesc.u        = 0;
    s_heroTimVirtualDesc.v        = 0;
    s_heroTimVirtualDesc.clutX    = (s16)((slotId % 64) * 16);
    s_heroTimVirtualDesc.clutY    = (s16)(HIRES_POOL_CLUT_ROW_BASE + (slotId / 64) * HIRES_POOL_MAX_ROWS);

    /* Re-registered on every player ProcessLoad rather than once-flagged: a
     * warm boot's Fs_QueueReset can drop a pending entry, and a lying flag
     * would strand the HERO-textured weapons on an empty slot forever. Reset
     * first so re-registration replaces the rows cleanly (pool precedent). */
    HiresOverride_CharaPoolSlotReset(slotId);
    Fs_QueueStartReadTim(FILE_CHARA_HERO_TIM, FS_BUFFER_1, &s_heroTimVirtualDesc);
    s_heroTimSlotQueued = 1;
}

void Pc_PlayAs_HeldItemImageDesc(void* imageDesc)
{
    if (PlayAs_Cur()->charaId == Chara_Harry || !s_heroTimSlotQueued)
    {
        return;
    }
    *(s_FsImageDesc*)imageDesc = s_heroTimVirtualDesc;
}

void Pc_PlayAs_OnPlayerModelLoaded(void)
{
    if (PlayAs_Cur()->charaId == Chara_Harry || !g_WorldGfxWork.harryModel.isLoaded)
    {
        return;
    }

    PlayAs_EnsureHeroTimSlot();
    Pc_PlayAs_ApplySkinVisibility();
    if (PlayAs_Cur()->hairBones != 0)
    {
        PlayAs_HairInit();
    }
}

int Pc_PlayAs_SetByIndex(int idx, int save)
{
    int booted;

    if (idx < 0 || idx >= PLAYAS_COUNT)
    {
        return 0;
    }

    booted = g_WorldGfxWork.registeredCharaModels[Chara_Harry] != NULL;

    if (idx != s_current || !booted)
    {
        s_current = idx;
        PlayAs_RowApply(idx);

        if (booted)
        {
            /* Synchronous reload, mirroring PoolChara_Load: the player draw
             * has no isLoaded gate, so the buffer must never be mid-read when
             * a frame renders. Free the held item first — the swap-back read
             * into the PSX slab writes through its window. */
            s32 pump        = 0;
            s32 savedItemId = g_WorldGfxWork.heldItem.itemId;
            s32 weaponAttack;

            Item_HeldItemModelFree();
            WorldGfx_HarryCharaLoad();
            while (Fs_QueueGetLength() > 0 && pump++ < 100000)
            {
                Fs_QueueUpdate();
            }
            WorldGfx_PlayerModelProcessLoad();

            /* Restore the exact held item, NOT WorldGfx_PlayerPrevHeldItem:
             * that derives the id from playerCombat.weaponAttack, which (a)
             * packs +10/+20 mid-melee-swing (garbage id -> weapon vanishes
             * until an inventory re-equip) and (b) knows nothing about
             * scripted cutscene props (phone/baby/blood pack), which are Set
             * directly by scene code and would be destroyed for the rest of
             * the scene. */
            WorldGfx_PlayerHeldItemSet(savedItemId);

            /* Tap-normalize the attach arg for the same +10/+20 reason; only
             * melee packs an input type (guns are ids 32+ and stay Tap). */
            weaponAttack = (u8)g_SysWork.playerCombat.weaponAttack;
            if (weaponAttack != (u8)NO_VALUE && weaponAttack < 30)
            {
                weaponAttack = WEAPON_ATTACK_ID_GET(weaponAttack);
            }
            Gfx_PlayerHeldItemAttach((u8)weaponAttack);

            /* Second drain: ProcessLoad queued the HERO.TIM virtual-slot
             * re-registration (its reset just freed the old GL rows the baked
             * weapon prims point at) and the restore queued the held PLM —
             * finish both before the next frame draws. */
            pump = 0;
            while (Fs_QueueGetLength() > 0 && pump++ < 100000)
            {
                Fs_QueueUpdate();
            }

            SH_DBG_ECHO("[PLAYAS] playing as %s", s_playable[idx].label);
        }
    }

    if (save)
    {
        PcConfig_SaveKeyValue("player_character", s_playable[idx].name);
    }
    return 1;
}

int Pc_PlayAs_SetByName(const char* name, int save)
{
    int i;
    int j;

    if (name == NULL || name[0] == '\0')
    {
        return 0;
    }

    for (i = 0; i < PLAYAS_COUNT; i++)
    {
        const char* want = s_playable[i].name;

        for (j = 0; ; j++)
        {
            char a = name[j];
            char b = want[j];

            if (a >= 'A' && a <= 'Z')
            {
                a += 'a' - 'A';
            }
            if (a != b)
            {
                break;
            }
            if (a == '\0')
            {
                return Pc_PlayAs_SetByIndex(i, save);
            }
        }
    }
    return 0;
}

int Pc_PlayAs_Cycle(int step)
{
    int idx = (s_current + step + PLAYAS_COUNT) % PLAYAS_COUNT;

    Pc_PlayAs_SetByIndex(idx, 1);
    return s_current;
}

void Pc_PlayAs_Init(void)
{
    if (g_PcConfig.playerCharacter[0] != '\0' &&
        !Pc_PlayAs_SetByName(g_PcConfig.playerCharacter, 0))
    {
        SH_DBG("[PLAYAS] unknown player_character '%s' — playing as Harry", g_PcConfig.playerCharacter);
    }
}
