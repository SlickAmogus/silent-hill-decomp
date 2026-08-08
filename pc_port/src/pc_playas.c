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

/* Harry's rig is 18 bones (0..17); g_SysWork carries 5 spare coords right after
 * them, so a skin may bind parts up to bone 22 and no further. */
#define PLAYAS_BONE_MAX  18
#define PLAYAS_EXTRA_MAX 5

typedef struct
{
    const char* name;  /* config value / console token (lowercase) */
    const char* label; /* display name for the amber panel / echo */
    u8          charaId;
    /* Embedded prop/variant meshes to hide so the skin looks like a bare
     * playable body: in-hand guns, bag, key, Flauros, duplicate hand
     * variants. NULL-terminated; exact 8-char ILM part names. */
    const char* hideParts[6];
    u8          female; /* drives the optional voice-pitch shift (SFX only) */
} PcPlayAsChara;

/* Every rig here shares Harry's bone 0..17 parent chain and binds its parts to
 * bones <= 22, which is what makes Harry's animation drive it: the chain is
 * what the rotations mean, and 22 is the last bone the player's coord array can
 * address (18 bones + the 5 spares). Rigs that fail either test — every monster,
 * the Incubus boss, the cat, the dogs — are a different skeleton entirely and
 * cannot be posed by Harry's keyframes at all. */
static const PcPlayAsChara s_playable[] = {
    { "harry",       "HARRY",       Chara_Harry,      { NULL }, 0 },
    { "lisa",        "LISA",        Chara_Lisa,       { NULL }, 1 },
    { "cybil",       "CYBIL",       Chara_Cybil,      { "06LGUN", "10RGUN", NULL }, 1 },
    { "kaufmann",    "KAUFMANN",    Chara_Kaufmann,   { "06LHAND2", "06LBAG", "10RHAND2", "10RGUN", "10RAGLA", NULL }, 0 },
    { "dahlia",      "DAHLIA",      Chara_Dahlia,     { "10RHAND2", "10RKEY", "10FLAURO", NULL }, 1 },
    { "cheryl",      "CHERYL",      Chara_Cheryl,     { NULL }, 1 },
    { "alessa",      "ALESSA",      Chara_Alessa,     { NULL }, 1 },
    { "ghostalessa", "GHOST ALESSA", Chara_GhostChildAlessa, { NULL }, 1 },
    { "bloodylisa",  "BLOODY LISA", Chara_BloodyLisa, { NULL }, 1 },
    { "nurse",       "PUPPET NURSE", Chara_PuppetNurse, { "10RHAND2", NULL }, 1 },
    { "doctor",      "PUPPET DOCTOR", Chara_PuppetDoctor, { "10RHAND2", NULL }, 0 },
    { "ghostdoctor", "GHOST DOCTOR", Chara_GhostDoctor, { NULL }, 0 },
    { "monstercybil", "MONSTER CYBIL", Chara_MonsterCybil, { "06LGUN", "10RGUN", NULL }, 1 },
    /* The Incubator's outer sleeve segments bind bones 22-23; 23 is past the
     * last addressable coord, so both right-hand segments are hidden and the
     * left pair with them, keeping her symmetrical. */
    { "incubator",   "INCUBATOR",   Chara_Incubator,  { "20LSODE1", "21LSODE2", "22RSODE1", "23RSODE2", NULL }, 1 },
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
 * HERO-textured weapon PLMs keep sampling Harry's sheet while the skin owns the
 * player material. Still needed now that the skin bypasses VRAM entirely: on a
 * cold boot straight into a skin, HERO.TIM is never uploaded anywhere at all.
 * Slot 257 is free: the pool uses 256+id for ids 2..43 only. */
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

/* The token the config key and the console command accept (lowercase). */
const char* Pc_PlayAs_Name(int idx)
{
    if (idx < 0 || idx >= PLAYAS_COUNT)
    {
        return "?";
    }
    return s_playable[idx].name;
}

int Pc_PlayAs_SkinCharaId(void)
{
    return PlayAs_Cur()->charaId;
}

int Pc_PlayAs_IsFemale(void)
{
    return PlayAs_Cur()->female != 0;
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

        /* Safety net: a part bound past the last addressable coord would have
         * the drawer index off the end of the spares. Hidden parts are skipped
         * before that read, so this keeps any future rig non-corrupting. */
        {
            const char* nm = bone->bone.modelInfo.modelHdr->name.str;

            if (nm[0] >= '0' && nm[0] <= '9' && nm[1] >= '0' && nm[1] <= '9' &&
                ((nm[0] - '0') * 10 + (nm[1] - '0')) >= PLAYAS_BONE_MAX + PLAYAS_EXTRA_MAX)
            {
                hide = 1;
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

/* ---- skeleton retarget ---------------------------------------------------
 *
 * A skin's mesh parts are authored in BONE-LOCAL space against THEIR OWN
 * skeleton's bind translations, but they are posed on Harry's — so every part
 * renders at Harry's joint spacing instead of its own. That is the whole
 * source of the reported artifacts: Cybil's head bind sits 20 units BELOW
 * Harry's, so her neck geometry is pulled 20 units long; Kaufmann's sits 24
 * ABOVE, so his head/collar geometry telescopes down into the shoulders.
 *
 * The fix is standard animation retargeting — source ROTATIONS, target
 * OFFSETS — and the format hands it to us: a bone whose translationDataIdx is
 * -1 takes its local translation from the ANM bind table once, in
 * Anim_BoneInit, and Anim_BoneUpdate never writes it again (it only writes
 * translations for bones with a channel: slot 0, i.e. bones 0/1/11 on every
 * humanoid rig). Writing the SKIN's own values into those coords therefore
 * sticks, while Harry's keyframes keep driving every rotation.
 *
 * The legs then land somewhere else, so the character floats or sinks: the
 * hips still ride Harry's slot-0 root motion. That is corrected through the
 * ANM header's rootYOffset byte, which Anim_BoneUpdate subtracts from the Y of
 * every slot-0 sharer each frame — one byte, applied by the engine itself, so
 * no per-frame accumulation is possible. */
static s32 s_skinBindT[PLAYAS_BONE_MAX][3];
static u8  s_skinBindHas[PLAYAS_BONE_MAX];
static s16 s_skinBindFileIdx = (s16)NO_VALUE; /* the ANM these came from */
static int s_skinBindValid;
static int s_rootYPatched;
static u8  s_rootYOrig;

/* Harry's OWN offsets, latched the first time a skin is loaded. Swapping back
 * to Harry has to write these back: Anim_BoneUpdate never restores a static
 * translation and the swap path does not re-run Anim_BoneInit, so without this
 * Harry would keep wearing the skin's neck, shoulders and legs until the next
 * New Game / Continue / save load. */
static s32 s_harryBindT[PLAYAS_BONE_MAX][3];
static u8  s_harryBindHas[PLAYAS_BONE_MAX];
static int s_harryBindLatched;

/* Bones 18+ of the skin's own rig, seated in g_SysWork's five spare coords —
 * the last bone indices the player's coord array can address. The ROTATION
 * matters as much as the offset: the Puppet Nurse's parasite hangs off bone 18
 * with a ~90 degree twist baked into its authored pose, so seating it at
 * identity (which is all Lisa's hair ever needed) swung the whole chain down
 * her leg. Each extra bone therefore carries the skin's own keyframe-0
 * rotation, which is the pose the model was built around. */
static s32 s_extraT[PLAYAS_EXTRA_MAX][3];
static s32 s_extraR[PLAYAS_EXTRA_MAX][9];
static u8  s_extraParent[PLAYAS_EXTRA_MAX];
static int s_extraCount;

/* Bind-table reader for a raw ANM image: bone i at 0x14 + i*6 =
 * { s8 parentBone, s8 rotationDataIdx, s8 translationDataIdx, s8 t0[3] },
 * translations scaled by the header's scaleLog2 (offset 0x12). */
static void PlayAs_ReadBinds(const u8* anm, s32 outT[PLAYAS_BONE_MAX][3], u8 outHas[PLAYAS_BONE_MAX],
                             int* outBoneCount, u8* outRootY)
{
    int boneCount = anm[0x06];
    int scale     = anm[0x12];
    int i;

    if (boneCount > PLAYAS_BONE_MAX)
    {
        boneCount = PLAYAS_BONE_MAX;
    }
    memset(outHas, 0, PLAYAS_BONE_MAX);

    /* Bone 0 is never animated and never drawn against — start at 1. */
    for (i = 1; i < boneCount; i++)
    {
        const u8* b = anm + 0x14 + i * 6;

        if ((s8)b[2] >= 0)
        {
            continue; /* animated translation (slot 0) — Harry's root motion owns it */
        }
        outT[i][0] = (s32)(s8)b[3] << scale;
        outT[i][1] = (s32)(s8)b[4] << scale;
        outT[i][2] = (s32)(s8)b[5] << scale;
        outHas[i]  = 1;
    }

    *outBoneCount = boneCount;
    *outRootY     = anm[0x13];
}

/* Sum of the Y offsets down the left leg (hips -> thigh -> shin -> foot). The
 * difference against Harry's is exactly how far the skin's feet miss the floor
 * his root motion was authored for. */
static s32 PlayAs_LegDrop(const s32 t[PLAYAS_BONE_MAX][3], const u8 has[PLAYAS_BONE_MAX])
{
    s32 drop = 0;
    int bones[3] = { 12, 13, 14 }; /* LMOMO thigh, LSUNE shin, LFOOT foot */
    int i;

    for (i = 0; i < 3; i++)
    {
        if (has[bones[i]])
        {
            drop += t[bones[i]][1];
        }
    }
    return drop;
}

/* Load the skin's ANM far enough to read its bind table. Read straight through
 * the FS queue like the chara pool does (pumping it rather than waiting a
 * VSync per state), into a buffer freed before returning — only the 18 bind
 * entries are kept. */
/* A loaded HB_BASE.ANM, not some earlier tenant of FS_BUFFER_0 (BODYPROG.BIN and
 * a couple of TIMs pass through it during boot). dataOffset is 404 and the rig
 * is 18 bones in every retail region. */
static int PlayAs_HarryAnmReady(const u8* anm)
{
    return anm[0] == 0x94 && anm[1] == 0x01 && anm[0x06] == PLAYAS_BONE_MAX;
}

static void PlayAs_LoadSkinBinds(void)
{
    const PcPlayAsChara* p = PlayAs_Cur();
    u8*                  harryAnm = (u8*)FS_BUFFER_0;
    s32                  skinT[PLAYAS_BONE_MAX][3];
    u8                   skinHas[PLAYAS_BONE_MAX];
    s16                  animFileIdx;
    s32                  size;
    u8*                  buf;
    s32                  pump = 0;
    int                  boneCount;
    int                  harryBones;
    u8                   throwaway;
    int                  i;

    if (p->charaId == Chara_Harry)
    {
        return; /* the caller clears; nothing to load */
    }

    animFileIdx = CHARA_FILE_INFOS[p->charaId].animFileIdx;
    if (animFileIdx == (s16)NO_VALUE || animFileIdx == s_skinBindFileIdx)
    {
        return; /* nothing to load, or already loaded for this skin */
    }
    if (!PlayAs_HarryAnmReady(harryAnm))
    {
        SH_DBG("[PLAYAS] HB_BASE not resident yet — retarget deferred");
        return;
    }

    /* Harry's own offsets, latched once: the swap-back path writes these back,
     * and the leg-drop compensation is measured against them. */
    if (!s_harryBindLatched)
    {
        PlayAs_ReadBinds(harryAnm, s_harryBindT, s_harryBindHas, &harryBones, &s_rootYOrig);
        s_harryBindLatched = 1;
        s_rootYPatched     = 1;
    }

    size = Fs_GetFileSectorAlignedSize(animFileIdx);
    if (size <= 0x14 + PLAYAS_BONE_MAX * 6)
    {
        return;
    }
    /* calloc, not malloc: a dropped read or an exhausted pump budget must leave
     * a buffer that FAILS the header check below rather than one holding heap
     * garbage that parses as a plausible bind table. */
    buf = (u8*)calloc(1, (size_t)size);
    if (buf == NULL)
    {
        return;
    }

    Fs_QueueStartRead(animFileIdx, buf);
    while (Fs_QueueGetLength() > 0 && pump++ < 100000)
    {
        Fs_QueueUpdate();
    }

    boneCount    = 0;
    s_extraCount = 0;
    if (buf[0] == 0x94 && buf[1] == 0x01 && buf[0x12] <= 4 && buf[0x06] >= PLAYAS_BONE_MAX)
    {
        int raw   = buf[0x06];
        int scale = buf[0x12];
        int e;

        PlayAs_ReadBinds(buf, skinT, skinHas, &boneCount, &throwaway);

        /* PlayAs_ReadBinds caps at Harry's 18; the rig's own extra bones come
         * straight off the table here, with their authored keyframe-0
         * rotation (rotation channel r lives at dataOffset + transBC*3 + r*9,
         * nine s8 row-major coefficients, q12 = v << 5). */
        u32 dataOffset = (u32)buf[0] | ((u32)buf[1] << 8);
        int transBC    = buf[0x03];

        for (e = PLAYAS_BONE_MAX; e < raw && s_extraCount < PLAYAS_EXTRA_MAX; e++)
        {
            const u8* b   = buf + 0x14 + e * 6;
            int       rot = (s8)b[1];
            int       k;

            s_extraParent[s_extraCount] = (u8)b[0];
            s_extraT[s_extraCount][0]   = (s32)(s8)b[3] << scale;
            s_extraT[s_extraCount][1]   = (s32)(s8)b[4] << scale;
            s_extraT[s_extraCount][2]   = (s32)(s8)b[5] << scale;

            for (k = 0; k < 9; k++)
            {
                s_extraR[s_extraCount][k] = ((k % 4) == 0) ? Q12_ANGLE(360.0f) : Q12_ANGLE(0.0f);
            }
            if (rot >= 0)
            {
                u32 off = dataOffset + (u32)transBC * 3 + (u32)rot * 9;

                if (off + 9 <= (u32)size)
                {
                    for (k = 0; k < 9; k++)
                    {
                        s_extraR[s_extraCount][k] = (s32)(s8)buf[off + k] << 5;
                    }
                }
            }
            s_extraCount++;
        }
    }
    free(buf);

    if (boneCount < PLAYAS_BONE_MAX)
    {
        /* Unreadable, or a rig too short to cover Harry's bones — either way the
         * missing bones would keep Harry's offsets, which is the mismatch this
         * path exists to remove. */
        SH_DBG("[PLAYAS] %s ANM unusable for retarget (bones %d)", p->label, boneCount);
        return;
    }

    /* A bone is retargetable only when BOTH rigs treat it as static. The engine
     * decides from HARRY's header, so a bone he animates must keep its keyframe
     * translation whatever the skin says (identical on every retail rig today,
     * but the predicate should not depend on that). */
    for (i = 1; i < PLAYAS_BONE_MAX; i++)
    {
        s_skinBindHas[i] = (u8)(skinHas[i] && s_harryBindHas[i]);
        s_skinBindT[i][0] = skinT[i][0];
        s_skinBindT[i][1] = skinT[i][1];
        s_skinBindT[i][2] = skinT[i][2];
    }
    s_skinBindValid   = 1;
    s_skinBindFileIdx = animFileIdx;

    /* Foot-height compensation through the header byte the engine already
     * applies every frame to each slot-0 sharer. */
    {
        s32 drop  = PlayAs_LegDrop(s_harryBindT, s_harryBindHas) - PlayAs_LegDrop(s_skinBindT, s_skinBindHas);
        s32 rootY = (s32)s_rootYOrig - drop;

        if (rootY < 0)   rootY = 0;
        if (rootY > 255) rootY = 255;
        harryAnm[0x13] = (u8)rootY;
        SH_DBG("[PLAYAS] %s retarget: leg drop %+d, rootYOffset %d -> %d",
               p->label, (int)drop, (int)s_rootYOrig, (int)rootY);
    }
}

/* Put Harry's own offsets and root height back. Restoring the header byte is not
 * enough: nothing in the engine ever rewrites a static translation, and the swap
 * path does not run Anim_BoneInit — so Harry has to be given his own bind values
 * back explicitly, which the per-frame tick then keeps asserting. */
static void PlayAs_ClearRetarget(void)
{
    s_skinBindFileIdx = (s16)NO_VALUE;
    s_extraCount      = 0; /* Harry's rig stops at bone 17 */

    if (s_rootYPatched)
    {
        ((u8*)FS_BUFFER_0)[0x13] = s_rootYOrig;
    }

    if (s_harryBindLatched)
    {
        memcpy(s_skinBindT, s_harryBindT, sizeof(s_skinBindT));
        memcpy(s_skinBindHas, s_harryBindHas, sizeof(s_skinBindHas));
        s_skinBindValid = 1; /* still writing — Harry's own values now */
    }
    else
    {
        s_skinBindValid = 0; /* no skin was ever active: never touch the skeleton */
    }
}

/* Bones past Harry's 18 — Lisa's hair, Alessa's hair, the Puppet Nurse's second
 * body, the Incubator's sleeves — exist in the SKIN's rig only, so HB_BASE.ANM
 * never poses them. Seat them as rigid children from the skin's OWN bind chain
 * (parent + translationInitial << scaleLog2, read with the rest of its binds)
 * in g_SysWork's five spare coords, which sit directly after
 * playerBoneCoords[17] and are what the draw path indexes for bones 18..22. */
static void PlayAs_ExtraBonesInit(void)
{
    int i;
    int r;
    int c;

    for (i = 0; i < s_extraCount; i++)
    {
        GsCOORDINATE2* coord  = &g_SysWork.unkCoords_E30[i];
        u8             parent = s_extraParent[i];

        memset(coord, 0, sizeof(*coord));
        for (r = 0; r < 3; r++)
        {
            for (c = 0; c < 3; c++)
            {
                coord->coord.m[r][c] = (s16)s_extraR[i][r * 3 + c];
            }
        }
        coord->coord.t[0] = s_extraT[i][0];
        coord->coord.t[1] = s_extraT[i][1];
        coord->coord.t[2] = s_extraT[i][2];
        /* A parent past 17 is another extra bone (the nurse's body chain and
         * Lisa's third hair segment both do this). */
        coord->super = (parent >= PLAYAS_BONE_MAX)
                           ? &g_SysWork.unkCoords_E30[parent - PLAYAS_BONE_MAX]
                           : &g_SysWork.playerBoneCoords[parent];
        coord->flg = 0;
    }
}

void Pc_PlayAs_PlayerAnimTick(void)
{
    int i;

    /* Re-assert the skin's bind offsets every frame rather than once after
     * Anim_BoneInit: it costs 17 vector writes, it is idempotent (Anim_BoneUpdate
     * never writes these translations back), and it cannot be undone by any
     * re-init path — map load, save load or warm reset. Runs at the tail of
     * Player_Update, so it lands after the pose and before the frame's
     * all-bones flg reset, which is what makes the world matrices recompose. */
    if (s_skinBindValid)
    {
        for (i = 1; i < PLAYAS_BONE_MAX; i++)
        {
            if (s_skinBindHas[i])
            {
                GsCOORDINATE2* c = &g_SysWork.playerBoneCoords[i];

                c->coord.t[0] = s_skinBindT[i][0];
                c->coord.t[1] = s_skinBindT[i][1];
                c->coord.t[2] = s_skinBindT[i][2];
            }
        }
    }

    if (s_extraCount == 0)
    {
        return;
    }

    /* super == NULL means a warm reset's SysWork_Clear wiped the coords. The
     * per-frame flg clear forces Vw_CoordHierarchyMatrixCompute to recompose
     * against the freshly-animated parent — flg is its "already processed"
     * cache and nothing else resets it for bones no ANM touches. */
    if (g_SysWork.unkCoords_E30[0].super == NULL)
    {
        PlayAs_ExtraBonesInit();
        return;
    }
    for (i = 0; i < s_extraCount; i++)
    {
        g_SysWork.unkCoords_E30[i].flg = 0;
    }
}

void Pc_PlayAs_PlayerImageDesc(void* imageDesc)
{
    s_FsImageDesc* image  = (s_FsImageDesc*)imageDesc;
    s32            slotId = HIRES_POOL_PLAYAS_SLOT;

    /* Keyed on the FILE, not the roster, so every retarget path is covered —
     * same test Pc_PlayAs_PlayerLmRedirect uses for the ILM. Stock Harry keeps
     * the vanilla parcel, so normal play is byte-identical. */
    if (CHARA_FILE_INFOS[Chara_Harry].textureFileIdx == FILE_CHARA_HERO_TIM)
    {
        return;
    }

    /* Chara_FsImageCalc hands the player tPage 27 at (704,256) with the chara
     * CLUT shelf directly below it, sized for HERO.TIM's 192 rows and 15-row
     * palette. Skins are bigger: PRS/PRSD are 256 rows, which reaches down over
     * the whole four-slot monster CLUT band at y464..479 and turns every enemy
     * in the map rainbow, and PRS's 48-row palette runs off the bottom of VRAM.
     * Hand the skin a virtual pool slot instead — Fs_QueuePostLoadTim skips both
     * LoadImage calls for clutY >= HIRES_POOL_CLUT_ROW_BASE and decodes straight
     * to GL, so no size can overrun anything. Geometry-agnostic by construction:
     * MOTH/BOS/BIG can join the roster with no further work. */
    image->tPage[0] = 0;
    image->tPage[1] = 28; /* GL override path keys off the clut word only */
    image->u        = 0;  /* must not be UCHAR_MAX or the virtual-slot gate fails */
    image->v        = 0;
    image->clutX    = (s16)((slotId % 64) * 16);
    image->clutY    = (s16)(HIRES_POOL_CLUT_ROW_BASE + (slotId / 64) * HIRES_POOL_MAX_ROWS);

    HiresOverride_CharaPoolSlotReset(slotId);
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
        PlayAs_ClearRetarget();
        return;
    }

    /* The skin's own skeleton proportions. Loaded here rather than at
     * Pc_PlayAs_Init, which runs before the filesystem exists. */
    PlayAs_LoadSkinBinds();
    PlayAs_EnsureHeroTimSlot();
    Pc_PlayAs_ApplySkinVisibility();
    PlayAs_ExtraBonesInit();

    /* Ghost Alessa, the Ghost Doctor and the Incubator ship with EVERY
     * primitive flagged semi-transparent — that is their apparition look, baked
     * into the model rather than applied by the ghost-fade code. As the player
     * character it just reads as a see-through body, so the flag comes off.
     * A no-op on the eleven skins whose prims are already solid. */
    Lm_TransparentPrimSet(g_WorldGfxWork.harryModel.lmHdr, false);
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
