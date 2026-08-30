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
 * THE ART IS GENERATED, not shipped. A ghost is a hollow humanoid contour and
 * a marker is a ring sigil, both rasterized here from a signed-distance field
 * into a hires-override pool slot. Two reasons: a texture the port draws
 * itself cannot be missing at runtime the way gamedata/decal.png can, and a
 * contour is defined by a distance threshold, which is exactly what an SDF is
 * already computing. gamedata/ghost.png and gamedata/memo.png override them
 * when present.
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
#include "bodyprog/gfx/world.h"
#include "bodyprog/view/vw_calc.h"

#include "hires_override.h"
#include "pc_config.h"
#include "sh_net.h"
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

#define GHOST_TEX_W 64
#define GHOST_TEX_H 128
#define MEMO_TEX_W  64
#define MEMO_TEX_H  64

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
/* Procedural art                                                      */
/* ------------------------------------------------------------------ */

static float ShNetG_SegDist(float px, float py, float ax, float ay, float bx, float by)
{
    float vx = bx - ax, vy = by - ay;
    float wx = px - ax, wy = py - ay;
    float len2 = vx * vx + vy * vy;
    float t    = (len2 > 0.0001f) ? ((wx * vx + wy * vy) / len2) : 0.0f;
    float dx, dy;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    dx = px - (ax + vx * t);
    dy = py - (ay + vy * t);
    return sqrtf(dx * dx + dy * dy);
}

/* Negative inside the body, positive outside — a plain union of capsules that
 * happens to look like a person from the front. */
static float ShNetG_BodySdf(float x, float y)
{
    float d;
    float head = sqrtf((x - 32.0f) * (x - 32.0f) + (y - 19.0f) * (y - 19.0f)) - 11.0f;

    d = head;
    /* torso */
    { float t = ShNetG_SegDist(x, y, 32.0f, 33.0f, 32.0f, 70.0f) - 12.5f; if (t < d) d = t; }
    /* arms */
    { float t = ShNetG_SegDist(x, y, 21.0f, 36.0f, 14.0f, 68.0f) - 5.0f;  if (t < d) d = t; }
    { float t = ShNetG_SegDist(x, y, 43.0f, 36.0f, 50.0f, 68.0f) - 5.0f;  if (t < d) d = t; }
    /* legs */
    { float t = ShNetG_SegDist(x, y, 26.0f, 68.0f, 24.0f, 121.0f) - 6.0f; if (t < d) d = t; }
    { float t = ShNetG_SegDist(x, y, 38.0f, 68.0f, 40.0f, 121.0f) - 6.0f; if (t < d) d = t; }
    return d;
}

/* White pixels, all the shaping in alpha: the prim's own RGB tints the whole
 * thing, so one texture serves every ghost colour. */
static unsigned char* ShNetG_MakeGhostRgba(void)
{
    unsigned char* px = (unsigned char*)malloc(GHOST_TEX_W * GHOST_TEX_H * 4);
    int            x, y;

    if (!px)
    {
        return NULL;
    }

    for (y = 0; y < GHOST_TEX_H; y++)
    {
        for (x = 0; x < GHOST_TEX_W; x++)
        {
            float d = ShNetG_BodySdf((float)x + 0.5f, (float)y + 0.5f);
            float a;
            int   o = (y * GHOST_TEX_W + x) * 4;

            /* The contour: a band straddling d == 0, brightest on the line and
             * falling off over ~3px each way. This is the "hollow outline". */
            float band = 1.0f - (fabsf(d) / 3.2f);
            if (band < 0.0f) band = 0.0f;
            a = band * band * 235.0f;

            /* A very faint interior so the figure still reads as a body at
             * distance, where the contour is a couple of pixels wide. Kept low
             * enough that it never becomes a solid silhouette. */
            if (d < 0.0f)
            {
                float fill = 26.0f + 18.0f * (1.0f - (float)y / (float)GHOST_TEX_H);
                if (fill > a) a = fill;
            }
            if (a > 255.0f) a = 255.0f;

            px[o + 0] = 255;
            px[o + 1] = 255;
            px[o + 2] = 255;
            px[o + 3] = (unsigned char)a;
        }
    }
    return px;
}

/* A ring with four radial ticks. Same white-plus-alpha arrangement, so kind
 * (memo / death / save) is purely a prim colour. */
static unsigned char* ShNetG_MakeMemoRgba(void)
{
    unsigned char* px = (unsigned char*)malloc(MEMO_TEX_W * MEMO_TEX_H * 4);
    int            x, y;

    if (!px)
    {
        return NULL;
    }

    for (y = 0; y < MEMO_TEX_H; y++)
    {
        for (x = 0; x < MEMO_TEX_W; x++)
        {
            float fx = (float)x + 0.5f - 32.0f;
            float fy = (float)y + 0.5f - 32.0f;
            float r  = sqrtf(fx * fx + fy * fy);
            float a  = 0.0f;
            int   o  = (y * MEMO_TEX_W + x) * 4;

            {
                float ring = 1.0f - (fabsf(r - 22.0f) / 3.0f);
                if (ring > 0.0f) a = ring * ring * 255.0f;
            }
            {
                float inner = 1.0f - (fabsf(r - 9.0f) / 2.0f);
                if (inner > 0.0f && inner * inner * 190.0f > a) a = inner * inner * 190.0f;
            }
            /* Ticks at the cardinals, between the two rings. */
            if (r > 9.0f && r < 22.0f)
            {
                float tick = 0.0f;
                if (fabsf(fx) < 1.6f) tick = 1.0f - fabsf(fx) / 1.6f;
                if (fabsf(fy) < 1.6f)
                {
                    float t2 = 1.0f - fabsf(fy) / 1.6f;
                    if (t2 > tick) tick = t2;
                }
                if (tick * 170.0f > a) a = tick * 170.0f;
            }
            if (a > 255.0f) a = 255.0f;

            px[o + 0] = 255;
            px[o + 1] = 255;
            px[o + 2] = 255;
            px[o + 3] = (unsigned char)a;
        }
    }
    return px;
}

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
            rgba = ShNetG_MakeGhostRgba();
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
            rgba = ShNetG_MakeMemoRgba();
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

void ShNet_DrawWorld(GsOT* ot)
{
    POLY_FT4*    poly;
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

    poly  = (POLY_FT4*)GsOUT_PACKET_P;
    range = ShNetG_RangeQ12();

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
            if (s_ghostTexOk && style != SHNET_GS_FOOTPRINT)
            {
                int alpha = 230;
                if (!(g->flags & SHNET_PF_ALIVE))   alpha = 150;
                if (g->flags & SHNET_PF_FLASHLIGHT) alpha = 255;
                poly = ShNetG_EmitQuad(ot, poly, &centre, &axisU, &axisV,
                                       SHNET_CLUT(GHOST_SLOT),
                                       GHOST_TEX_W - 1, GHOST_TEX_H - 1,
                                       cr, cg, cb, alpha);
            }
            if (s_memoTexOk && style != SHNET_GS_SILHOUETTE)
            {
                poly = ShNetG_EmitFootprint(ot, poly, gx, gy, gz, cr, cg, cb);
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
