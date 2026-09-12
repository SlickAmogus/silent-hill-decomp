/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_ghost.c - other players, and the markers they leave, rendered into
 * the world.
 *
 * Render path is pc_decals.c's, for the same reasons: world-space corners as
 * Q8 offsets from a Vw_WorldScreenMatrixAtPositionGet matrix, RotTransPers per
 * corner, prims taken from the GsOUT_PACKET_P arena (the OT0 sanitizer in
 * game_main.c only accepts prims that live inside that buffer), addPrim into
 * the world OT at the same SZ>>1 bucket scale the world geometry uses. Going
 * through the game's own ordering table rather than a GL overlay is what makes
 * a ghost occlude behind walls and fade into the fog for free.
 *
 * THE ART IS GENERATED, not shipped (sh_net_art.c): a ghost is a hollow
 * humanoid contour and a marker is a ring sigil, both rasterized from a
 * signed-distance field into a hires-override pool slot. Two reasons: a
 * texture the port draws itself cannot be missing at runtime the way
 * gamedata/decal.png can, and a contour IS a distance threshold, which is
 * what an SDF is already computing. gamedata/ghost.png and gamedata/memo.png
 * override them when present.
 *
 * The billboard is CYLINDRICAL: its horizontal axis is the camera's right
 * vector read out of the world->screen matrix, its vertical axis is world up.
 * A fully camera-facing quad would lean with the camera pitch and a standing
 * figure that leans reads as a bug.
 */

#include "game.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bodyprog/collision/collision.h"
#include "bodyprog/math/math.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/gfx/world.h"
#include "bodyprog/screen/screen_data.h"
#include "bodyprog/view/vw_calc.h"
#include "bodyprog/game_boot/fs_chara_anim.h"
#include "main/fsqueue.h"

#include "hires_override.h"
#include "pc_config.h"
#include "sh_net.h"
#include "sh_net_art.h"
#include "sh_net_ui.h"
#include "sh_log.h"
#include "stb_image.h"

/* Pool slots. Above HIRES_POOL_CHARA_SLOT_BASE so HiresOverride_PoolSlotsReset
 * (which only frees below it) leaves them alone across map loads; below the
 * minimap's 510/511. Prim clut encoding is the canonical one in
 * hires_override.h. */
#define GHOST_SLOT (HIRES_POOL_SLOT_MAX - 3)
#define MEMO_SLOT  (HIRES_POOL_SLOT_MAX - 4)

#define SHNET_CLUT(slot)                                                        \
    (u16)((((HIRES_POOL_CLUT_ROW_BASE + ((slot) / 64) * HIRES_POOL_MAX_ROWS)) << 6) | \
          ((slot) % 64))

#define GHOST_TEX_W SHNET_ART_GHOST_W
#define GHOST_TEX_H SHNET_ART_GHOST_H
#define MEMO_TEX_W  SHNET_ART_MEMO_W
#define MEMO_TEX_H  SHNET_ART_MEMO_H

/* World units. SH1 is metric: pc_decals.c's Q12(0.05f) is documented as ~5cm,
 * so a 1.80-tall, 0.72-wide quad is a person-sized billboard. */
#define GHOST_HALF_W Q12(0.36f)
#define GHOST_HALF_H Q12(0.90f)

/* Floor markers sit a hair above the ground for the same z-fighting reason
 * decals do, and PSX -Y is up, so "above" is a SUBTRACTION. */
#define MEMO_HALF    Q12(0.30f)
#define MEMO_LIFT    Q12(0.03f)

/* Same bucket scale as everything else in OT0. Getting this wrong is the
 * OT-bucket-scale class in pc_decals.c: a semi-transparent prim in a nearer
 * bucket still paints last, however transparent it is. */
#define SHNET_OT_SHIFT 1

/* Hard cap on prims per frame, whatever the server sends. The frame packet
 * arena is shared with the world and has nothing stopping emission past its
 * end (see PC_WM_PACKET_BUDGET in bodyprog_80040B74.c), and the counts here
 * are driven by a REMOTE party: a server bug, or a hostile one, could put
 * hundreds of markers on one map. Range culling already keeps the normal case
 * far below this; the cap is what makes the abnormal case a few missing
 * markers instead of a corrupted arena. */
#define SHNET_MAX_PRIMS 192

/* A ghost further than this is not drawn. Not an optimization — the point is
 * that a silhouette visible across the whole map would give away rooms the
 * player has not reached. Config: online_ghost_range (metres). */
#define GHOST_RANGE_DEFAULT 40

extern s_WorldEnvWork g_WorldEnvWork;
extern int            Pc_BloodFogKeep(s32 z);
extern float          g_PsyX_FogStrength;
extern void           PsyX_SetNextPrimAlpha(int a);

static int s_ghostTexOk;
static int s_memoTexOk;
static int s_texTried;

/* ------------------------------------------------------------------ */
/* Texture registration                                                */
/* ------------------------------------------------------------------ */

/* An override PNG replaces the generated art entirely. Returns a malloc'd RGBA
 * buffer plus its dimensions, or NULL when the file is absent. */
static unsigned char* ShNetG_LoadOverride(const char* path, int* outW, int* outH)
{
    FILE*          f = fopen(path, "rb");
    long           size;
    unsigned char* file;
    unsigned char* rgba;
    int            comp = 0;

    if (!f)
    {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    file = (size > 0) ? (unsigned char*)malloc((size_t)size) : NULL;
    if (!file || fread(file, 1, (size_t)size, f) != (size_t)size)
    {
        fclose(f);
        free(file);
        return NULL;
    }
    fclose(f);

    rgba = stbi_load_from_memory(file, (int)size, outW, outH, &comp, 4);
    free(file);
    return rgba;
}

/* Deferred to the first draw so a GL context exists, exactly as the decal
 * texture is. Runs once per session: these slots sit above the chara base, so
 * HiresOverride_PoolSlotsReset never frees them on a map load. */
static void ShNetG_EnsureTextures(void)
{
    if (s_texTried)
    {
        return;
    }
    s_texTried = 1;

    {
        int            w = GHOST_TEX_W, h = GHOST_TEX_H;
        unsigned char* rgba = ShNetG_LoadOverride("gamedata/ghost.png", &w, &h);
        int            generated = 0;
        if (!rgba)
        {
            w = GHOST_TEX_W;
            h = GHOST_TEX_H;
            rgba = ShNetArt_MakeGhost();
            generated = 1;
        }
        if (rgba)
        {
            s_ghostTexOk = (HiresOverride_PoolSlotRegisterRGBA(
                                GHOST_SLOT, 0, rgba, w, h,
                                GHOST_TEX_W, GHOST_TEX_H) == 0);
            if (generated) free(rgba); else stbi_image_free(rgba);
        }
        SH_DBG("[NET] ghost texture %s (%dx%d, slot %d, clut 0x%04X)",
               s_ghostTexOk ? (generated ? "generated" : "from gamedata/ghost.png") : "FAILED",
               w, h, GHOST_SLOT, (unsigned)SHNET_CLUT(GHOST_SLOT));
    }

    {
        int            w = MEMO_TEX_W, h = MEMO_TEX_H;
        unsigned char* rgba = ShNetG_LoadOverride("gamedata/memo.png", &w, &h);
        int            generated = 0;
        if (!rgba)
        {
            w = MEMO_TEX_W;
            h = MEMO_TEX_H;
            rgba = ShNetArt_MakeMarker();
            generated = 1;
        }
        if (rgba)
        {
            s_memoTexOk = (HiresOverride_PoolSlotRegisterRGBA(
                               MEMO_SLOT, 0, rgba, w, h,
                               MEMO_TEX_W, MEMO_TEX_H) == 0);
            if (generated) free(rgba); else stbi_image_free(rgba);
        }
        SH_DBG("[NET] marker texture %s (slot %d, clut 0x%04X)",
               s_memoTexOk ? "ready" : "FAILED", MEMO_SLOT,
               (unsigned)SHNET_CLUT(MEMO_SLOT));
    }
}

/* ------------------------------------------------------------------ */
/* Colour                                                              */
/* ------------------------------------------------------------------ */

/* Desaturated and dim on purpose: a saturated ghost in a Silent Hill corridor
 * looks like a debug marker. Every entry stays under 160 so the fog still
 * swallows it at range. */
static const u8 s_ghostPalette[8][3] = {
    { 150, 158, 160 }, /* pale grey-cyan */
    { 160, 142, 116 }, /* amber */
    { 132, 150, 158 }, /* steel */
    { 158, 130, 134 }, /* rose */
    { 136, 156, 132 }, /* moss */
    { 148, 136, 158 }, /* violet */
    { 158, 152, 124 }, /* bone */
    { 124, 146, 156 }  /* slate */
};

static void ShNetG_GhostColor(unsigned int playerId, u8* r, u8* g, u8* b)
{
    unsigned int h = playerId * 2654435761u;
    const u8*    c = s_ghostPalette[(h >> 28) & 7];
    *r = c[0];
    *g = c[1];
    *b = c[2];
}

static void ShNetG_MarkerColor(int kind, u8* r, u8* g, u8* b)
{
    switch (kind)
    {
    case SHNET_MARK_DEATH: *r = 150; *g = 40;  *b = 44;  break;
    case SHNET_MARK_SAVE:  *r = 96;  *g = 148; *b = 108; break;
    default:               *r = 158; *g = 142; *b = 96;  break;
    }
}

/* ------------------------------------------------------------------ */
/* Quad emission                                                       */
/* ------------------------------------------------------------------ */

/* Project one world-space quad and add it to the OT. `centre` is Q19.12 world;
 * axisU/axisV are Q19.12 HALF extents. Returns the next free packet slot, or
 * `poly` unchanged when the quad was rejected (behind the camera, off screen,
 * or fully fogged). */
static POLY_FT4* ShNetG_EmitQuad(GsOT* ot, POLY_FT4* poly,
                                 const VECTOR3* centre,
                                 const VECTOR3* axisU, const VECTOR3* axisV,
                                 u16 clut, int uMax, int vMax,
                                 u8 cr, u8 cg, u8 cb, int alphaScale)
{
    MATRIX  mat;
    SVECTOR v;
    s16     sx[4];
    s16     sy[4];
    s32     bucketSum = 0;
    s32     bucket;
    int     k;
    int     keep;

    Vw_WorldScreenMatrixAtPositionGet(&mat, centre->vx, centre->vy, centre->vz);
    SetRotMatrix(&mat);
    SetTransMatrix(&mat);

    for (k = 0; k < 4; k++)
    {
        /* PSX quad Z-order: 0=TL 1=TR 2=BL 3=BR. */
        s32  st = (k & 1) ? 1 : -1;
        s32  sb = (k & 2) ? 1 : -1;
        s32  otz;
        s32  sxy;
        long p;
        long flag;

        v.vx  = (s16)Q12_TO_Q8(st * axisU->vx + sb * axisV->vx);
        v.vy  = (s16)Q12_TO_Q8(st * axisU->vy + sb * axisV->vy);
        v.vz  = (s16)Q12_TO_Q8(st * axisU->vz + sb * axisV->vz);
        v.pad = 0;

        otz = RotTransPers(&v, (int*)&sxy, &p, &flag);
        if (otz <= 8)
        {
            return poly;
        }
        sx[k] = (s16)(sxy & 0xFFFF);
        sy[k] = (s16)((u32)sxy >> 16);
        if (ABS(sx[k]) > 2048 || ABS(sy[k]) > 2048)
        {
            return poly;
        }
        bucketSum += otz;
    }

    /* Fog. bucketSum is the sum of four quartered corner OTZs, which IS the
     * average SZ — the same quantity the blood prims hand to Pc_BloodFogKeep,
     * and the correction pc_decals.c had to make twice. The keep is scaled by
     * the shader's fog strength because that is what the scene actually looks
     * like, not what the unscaled ramp says. */
    {
        int fade = 256 - Pc_BloodFogKeep(bucketSum);
        fade = (int)(fade * (g_PsyX_FogStrength > 0.0f ? g_PsyX_FogStrength : 1.0f));
        if (fade > 256) fade = 256;
        keep = 256 - fade;
        keep = (keep * alphaScale) >> 8;
        if (keep < 24)
        {
            return poly;
        }
        if (keep > 255) keep = 255;
    }

    bucket = (bucketSum >> 2) >> SHNET_OT_SHIFT;
    bucket -= 1; /* one bucket nearer than the surface, as the decals do */
    if (bucket < 1)
    {
        bucket = 1;
    }
    if (bucket >= ORDERING_TABLE_SIZE)
    {
        bucket = ORDERING_TABLE_SIZE - 1;
    }

    setPolyFT4(poly);
    setSemiTrans(poly, true);
    PsyX_SetNextPrimAlpha(keep);
    setRGB0(poly, cr, cg, cb);

    /* tpage is irrelevant: the bit-15 clut alone keys the GL override's
     * pool-slot fast path, and page bits never sample VRAM. */
    poly->tpage = 0;
    poly->clut  = clut;
    poly->u0 = 0;            poly->v0 = 0;
    poly->u1 = (u8)uMax;     poly->v1 = 0;
    poly->u2 = 0;            poly->v2 = (u8)vMax;
    poly->u3 = (u8)uMax;     poly->v3 = (u8)vMax;
    setXY4(poly, sx[0], sy[0], sx[1], sy[1], sx[2], sy[2], sx[3], sy[3]);

    addPrim(&ot->org[bucket], poly);
    return poly + 1;
}

/* ------------------------------------------------------------------ */
/* Draw                                                                */
/* ------------------------------------------------------------------ */

static s32 ShNetG_RangeQ12(void)
{
    int m = g_PcConfig.onlineGhostRange > 0 ? g_PcConfig.onlineGhostRange
                                            : GHOST_RANGE_DEFAULT;
    if (m > 400) m = 400;
    return (s32)m << 12;
}

/* Floor ring under a ghost, in the marker texture. Drawn for the FOOTPRINT and
 * BOTH styles: the contour alone disappears behind a doorframe, and knowing
 * exactly where another player is standing is most of the appeal. */
static POLY_FT4* ShNetG_EmitFootprint(GsOT* ot, POLY_FT4* poly,
                                      int gx, int gy, int gz,
                                      u8 cr, u8 cg, u8 cb)
{
    VECTOR3            centre;
    VECTOR3            axisU;
    VECTOR3            axisV;
    s_CollisionSurface surf;

    Collision_SurfaceGet(&surf, gx, gz);
    centre.vx = gx;
    centre.vy = surf.groundHeight - MEMO_LIFT;
    centre.vz = gz;
    if (ABS(centre.vy - gy) > Q12(2.5f))
    {
        centre.vy = gy - MEMO_LIFT;
    }

    axisU.vx = MEMO_HALF;
    axisU.vy = 0;
    axisU.vz = 0;
    axisV.vx = 0;
    axisV.vy = 0;
    axisV.vz = MEMO_HALF;

    return ShNetG_EmitQuad(ot, poly, &centre, &axisU, &axisV,
                           SHNET_CLUT(MEMO_SLOT),
                           MEMO_TEX_W - 1, MEMO_TEX_H - 1, cr, cg, cb, 190);
}

#define GHOST_BONE_MAX 64
#define SHNET_MAX_MODELS 16

static int ShNetG_TryDrawModel(int charaId, int gx, int gy, int gz, short grot, int animFrame)
{
    static GsCOORDINATE2 s_coords[GHOST_BONE_MAX];
    s_CharaModel*  cm;
    s_AnmHeader*   anm;
    SVECTOR        rot;
    s16            ret;
    int            slot;

    if (charaId <= 0 || charaId >= Chara_Count)
    {
        return 0;
    }
    cm = g_WorldGfxWork.registeredCharaModels[charaId];
    if (cm == NULL || cm->skeleton.bones_4 == NULL)
    {
        return 0;
    }
    /* Harry's animation is not in the pool: the player poses itself from the
     * base ANM at FS_BUFFER_0, so a Harry ghost reads the same header. */
    if (charaId == Chara_Harry)
    {
        anm = (s_AnmHeader*)FS_BUFFER_0;
    }
    else
    {
        slot = PC_CHARA_ANIM_SLOT(charaId);
        if (slot < 0 || slot >= CHARA_ANIM_DATA_COUNT)
        {
            return 0;
        }
        anm = g_CharaModelAnimsData[slot].activeAnmHdr;
    }
    if (anm == NULL || anm->boneCount <= 0 || anm->boneCount > GHOST_BONE_MAX)
    {
        return 0;
    }

    /* Pose the skeleton to its resting keyframe, then plant the root at the
     * ghost's world position and heading. Both operations mirror the spawned-
     * actor path exactly. */
    {
        /* The ghost's own keyframe, into the base ANM's shared keyframe bank,
         * so a walking player's ghost actually walks. Clamped: a keyframe from
         * a map-specific clip the base bank does not contain would be garbage,
         * and clamping keeps it in-range rather than reading past the bank. */
        int kf = animFrame;
        int maxKf = (anm->keyframeCount > 0) ? (int)anm->keyframeCount - 1 : 0;
        if (kf < 0)     kf = 0;
        if (kf > maxKf) kf = maxKf;
        Anim_BoneInit(anm, s_coords);
        Anim_BoneUpdate(anm, s_coords, kf, kf, Q12(0.0f));
    }

    rot.vx = 0;
    rot.vy = grot;
    rot.vz = 0;
    rot.pad = 0;
    Math_RotMatrixZxyNegGte(&rot, &s_coords[0].coord);
    s_coords[0].coord.t[0] = Q12_TO_Q8(gx);
    s_coords[0].coord.t[1] = Q12_TO_Q8(gy);
    s_coords[0].coord.t[2] = Q12_TO_Q8(gz);
    s_coords[0].flg = 0;

    /* One-shot per session: confirms the experimental model path took effect
     * for whoever is testing it. */
    { static int s_once = 0; if (!s_once) { s_once = 1;
        SH_DBG("[NET] modeled ghosts active (chara %d, %d bones)", charaId, (int)anm->boneCount); } }
    ret = (s16)func_8003DD74((e_CharaId)charaId, 0);
    func_80045534(&cm->skeleton, &g_OrderingTable0[g_ActiveBufferIdx], 1,
                  s_coords, Q8_TO_Q12(CHARA_FILE_INFOS[charaId].field_6),
                  (u16)ret, CHARA_FILE_INFOS[charaId].field_8);
    return 1;
}

/* Pool slots for baked name textures. Above the chara base so a map load's
 * PoolSlotsReset leaves them alone; below the minimap's 510/511 and the ghost/
 * marker 509/508. */
#define NAMEPLATE_SLOT0 496
#define NAMEPLATE_SLOTS 8
#define NAMEPLATE_PX    26

static struct { char name[SHNET_NAME_MAX]; int w, h; int used; } s_np[NAMEPLATE_SLOTS];
static int s_npNext;

/* Slot index (0..NAMEPLATE_SLOTS-1) holding `name`, baking + registering it if
 * new. Round-robin eviction: with <= 8 names on screen nothing is evicted. */
static int ShNetG_NameplateSlot(const char* name)
{
    int i;
    if (!name || !name[0])
    {
        return -1;
    }
    for (i = 0; i < NAMEPLATE_SLOTS; i++)
    {
        if (s_np[i].used && strcmp(s_np[i].name, name) == 0)
        {
            return i;
        }
    }
    {
        int            idx = s_npNext;
        int            w = 0, h = 0;
        unsigned char* rgba = ShNetUi_RasterizeText(name, NAMEPLATE_PX, &w, &h);
        if (!rgba)
        {
            return -1;
        }
        if (HiresOverride_PoolSlotRegisterRGBA(NAMEPLATE_SLOT0 + idx, 0, rgba, w, h, w, h) != 0)
        {
            free(rgba);
            return -1;
        }
        free(rgba);
        strncpy(s_np[idx].name, name, SHNET_NAME_MAX - 1);
        s_np[idx].name[SHNET_NAME_MAX - 1] = '\0';
        s_np[idx].w    = w;
        s_np[idx].h    = h;
        s_np[idx].used = 1;
        s_npNext = (s_npNext + 1) % NAMEPLATE_SLOTS;
        return idx;
    }
}

static POLY_FT4* ShNetG_DrawNameplate(GsOT* ot, POLY_FT4* poly, const char* name,
                                      int gx, int gy, int gz)
{
    int     idx = ShNetG_NameplateSlot(name);
    VECTOR3 centre;
    VECTOR3 axisU;
    VECTOR3 axisV;
    s32     halfH = Q12(0.16f);
    s32     halfW;

    if (idx < 0)
    {
        return poly;
    }
    halfW = (s32)(((long long)halfH * s_np[idx].w) / (s_np[idx].h ? s_np[idx].h : 1));

    /* Just above the head. Feet are at gy, PSX -Y is up, the body is ~1.8m. */
    centre.vx = gx;
    centre.vy = gy - Q12(2.05f);
    centre.vz = gz;
    axisU.vx = (halfW * GsWSMATRIX.m[0][0]) >> 12;
    axisU.vy = (halfW * GsWSMATRIX.m[0][1]) >> 12;
    axisU.vz = (halfW * GsWSMATRIX.m[0][2]) >> 12;
    axisV.vx = 0;
    axisV.vy = -halfH;
    axisV.vz = 0;

    return ShNetG_EmitQuad(ot, poly, &centre, &axisU, &axisV,
                           SHNET_CLUT(NAMEPLATE_SLOT0 + idx),
                           s_np[idx].w - 1, s_np[idx].h - 1, 235, 230, 205, 255);
}

void ShNet_DrawWorld(GsOT* ot)
{
    POLY_FT4*    poly;
    POLY_FT4*    primBase;
    int          modelsDrawn;
    int          namesDrawn;
    int          i;
    int          count;
    const int    style = g_PcConfig.onlineGhostStyle;
    s32          range;
    const VECTOR3* self = &g_SysWork.playerWork.player.position;

    if (!g_PcConfig.onlineEnabled || ShNet_Status() != SHNET_ST_CONNECTED)
    {
        return;
    }
    if (g_GameWork.gameState != GameState_InGame)
    {
        return;
    }

    ShNetG_EnsureTextures();
    if (!s_ghostTexOk && !s_memoTexOk)
    {
        return;
    }

    poly     = (POLY_FT4*)GsOUT_PACKET_P;
    primBase = poly;
    range    = ShNetG_RangeQ12();
    modelsDrawn = 0;
    namesDrawn  = 0;

    /* ---- other players ---- */
    if (g_PcConfig.onlineGhosts && (s_ghostTexOk || s_memoTexOk))
    {
        count = ShNet_GhostCount();
        for (i = 0; i < count; i++)
        {
            const ShNetGhost* g = ShNet_Ghost(i);
            VECTOR3           centre;
            VECTOR3           axisU;
            VECTOR3           axisV;
            int               gx, gy, gz;
            short             grot;
            u8                cr, cg, cb;

            if (poly - primBase >= SHNET_MAX_PRIMS)
            {
                break;
            }
            if (!g || !ShNet_GhostInterp(i, &gx, &gy, &gz, &grot))
            {
                continue;
            }
            if (ABS(gx - self->vx) > range || ABS(gz - self->vz) > range)
            {
                continue;
            }

            /* Feet at the reported position, so the quad rises to head height.
             * PSX -Y is up. */
            centre.vx = gx;
            centre.vy = gy - GHOST_HALF_H;
            centre.vz = gz;

            /* The camera's right axis in world space is row 0 of the
             * world->screen ROTATION: for a rotation R, R*v gives view
             * coordinates, so the world vector that maps to view +X is
             * transpose(R) * (1,0,0), which reads out as that row.
             * Vw_WorldScreenMatrixAtPositionGet copies this same rotation
             * verbatim and only varies the translation, so reading GsWSMATRIX
             * saves building a second matrix per ghost. */
            axisU.vx = (GHOST_HALF_W * GsWSMATRIX.m[0][0]) >> 12;
            axisU.vy = (GHOST_HALF_W * GsWSMATRIX.m[0][1]) >> 12;
            axisU.vz = (GHOST_HALF_W * GsWSMATRIX.m[0][2]) >> 12;

            axisV.vx = 0;
            axisV.vy = -GHOST_HALF_H; /* world up */
            axisV.vz = 0;

            ShNetG_GhostColor(g->playerId, &cr, &cg, &cb);
            /* A ghost with no flashlight and no motion is a little dimmer, so
             * the ones actually moving read first. */
            {
                int drewModel = 0;
                /* A full character model costs several KB of the shared packet
                 * arena, so cap how many draw as models per frame; the rest
                 * fall back to the cheap silhouette. */
                if (g_PcConfig.onlineGhostModel && modelsDrawn < SHNET_MAX_MODELS &&
                    !(g->flags & SHNET_PF_CUTSCENE))
                {
                    /* func_80045534 allocates through GsOUT_PACKET_P, so commit
                     * the silhouette cursor into it first and read it back
                     * after. primBase is re-based by the same delta so the
                     * silhouette-prim budget below is not tripped by the
                     * model's own packets. */
                    unsigned char* before = (unsigned char*)poly;
                    GsOUT_PACKET_P = (PACKET*)poly;
                    drewModel = ShNetG_TryDrawModel(g->charaId, gx, gy, gz, grot, (int)g->animFrame);
                    {
                        unsigned char* after = (unsigned char*)GsOUT_PACKET_P;
                        poly     = (POLY_FT4*)after;
                        primBase = (POLY_FT4*)((unsigned char*)primBase + (after - before));
                    }
                    if (drewModel) modelsDrawn++;
                }
                if (!drewModel && s_ghostTexOk && style != SHNET_GS_FOOTPRINT)
                {
                    int alpha = 230;
                    if (!(g->flags & SHNET_PF_ALIVE))   alpha = 150;
                    if (g->flags & SHNET_PF_FLASHLIGHT) alpha = 255;
                    poly = ShNetG_EmitQuad(ot, poly, &centre, &axisU, &axisV,
                                           SHNET_CLUT(GHOST_SLOT),
                                           GHOST_TEX_W - 1, GHOST_TEX_H - 1,
                                           cr, cg, cb, alpha);
                }
            }
            if (s_memoTexOk && style != SHNET_GS_SILHOUETTE)
            {
                poly = ShNetG_EmitFootprint(ot, poly, gx, gy, gz, cr, cg, cb);
            }

            /* The player's name over their head. Off is a config toggle; the
             * '?' placeholder (no roster entry yet) is skipped. */
            if (g_PcConfig.onlineNameplates && namesDrawn < NAMEPLATE_SLOTS &&
                g->name[0] && g->name[0] != '?')
            {
                poly = ShNetG_DrawNameplate(ot, poly, g->name, gx, gy, gz);
                namesDrawn++;
            }
        }
    }

    /* ---- markers ---- */
    if (g_PcConfig.onlineMemos && s_memoTexOk)
    {
        count = ShNet_MemoCount();
        for (i = 0; i < count; i++)
        {
            const ShNetMemo* m = ShNet_Memo(i);
            VECTOR3          centre;
            VECTOR3          axisU;
            VECTOR3          axisV;
            u8               cr, cg, cb;

            if (poly - primBase >= SHNET_MAX_PRIMS)
            {
                break;
            }
            if (!m)
            {
                continue;
            }
            if (m->kind == SHNET_MARK_DEATH && !g_PcConfig.onlineDeaths)
            {
                continue;
            }
            if (ABS(m->x - self->vx) > range || ABS(m->z - self->vz) > range)
            {
                continue;
            }

            /* Snap to the floor under the marker rather than trusting the
             * placer's Y: they may have been standing on a step the local
             * geometry does not have loaded, and a marker floating in the air
             * is worse than one a few centimetres off. */
            {
                s_CollisionSurface surf;
                Collision_SurfaceGet(&surf, m->x, m->z);
                centre.vx = m->x;
                centre.vy = surf.groundHeight - MEMO_LIFT;
                centre.vz = m->z;
                /* A floor sample that disagrees wildly with where the marker
                 * was placed means a different storey; keep the placer's. */
                if (ABS(centre.vy - m->y) > Q12(2.5f))
                {
                    centre.vy = m->y - MEMO_LIFT;
                }
            }

            axisU.vx = MEMO_HALF;
            axisU.vy = 0;
            axisU.vz = 0;
            axisV.vx = 0;
            axisV.vy = 0;
            axisV.vz = MEMO_HALF;

            ShNetG_MarkerColor(m->kind, &cr, &cg, &cb);
            poly = ShNetG_EmitQuad(ot, poly, &centre, &axisU, &axisV,
                                   SHNET_CLUT(MEMO_SLOT),
                                   MEMO_TEX_W - 1, MEMO_TEX_H - 1,
                                   cr, cg, cb, 255);
        }
    }

    GsOUT_PACKET_P = (PACKET*)poly;
}
