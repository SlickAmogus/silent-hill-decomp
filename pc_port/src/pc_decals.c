/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Bullet-hole decals — see pc_decals.h for the system overview.
 *
 * Render path mirrors the blood-splat drawer (func_80062708): world-space
 * corners as Q8 offsets from a Vw_WorldScreenMatrixAtPositionGet matrix,
 * RotTransPers per corner, POLY_FT4 from the GsOUT_PACKET_P arena (the OT0
 * sanitizer in game_main.c only accepts prims inside that packet buffer),
 * addPrim into the world OT at the SZ>>5 bucket world geometry uses. */
#include "game.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bodyprog/bodyprog.h"
#include "bodyprog/map/terrain.h"
#include "bodyprog/collision/collision.h"
#include "bodyprog/collision/ray.h"
#include "bodyprog/math/math.h"
#include "bodyprog/screen/screen_data.h"
#include "bodyprog/gfx/world.h"
#include "bodyprog/view/vw_calc.h"

#include "hires_override.h"
#include "pc_config.h"
#include "pc_decals.h"
#include "pc_wide_lm.h"
#include "sh_log.h"
#include "stb_image.h"

extern s_MapTerrain g_Map;

#define DECAL_MAX       64
#define DECAL_HALF      Q12(0.0375f) /* quad half-extent (~3.75cm) */
#define DECAL_OFFSET    Q12(0.02f) /* lift along the normal against z-fighting */
#define DECAL_FLOOR_TOL Q12(0.15f) /* |impactY - floorY| for the FLOOR case (blood splats use the same) */

/* Hires-override virtual pool slot reserved for the decal (Ipd_TexturesInit
 * excludes it from the chunk pool). UVs are TIM-native space: the override
 * shader maps 0..nativeW over the GL texture, so any decal.png size works. */
#define DECAL_POOL_SLOT HIRES_POOL_DECAL_SLOT
#define DECAL_NATIVE    32
#define DECAL_UV_MAX    (DECAL_NATIVE - 1)
/* Prim clut word for the slot — canonical encoding in hires_override.h:
 * clutY = 512 + (slot/64)*16 in bits 6..15 (bit 15 set), slot%64 in bits 0..5. */
#define DECAL_CLUT                                                             \
    (u16)((((HIRES_POOL_CLUT_ROW_BASE + (DECAL_POOL_SLOT / 64) * HIRES_POOL_MAX_ROWS)) << 6) | \
          (DECAL_POOL_SLOT % 64))

/* Depth handling is in two halves, and BOTH are needed:
 *
 *   1. SEMI-TRANSPARENT, like the blood splats (setSemiTrans in func_80062708),
 *      so the quad never wins the depth buffer against a character in front of
 *      the wall.
 *   2. Bucketed on OT0's own scale (DECAL_OT_SHIFT), so it never wins PAINTER
 *      order against him either.
 *
 * Getting only (1) is what shipped first and it was not enough: the quad was
 * also copying blood's >>3 bucketing, which is a different scale from the rest
 * of OT0, and a semi-transparent prim in a nearer bucket still paints last. */
/* OT0 bucket scale. func_80057090's shift argument is 1 at every character and
 * held-item call site (world_draw.c:746, :1034), and Ipd_ChunkDraw passes a
 * bool, so nothing in this table is coarser than >>1. The decal has to use the
 * same scale as the characters it must lose to — blood's >>3 does not, which is
 * why bullet holes drew over Harry's head while blood on the floor looks fine
 * (blood is only ever compared against things standing ON it). */
#define DECAL_OT_SHIFT 1

/* One bucket NEARER than the surface it sits on, so it still beats its own host
 * wall in painter order. DECAL_OFFSET already lifts it along the normal, but at
 * this shift the lift can round into the same bucket as the wall, and within a
 * bucket the draw order is list order, not depth. */
#define DECAL_OT_LIFT 1

typedef struct {
    VECTOR3 center; /* Q19.12 world, already offset along the normal */
    VECTOR3 axisU;  /* Q12 half-extent tangent */
    VECTOR3 axisV;  /* Q12 half-extent bitangent */
    u8      glass;  /* drawn with decalGlass.png */
} s_PcDecal;

/* What the eye sees where a bullet stopped. Collision knows only that
 * something is there: an invisible wall and a brick one are the same to it. */
enum { SURF_NONE, SURF_SOLID, SURF_GLASS };

/* The rendered wall is looked for this far either side of the collision hit
 * along the shot (Q8 world units, 256 = 1 m): collision planes sit a little
 * off the art. */
#define SURF_NEAR   128.0f
/* A pane itself is not drawn; what marks it is the cut-out backdrop standing
 * behind the opening, looked for this far past the hit. */
#define SURF_BEYOND 1024.0f

#define DECAL_OBJ_MAX 32
typedef struct
{
    const s_ModelHeader* modelHdr;
    MATRIX               mat;
} s_PcDecalObj;

/* World objects of the frame being drawn, and of the last one drawn whole:
 * the game empties its own list as it draws, before any shot is traced. */
static s_PcDecalObj s_objCur[DECAL_OBJ_MAX];
static s_PcDecalObj s_objLast[DECAL_OBJ_MAX];
static int          s_objCurCount;
static int          s_objLastCount;

void Pc_DecalsNoteWorldObject(const s_ModelHeader* modelHdr, const MATRIX* worldMat)
{
    if (modelHdr == NULL || worldMat == NULL || s_objCurCount >= DECAL_OBJ_MAX)
    {
        return;
    }
    s_objCur[s_objCurCount].modelHdr = modelHdr;
    s_objCur[s_objCurCount].mat      = *worldMat;
    s_objCurCount++;
}

typedef struct
{
    float o[3];      /* shot origin, Q8 world */
    float d[3];      /* unit direction */
    float len;       /* origin to the collision hit */
    float nearT;     /* nearest drawn surface around the hit, <0 none */
    float beyondT;   /* nearest drawn surface past it, <0 none */
    int   beyondCut; /* that surface is a cut-out (see-through texels) */
    int   blind;     /* a model was met that this cannot read */
} s_SurfQuery;

static void Surf_Tri(s_SurfQuery* q, const float* a, const float* b, const float* c, int cutout)
{
    float e1[3], e2[3], p[3], s[3], r[3], det, inv, u, v, t;
    int   i;

    for (i = 0; i < 3; i++)
    {
        e1[i] = b[i] - a[i];
        e2[i] = c[i] - a[i];
    }
    p[0] = q->d[1] * e2[2] - q->d[2] * e2[1];
    p[1] = q->d[2] * e2[0] - q->d[0] * e2[2];
    p[2] = q->d[0] * e2[1] - q->d[1] * e2[0];
    det  = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
    if (det > -1e-4f && det < 1e-4f)
    {
        return;
    }
    inv = 1.0f / det;
    for (i = 0; i < 3; i++)
    {
        s[i] = q->o[i] - a[i];
    }
    /* A little slack at the edges: a shot on a seam between two quads would
     * otherwise slip between them. */
    u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
    if (u < -0.02f || u > 1.02f)
    {
        return;
    }
    r[0] = s[1] * e1[2] - s[2] * e1[1];
    r[1] = s[2] * e1[0] - s[0] * e1[2];
    r[2] = s[0] * e1[1] - s[1] * e1[0];
    v    = (q->d[0] * r[0] + q->d[1] * r[1] + q->d[2] * r[2]) * inv;
    if (v < -0.02f || u + v > 1.04f)
    {
        return;
    }
    t = (e2[0] * r[0] + e2[1] * r[1] + e2[2] * r[2]) * inv;
    if (t >= q->len - SURF_NEAR && t <= q->len + SURF_NEAR)
    {
        if (q->nearT < 0.0f || t < q->nearT)
        {
            q->nearT = t;
        }
    }
    else if (t > q->len + SURF_NEAR && t <= q->len + SURF_BEYOND)
    {
        if (q->beyondT < 0.0f || t < q->beyondT)
        {
            q->beyondT   = t;
            q->beyondCut = cutout;
        }
    }
}

static void Surf_Model(s_SurfQuery* q, const s_ModelHeader* modelHdr, const MATRIX* mat)
{
    static float wv[256][3];
    s_MeshHeader* mesh;

    if (modelHdr == NULL)
    {
        return;
    }
    /* High-poly replacements keep their triangles elsewhere. */
    if (Pc_WideLm_IsWide(modelHdr) || modelHdr->meshHdrs == NULL || modelHdr->meshCount == 0)
    {
        q->blind = 1;
        return;
    }
    for (mesh = modelHdr->meshHdrs; mesh < &modelHdr->meshHdrs[modelHdr->meshCount]; mesh++)
    {
        const s_Primitive* prim;
        int                base = modelHdr->vertexOffset;
        int                i;

        if (mesh->primitives == NULL || mesh->verticesXy == NULL || mesh->verticesZ == NULL)
        {
            continue;
        }
        for (i = 0; i < mesh->vertexCount && base + i < 256; i++)
        {
            const float lx = (float)mesh->verticesXy[i].vx;
            const float ly = (float)mesh->verticesXy[i].vy;
            const float lz = (float)mesh->verticesZ[i];
            int         k;

            for (k = 0; k < 3; k++)
            {
                wv[base + i][k] = (mat->m[k][0] * lx + mat->m[k][1] * ly + mat->m[k][2] * lz) / 4096.0f + (float)mat->t[k];
            }
        }
        for (prim = mesh->primitives; prim < &mesh->primitives[mesh->primitiveCount]; prim++)
        {
            int vi[4];
            int k, ok = 1;

            for (k = 0; k < 4; k++)
            {
                vi[k] = prim->field_C[k];
                if (vi[k] == 0xFF)
                {
                    vi[k] = prim->field_C[2];
                }
                if (vi[k] < base || vi[k] >= base + mesh->vertexCount)
                {
                    ok = 0;
                }
            }
            if (!ok)
            {
                continue;
            }
            Surf_Tri(q, wv[vi[0]], wv[vi[1]], wv[vi[2]], prim->field_6.bits.isTransparent);
            if (vi[3] != vi[2])
            {
                Surf_Tri(q, wv[vi[1]], wv[vi[2]], wv[vi[3]], prim->field_6.bits.isTransparent);
            }
        }
    }
}

/* Maps with real panes the player can shoot at. Elsewhere a cut-out behind an
 * invisible wall is a fence or foliage as often as a window, so nothing is
 * drawn there. */
static int Surf_MapHasGlass(void)
{
    return g_SavegamePtr->mapIdx == MapIdx_MAP0_S01;
}

static int Surf_Classify(const VECTOR3* pos, const VECTOR3* origin)
{
    s_SurfQuery q;
    s_Chunk*    chunk;
    int         i;

    for (i = 0; i < 3; i++)
    {
        const float o = (float)(&origin->vx)[i] / 16.0f;
        const float p = (float)(&pos->vx)[i] / 16.0f;

        q.o[i] = o;
        q.d[i] = p - o;
    }
    q.len = sqrtf(q.d[0] * q.d[0] + q.d[1] * q.d[1] + q.d[2] * q.d[2]);
    if (q.len < 1.0f)
    {
        return SURF_SOLID;
    }
    for (i = 0; i < 3; i++)
    {
        q.d[i] /= q.len;
    }
    q.nearT     = -1.0f;
    q.beyondT   = -1.0f;
    q.beyondCut = 0;
    q.blind     = 0;

    for (chunk = g_Map.activeChunks; chunk < &g_Map.activeChunks[g_Map.activeChunkCount]; chunk++)
    {
        const s_IpdHeader* ipd = chunk->ipdHdr;
        int                b;

        if (ipd == NULL || IpdHeader_LoadStateGet(chunk) < StaticModelLoadState_Loaded ||
            !Ipd_CellPositionMatchCheck(chunk, &g_Map))
        {
            continue;
        }
        for (b = 0; b < ipd->modelBufferCount; b++)
        {
            const s_IpdModelBuffer*   buf = &ipd->modelBuffers[b];
            const s_IpdModelInstance* inst;

            for (inst = buf->modelInstances; inst < &buf->modelInstances[buf->modelInstanceCount]; inst++)
            {
                MATRIX mat = inst->mat;

                mat.t[0] += ipd->cellX * Q12_TO_Q8(CHUNK_CELL_SIZE);
                mat.t[2] += ipd->cellZ * Q12_TO_Q8(CHUNK_CELL_SIZE);
                Surf_Model(&q, inst->modelHdr, &mat);
            }
        }
    }
    for (i = 0; i < s_objLastCount; i++)
    {
        Surf_Model(&q, s_objLast[i].modelHdr, &s_objLast[i].mat);
    }

    SH_DBG("[DECAL] shot %.0f long: drawn surface at %.0f, next past it %.0f%s%s", q.len, q.nearT, q.beyondT,
           q.beyondCut ? " (cut-out)" : "", q.blind ? ", unreadable models present" : "");
    if (q.nearT >= 0.0f)
    {
        return SURF_SOLID;
    }
    if (Surf_MapHasGlass() && q.beyondT >= 0.0f && q.beyondCut)
    {
        return SURF_GLASS;
    }
    /* Unreadable geometry in the scene: say solid, as before this test. */
    return q.blind ? SURF_SOLID : SURF_NONE;
}

static s_PcDecal s_decals[DECAL_MAX];
extern s_WorldEnvWork g_WorldEnvWork;

static int       s_decalCount = 0;
static int       s_decalHead  = 0; /* FIFO write index (oldest overwritten) */

static unsigned char* s_rgba          = NULL; /* decoded PNG, cached for re-registration after pool resets */
static int            s_rgbaW         = 0;
static int            s_rgbaH         = 0;
static int            s_texRegistered = 0;
static int            s_texMissing    = 0; /* decal.png absent/broken — logged once, feature off */
static unsigned char* s_glassRgba     = NULL; /* decalGlass.png: the slot's second row */
static int            s_glassW        = 0;
static int            s_glassH        = 0;
static int            s_glassMissing  = 0;

void Pc_DecalAddBulletImpact(const VECTOR3* pos, const VECTOR3* dir, const VECTOR3* origin)
{
    s_CollisionSurface surf;
    s_PcDecal          d;

    if (!g_PcConfig.bulletDecals || s_texMissing)
    {
        return;
    }

    d.center = *pos;
    switch (Surf_Classify(pos, origin))
    {
        case SURF_NONE:
            return;
        case SURF_GLASS:
            if (s_glassMissing)
            {
                return;
            }
            d.glass = 1;
            break;
        default:
            d.glass = 0;
            break;
    }

    Collision_SurfaceGet(&surf, d.center.vx, d.center.vz);
    if (ABS(d.center.vy - surf.groundHeight) < DECAL_FLOOR_TOL)
    {
        /* FLOOR: quad flat in the XZ plane, lifted slightly (PSX -Y = up). */
        d.center.vy = surf.groundHeight - DECAL_OFFSET;
        d.axisU.vx  = DECAL_HALF;
        d.axisU.vy  = 0;
        d.axisU.vz  = 0;
        d.axisV.vx  = 0;
        d.axisV.vy  = 0;
        d.axisV.vz  = DECAL_HALF;
    }
    else
    {
        /* WALL: SH1 collision carries no surface normals, so sample the wall
         * plane with two parallel rays offset sideways from the shot — their
         * clipped hit points span the wall's XZ heading, so the decal lies
         * flat ON the wall instead of tilting to face the shooter (which read
         * wrong on angled shots). Falls back to shooter-facing when a sample
         * ray misses or lands on a different surface. */
        float fx  = (float)dir->vx;
        float fz  = (float)dir->vz;
        float mag = sqrtf(fx * fx + fz * fz);
        s32   nx;
        s32   nz;
        s32   planeOk = 0;

        if (mag < 1.0f)
        {
            return; /* near-vertical shot; no wall heading to face */
        }

        /* Unit shot heading (XZ) and its perpendicular, Q12. */
        nx = (s32)(-fx * 4096.0f / mag);
        nz = (s32)(-fz * 4096.0f / mag);

        {
            s32        px = Q12_MULT(-nz, Q12(0.25f)); /* sideways sample offset */
            s32        pz = Q12_MULT(nx, Q12(0.25f));
            s_RayTrace tr[2];
            VECTOR3    h[2];
            int        s;

            for (s = 0; s < 2; s++)
            {
                s32     sgn = s ? -1 : 1;
                VECTOR3 from;
                VECTOR3 to;

                from.vx = origin->vx + sgn * px;
                from.vy = origin->vy;
                from.vz = origin->vz + sgn * pz;
                /* Aim past the wall so the sample definitely reaches it. */
                to.vx = pos->vx + sgn * px - Q12_MULT(nx, Q12(0.5f));
                to.vy = pos->vy;
                to.vz = pos->vz + sgn * pz - Q12_MULT(nz, Q12(0.5f));

                if (!Ray_TraceQuery(&tr[s], &from, &to) || tr[s].character != NULL)
                {
                    break;
                }
                h[s] = tr[s].target;
            }

            if (s == 2)
            {
                float wx = (float)(h[1].vx - h[0].vx);
                float wz = (float)(h[1].vz - h[0].vz);
                float wm = sqrtf(wx * wx + wz * wz);

                /* Degenerate span (corner/edge hit) -> keep the fallback. */
                if (wm > 512.0f)
                {
                    s32 wnx = (s32)(-wz * 4096.0f / wm);
                    s32 wnz = (s32)(wx * 4096.0f / wm);

                    /* Orient the wall normal back toward the shooter. */
                    if (Q12_MULT(wnx, nx) + Q12_MULT(wnz, nz) < 0)
                    {
                        wnx = -wnx;
                        wnz = -wnz;
                    }
                    nx      = wnx;
                    nz      = wnz;
                    planeOk = 1;
                }
            }
        }
        (void)planeOk;

        d.center.vx += Q12_MULT(nx, DECAL_OFFSET);
        d.center.vz += Q12_MULT(nz, DECAL_OFFSET);

        d.axisU.vx = Q12_MULT(nz, DECAL_HALF); /* horizontal, along the wall */
        d.axisU.vy = 0;
        d.axisU.vz = Q12_MULT(-nx, DECAL_HALF);
        d.axisV.vx = 0;
        d.axisV.vy = DECAL_HALF; /* vertical */
        d.axisV.vz = 0;
    }

    s_decals[s_decalHead] = d;
    s_decalHead           = (s_decalHead + 1) % DECAL_MAX;
    if (s_decalCount < DECAL_MAX)
    {
        s_decalCount++;
    }
}

/* Register the PNG into the reserved pool slot. Called from the draw path so
 * a GL context exists; re-registers after every Pc_DecalsReset because
 * HiresOverride_PoolSlotsReset frees the slot's GL texture on map init. */
static int Pc_DecalEnsureTexture(void)
{
    if (s_texRegistered)
    {
        return 1;
    }
    if (s_texMissing)
    {
        return 0;
    }

    if (s_rgba == NULL)
    {
        FILE*          f;
        long           size;
        unsigned char* buf;
        int            comp = 0;

        f = fopen("gamedata/image/decal.png", "rb");
        /* An install updated exe-only still has it where it used to be. */
        if (f == NULL)
        {
            f = fopen("gamedata/decal.png", "rb");
        }
        if (f == NULL)
        {
            SH_DBG("[DECAL] gamedata/image/decal.png not found — bullet decals disabled");
            s_texMissing = 1;
            return 0;
        }
        fseek(f, 0, SEEK_END);
        size = ftell(f);
        fseek(f, 0, SEEK_SET);
        buf = (size > 0) ? (unsigned char*)malloc((size_t)size) : NULL;
        if (buf == NULL || fread(buf, 1, (size_t)size, f) != (size_t)size)
        {
            fclose(f);
            free(buf);
            SH_DBG("[DECAL] gamedata/image/decal.png unreadable — bullet decals disabled");
            s_texMissing = 1;
            return 0;
        }
        fclose(f);

        s_rgba = stbi_load_from_memory(buf, (int)size, &s_rgbaW, &s_rgbaH, &comp, 4);
        free(buf);
        if (s_rgba == NULL || s_rgbaW <= 0 || s_rgbaH <= 0)
        {
            SH_DBG("[DECAL] gamedata/image/decal.png decode failed: %s — bullet decals disabled",
                   stbi_failure_reason());
            s_texMissing = 1;
            return 0;
        }
    }

    if (HiresOverride_PoolSlotRegisterRGBA(DECAL_POOL_SLOT, 0, s_rgba, s_rgbaW, s_rgbaH,
                                           DECAL_NATIVE, DECAL_NATIVE) != 0)
    {
        return 0;
    }

    if (s_glassRgba == NULL && !s_glassMissing)
    {
        FILE* f = fopen("gamedata/image/decalGlass.png", "rb");

        s_glassMissing = 1;
        if (f != NULL)
        {
            long           size;
            unsigned char* buf;
            int            comp = 0;

            fseek(f, 0, SEEK_END);
            size = ftell(f);
            fseek(f, 0, SEEK_SET);
            buf = (size > 0) ? (unsigned char*)malloc((size_t)size) : NULL;
            if (buf != NULL && fread(buf, 1, (size_t)size, f) == (size_t)size)
            {
                s_glassRgba = stbi_load_from_memory(buf, (int)size, &s_glassW, &s_glassH, &comp, 4);
            }
            free(buf);
            fclose(f);
            s_glassMissing = s_glassRgba == NULL;
        }
        if (s_glassMissing)
        {
            SH_DBG("[DECAL] gamedata/image/decalGlass.png missing or unreadable - no decals on glass");
        }
    }
    if (s_glassRgba != NULL)
    {
        HiresOverride_PoolSlotRegisterRGBA(DECAL_POOL_SLOT, 1, s_glassRgba, s_glassW, s_glassH, DECAL_NATIVE, DECAL_NATIVE);
    }

    SH_DBG("[DECAL] decal.png %dx%d registered in pool slot %d (clut=0x%04X)",
           s_rgbaW, s_rgbaH, DECAL_POOL_SLOT, (unsigned)DECAL_CLUT);
    s_texRegistered = 1;
    return 1;
}

void Pc_DecalsDraw(GsOT* ot)
{
    POLY_FT4* poly;
    int       i;

    /* Every world object of this frame has been drawn by now. */
    memcpy(s_objLast, s_objCur, sizeof(s_objCur[0]) * (size_t)s_objCurCount);
    s_objLastCount = s_objCurCount;
    s_objCurCount  = 0;

    if (!g_PcConfig.bulletDecals || s_decalCount == 0)
    {
        return;
    }
    if (!Pc_DecalEnsureTexture())
    {
        return;
    }

    poly = (POLY_FT4*)GsOUT_PACKET_P;

    for (i = 0; i < s_decalCount; i++)
    {
        const s_PcDecal* d = &s_decals[i];
        MATRIX           mat;
        SVECTOR          v;
        s16              sx[4];
        s16              sy[4];
        s32              bucketSum = 0;
        s32              bucket;
        int              k;
        int              ok = 1;
        int              decalKeep = 256;

        Vw_WorldScreenMatrixAtPositionGet(&mat, d->center.vx, d->center.vy, d->center.vz);
        SetRotMatrix(&mat);
        SetTransMatrix(&mat);

        for (k = 0; k < 4; k++)
        {
            /* PSX quad Z-order: 0=TL 1=TR 2=BL 3=BR -> center -/+tan -/+bitan.
             * Corner offsets in Q8 world units — the space the WS matrix
             * translation lives in (see func_8005E89C / blood splats). */
            s32  st  = (k & 1) ? 1 : -1;
            s32  sb  = (k & 2) ? 1 : -1;
            s32  otz;
            s32  sxy;
            long p;
            long flag;

            v.vx  = (s16)Q12_TO_Q8(st * d->axisU.vx + sb * d->axisV.vx);
            v.vy  = (s16)Q12_TO_Q8(st * d->axisU.vy + sb * d->axisV.vy);
            v.vz  = (s16)Q12_TO_Q8(st * d->axisU.vz + sb * d->axisV.vz);
            v.pad = 0;

            otz = RotTransPers(&v, (int*)&sxy, &p, &flag);
            if (otz <= 8) /* behind/at the near plane */
            {
                ok = 0;
                break;
            }

            sx[k] = (s16)(sxy & 0xFFFF);
            sy[k] = (s16)((u32)sxy >> 16);
            if (ABS(sx[k]) > 2048 || ABS(sy[k]) > 2048)
            {
                ok = 0;
                break;
            }

            /* Blood-splat bucket source: RAW RotTransPers SZ (func_80062708
             * field_20C = average of the 4 corner SZ; no <<2 inflation). */
            bucketSum += otz;
        }

        if (!ok)
        {
            continue;
        }

        /* Bucket on the SAME scale as everything else in OT0, which is what the
         * blood-splat copy got wrong: blood's >>3 produced an index ~4x too
         * SMALL, and a smaller index is NEARER, so the decal sorted in front of
         * a character standing between the camera and the wall and painted over
         * him. Semi-transparency stops it winning the depth test; it does not
         * stop it winning painter order. */
        bucket = (bucketSum >> 2) >> DECAL_OT_SHIFT;
        bucket -= DECAL_OT_LIFT;
        if (bucket < 1)
        {
            /* The game's own sorters drop otz <= 0 outright, so never hand
             * DrawOTag bucket 0. */
            bucket = 1;
        }
        if (bucket >= ORDERING_TABLE_SIZE)
        {
            bucket = ORDERING_TABLE_SIZE - 1;
        }

        /* Fully fogged: stop drawing entirely, the way PSX drops world polys
         * past the fog far plane. The bullet-hole texture is DARK, and an
         * average-blended dark texture darkens the frame no matter what the
         * vertex colour is mixed toward -- (fog + 0.2*fog)/2 is still below the
         * fog -- so past the point where fog should have swallowed the hole,
         * colour maths cannot hide it. Skipping is the only correct occlusion. */
        {
            /* The keep must track what the fog LOOKS like, not the raw ramp:
             * the shader multiplies fog by g_PsyX_FogStrength (typically >1),
             * so the scene whites out faster than the unscaled ramp says. An
             * unscaled fade left the decal outliving the wall it sits on -- a
             * lone dark dot in blank fog. Scale the fade by the same strength,
             * then skip once it is nearly gone. */
            extern int   Pc_BloodFogKeep(s32 z);
            extern float g_PsyX_FogStrength;
            /* bucketSum accumulates RotTransPers RETURN values, and RotTransPers
             * returns OTZ = SZ >> 2 -- so bucketSum >> 2 is a QUARTER of the true
             * average depth, and the keep was being evaluated as if every decal
             * were four times closer than it is: barely any fade at any range,
             * which is why the previous two fixes changed nothing visible. The
             * sum of the four quartered corners IS the average SZ, so the ramp
             * gets bucketSum itself. (The OT bucketing below is unaffected: its
             * >>1 shift was calibrated against the quartered otz.) */
            int fade = 256 - Pc_BloodFogKeep(bucketSum);

            fade = (int)(fade * (g_PsyX_FogStrength > 0.0f ? g_PsyX_FogStrength : 1.0f));
            if (fade > 256) fade = 256;
            decalKeep = 256 - fade;

            if (decalKeep < 64)
                continue;
        }

        setPolyFT4(poly);
        /* Semi-transparent like the blood splats (setSemiTrans in func_80062708):
         * the decal then never depth-tests, so a character/object in front covers
         * it in painter order instead of the decal punching through. The override
         * shader still alpha-discards the PNG's transparent texels, so the hole
         * shape is preserved — only the surviving pixels blend. */
        setSemiTrans(poly, true);
        {
            /* Take the room's ambient like billboards do (worldTintColor
             * scaled by the ambient level) so decals sit in the scene's
             * lighting instead of rendering full-bright everywhere. Floor of
             * 12 keeps them findable in pitch-black flashlight rooms. */
            s32 lr = Q12_MULT(g_WorldEnvWork.worldTintColor.r, g_WorldEnvWork.field_20);
            s32 lg = Q12_MULT(g_WorldEnvWork.worldTintColor.g, g_WorldEnvWork.field_20);
            s32 lb = Q12_MULT(g_WorldEnvWork.worldTintColor.b, g_WorldEnvWork.field_20);
            if (lr < 12) lr = 12;
            if (lg < 12) lg = 12;
            if (lb < 12) lb = 12;
            if (lr > 128) lr = 128;
            if (lg > 128) lg = 128;
            if (lb > 128) lb = 128;
            /* Fade with world fog, exactly as the blood prims do. A decal is
             * semi-transparent, so it disappears as its source colour goes to
             * zero -- without this a bullet hole stayed at full strength however
             * far away or however thick the fog, standing out against a wall
             * that had already faded. bucketSum >> 2 is the average corner SZ,
             * the same depth measure blood passes in. */
            {
                /* True transparency fade. Neither colour direction works for an
                 * average-blended DARK texture: toward zero it half-darkens
                 * forever (the black hole), toward the fog colour it still
                 * darkens because the texture itself is dark. BM_AVERAGE is
                 * genuine SRC_ALPHA blending on PC, so the per-prim alpha
                 * channel fades the decal to actually invisible, with the art
                 * and the near look untouched. decalKeep is the strength-scaled
                 * keep computed at the skip above, so the fade tracks the fog
                 * as it is actually rendered. */
                extern void PsyX_SetNextPrimAlpha(int a);

                PsyX_SetNextPrimAlpha(decalKeep);
            }

            setRGB0(poly, (u8)lr, (u8)lg, (u8)lb);
        }
        /* tpage is irrelevant: the bit-15 clut alone keys the GL override
         * (ApplyHiresOverride pool-slot fast path); page bits never sample VRAM. */
        poly->tpage = 0;
        /* The pane's image is the slot's second palette row. */
        poly->clut  = (u16)(DECAL_CLUT + (d->glass ? (1 << 6) : 0));
        poly->u0    = 0;
        poly->v0    = 0;
        poly->u1    = DECAL_UV_MAX;
        poly->v1    = 0;
        poly->u2    = 0;
        poly->v2    = DECAL_UV_MAX;
        poly->u3    = DECAL_UV_MAX;
        poly->v3    = DECAL_UV_MAX;
        setXY4(poly, sx[0], sy[0], sx[1], sy[1], sx[2], sy[2], sx[3], sy[3]);

        addPrim(&ot->org[bucket], poly);
        poly++;
    }

    GsOUT_PACKET_P = (PACKET*)poly;
}

void Pc_DecalsReset(void)
{
    s_decalCount    = 0;
    s_decalHead     = 0;
    s_texRegistered = 0; /* HiresOverride_PoolSlotsReset freed the slot texture */
    /* Their models belong to the map being left. */
    s_objCurCount  = 0;
    s_objLastCount = 0;
}
