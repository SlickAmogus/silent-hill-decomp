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

#include "bodyprog/collision/collision.h"
#include "bodyprog/collision/ray.h"
#include "bodyprog/math/math.h"
#include "bodyprog/screen/screen_data.h"
#include "bodyprog/gfx/world.h"
#include "bodyprog/view/vw_calc.h"

#include "hires_override.h"
#include "pc_config.h"
#include "pc_decals.h"
#include "sh_log.h"
#include "stb_image.h"

#define DECAL_MAX       64
#define DECAL_HALF      Q12(0.05f) /* quad half-extent (~5cm) */
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

/* Depth handling mirrors the game's own blood-splat drawer (func_80062708):
 * accumulate RAW RotTransPers SZ (no <<2), push one blood-bias step toward FAR
 * (blood's var_s7 = 256), and draw the decal SEMI-TRANSPARENT.
 *
 * The semi-transparency is the load-bearing part: like blood, the decal then
 * never depth-tests, so it CANNOT win the depth buffer against a character
 * standing in front of the wall — it just blends onto the scene and anything
 * drawn nearer paints over it (no more see-through-objects). The far push keeps
 * it from over-drawing foreground in painter order; the DECAL_OFFSET normal lift
 * keeps it in front of its own host surface. This replaces the previous OPAQUE
 * quad pulled one bucket FORWARD, which won the depth test against — and drew
 * over — characters/objects near the wall (the reported bug). Bump toward 0 if a
 * decal dips behind a grazing wall. */
#define DECAL_BLOOD_BIAS 256 /* == blood-splat var_s7 (func_80062708) */

typedef struct {
    VECTOR3 center; /* Q19.12 world, already offset along the normal */
    VECTOR3 axisU;  /* Q12 half-extent tangent */
    VECTOR3 axisV;  /* Q12 half-extent bitangent */
} s_PcDecal;

static s_PcDecal s_decals[DECAL_MAX];
extern s_WorldEnvWork g_WorldEnvWork;

static int       s_decalCount = 0;
static int       s_decalHead  = 0; /* FIFO write index (oldest overwritten) */

static unsigned char* s_rgba          = NULL; /* decoded PNG, cached for re-registration after pool resets */
static int            s_rgbaW         = 0;
static int            s_rgbaH         = 0;
static int            s_texRegistered = 0;
static int            s_texMissing    = 0; /* decal.png absent/broken — logged once, feature off */

void Pc_DecalAddBulletImpact(const VECTOR3* pos, const VECTOR3* dir, const VECTOR3* origin)
{
    s_CollisionSurface surf;
    s_PcDecal          d;

    if (!g_PcConfig.bulletDecals || s_texMissing)
    {
        return;
    }

    d.center = *pos;

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

        f = fopen("gamedata/decal.png", "rb");
        if (f == NULL)
        {
            SH_DBG("[DECAL] gamedata/decal.png not found — bullet decals disabled");
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
            SH_DBG("[DECAL] gamedata/decal.png unreadable — bullet decals disabled");
            s_texMissing = 1;
            return 0;
        }
        fclose(f);

        s_rgba = stbi_load_from_memory(buf, (int)size, &s_rgbaW, &s_rgbaH, &comp, 4);
        free(buf);
        if (s_rgba == NULL || s_rgbaW <= 0 || s_rgbaH <= 0)
        {
            SH_DBG("[DECAL] gamedata/decal.png decode failed: %s — bullet decals disabled",
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

    SH_DBG("[DECAL] decal.png %dx%d registered in pool slot %d (clut=0x%04X)",
           s_rgbaW, s_rgbaH, DECAL_POOL_SLOT, (unsigned)DECAL_CLUT);
    s_texRegistered = 1;
    return 1;
}

void Pc_DecalsDraw(GsOT* ot)
{
    POLY_FT4* poly;
    int       i;

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

        /* Blood-splat bucketing (func_80062708): (avgSZ + var_s7) >> 3. The far
         * push (DECAL_BLOOD_BIAS) is what lets foreground paint over the decal. */
        bucket = ((bucketSum >> 2) + DECAL_BLOOD_BIAS) >> 3;
        if (bucket < 0)
        {
            bucket = 0;
        }
        if (bucket >= ORDERING_TABLE_SIZE)
        {
            bucket = ORDERING_TABLE_SIZE - 1;
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
            setRGB0(poly, (u8)lr, (u8)lg, (u8)lb);
        }
        /* tpage is irrelevant: the bit-15 clut alone keys the GL override
         * (ApplyHiresOverride pool-slot fast path); page bits never sample VRAM. */
        poly->tpage = 0;
        poly->clut  = DECAL_CLUT;
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
}
