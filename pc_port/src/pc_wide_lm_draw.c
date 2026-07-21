/* v7 high-poly ILM — wide drawer.
 *
 * A faithful clone of the stock LIT character chain (func_8005A21C: the
 * func_8005A900 transform, func_8005AA08 shade, func_8005AC50 emit) with the
 * fixed-size u8 GTE scratch pool REMOVED. A stock walker addresses at most 256
 * verts/normals through the scratchpad; a dense modded part can carry
 * thousands, so each mesh transforms into malloc'd per-vertex buffers indexed
 * [0..count) instead. Stock models never reach here — Pc_WideLm_IsWide is false
 * for every v6 file, so func_80057090 keeps its unchanged dispatch — and the
 * whole subsystem lives in pc_port/, absent from the PSX/decomp build.
 *
 * Scope for this stage: the lit chain only (validator rejects v7 parts whose
 * field_B_0/field_B_4 select the flat or third chains), affine (no PGXP
 * perspective). Fog, shade, backface, PGXP and the F1 packet-arena bound are
 * all carried across from the stock emit unchanged. */

#include "game.h"
#include "inline_no_dmpsx.h"

#include <psyq/gtemac.h>

#include "bodyprog/bodyprog.h"
#include "bodyprog/math/arithmetic.h"
#include "bodyprog/gfx/world.h"

#include <stdlib.h>

#include "pc_config.h"
#include "pc_wide_lm.h"
#include "sh_log.h"

/* Writable view of g_WorldEnvWork. The drawer sets field_14C exactly as the
 * stock lit dispatch does (func_80057090:1781); the symbol is const in
 * read-only TUs but writable in its defining TU and in bodyprog_80040B74.c —
 * this declaration matches the latter. */
extern s_WorldEnvWork g_WorldEnvWork;

/* func_80057090:1776 light-vector setup. No prototype in a shared header. */
extern void func_80057228(MATRIX* mat, s32 alpha, SVECTOR* arg2, VECTOR3* arg3);

/* F1: end of the active GsOUT_PACKET_P arena (game_main.c). */
extern PACKET* Pc_PacketBuf_End(void);

extern int PsyX_PGXP_TriBackface(const void* a, const void* b, const void* c, int nclip);
extern int PsyX_PGXP_QuadBackface(const void* a, const void* b, const void* c, const void* d, int n0, int n1);

/* ---- Macros cloned verbatim from func_8005AC50's file-local set
 *      (bodyprog_80055028.c). They are not exported in any header. ---- */

/* Emit far-distance cap (:32 PC form). */
#define WIDE_FOG_FAR_DIST() (g_PcConfig.disableCulling ? 0x7FFFFFFF : g_WorldEnvWork.fog.farDistance)

/* :4024-4027 — PsyCross does not clip huge polys; drop any vertex far off-screen. */
#define WIDE_SCREEN_BOUND 1024
#define WIDE_VERTEX_OOB(packed) \
    ((s16)((packed) & 0xFFFF) < -WIDE_SCREEN_BOUND || (s16)((packed) & 0xFFFF) > WIDE_SCREEN_BOUND || \
     (s16)(((packed) >> 16) & 0xFFFF) < -WIDE_SCREEN_BOUND || (s16)(((packed) >> 16) & 0xFFFF) > WIDE_SCREEN_BOUND)

/* :44-48 — keep the OT bucket inside org[ORDERING_TABLE_SIZE]. */
#define WIDE_CLAMP_OT_DEPTH(depth, shift)                                       \
    do {                                                                        \
        if ((((depth) >> (shift)) >> 2) >= ORDERING_TABLE_SIZE)                 \
            (depth) = (ORDERING_TABLE_SIZE - 1) << ((shift) + 2);               \
    } while (0)

/* :57-72 — whole-map mode far lift + wrapped-depth rescue (no-ops when off). */
#define WIDE_WHOLEMAP_FARCAP(cap)                                               \
    do { if (Pc_WholeMapDrawActive()) (cap) = 0x7FFFFFFF; } while (0)
#define WIDE_WHOLEMAP_DEPTH_RESCUE(depth, shift)                                \
    do {                                                                        \
        if ((depth) < 0 && Pc_WholeMapDrawActive())                            \
            (depth) = (ORDERING_TABLE_SIZE - 1) << ((shift) + 2);              \
    } while (0)

/* :132 — per-vertex fog factor (0-127) from a single screen Z. */
#define WIDE_SCREEN_Z_TO_FOG(z) ({ \
    s32 _z = (s32)(z); s32 _fb; \
    if (!g_WorldEnvWork.isFogEnabled) { _fb = 0; } \
    else if (_z < (1 << g_WorldEnvWork.fog.depthShift)) { \
        s32 _idx = (_z << 7) >> g_WorldEnvWork.fog.depthShift; \
        if (_idx < 0) { _idx = 0; } \
        if (_idx > 127) { _idx = 127; } \
        _fb = g_WorldEnvWork.fogRamp[_idx]; \
    } else { _fb = 255; } \
    (u8)((_fb * 127) >> 8); })

/* func_8005AA08's file-local gte_strgb3_vec (PC form): store the previous nct's
 * three packed-RGB results into a VECTOR3-strided slot. */
#define WIDE_STRGB3_VEC(r0) do {                     \
        *(u32*)((char*)(r0) + 0) = MFC2(20);         \
        *(u32*)((char*)(r0) + 4) = MFC2(21);         \
        *(u32*)((char*)(r0) + 8) = MFC2(22);         \
    } while (0)

#define WIDE_CLAMP_IDX(i, cnt) ((i) < (cnt) ? (i) : (cnt) - 1)

typedef union
{
    POLY_GT3* gt3;
    POLY_GT4* gt4;
    PACKET*   packet;
} u_widePoly;

/* Per-part scratch, grown to high-water and kept for the process. The GTE
 * batches 3 verts/normals at a time and writes up to 2 slots past the real
 * count, so every buffer carries +2 slack (the slack the stock scratch pool
 * carried inside its declared window). */
static DVECTOR* s_wideXy;
static s16*     s_wideZ;
static s32*     s_wideShade;
static u32      s_wideVertCap;
static u32      s_wideShadeCap;

static int WideBufs_Ensure(u32 vertCount, u32 normCount)
{
    if (vertCount > s_wideVertCap)
    {
        u32      cap = vertCount + (vertCount >> 1) + 16;
        DVECTOR* nxy = (DVECTOR*)realloc(s_wideXy, ((size_t)cap + 2) * sizeof(DVECTOR));
        s16*     nz  = (s16*)realloc(s_wideZ, ((size_t)cap + 2) * sizeof(s16));

        if (nxy != NULL) { s_wideXy = nxy; }
        if (nz != NULL)  { s_wideZ = nz; }
        if (nxy == NULL || nz == NULL)
        {
            return 0;
        }
        s_wideVertCap = cap;
    }
    if (normCount > s_wideShadeCap)
    {
        u32  cap = normCount + (normCount >> 1) + 16;
        s32* ns  = (s32*)realloc(s_wideShade, ((size_t)cap + 2) * sizeof(s32));

        if (ns == NULL)
        {
            return 0;
        }
        s_wideShade    = ns;
        s_wideShadeCap = cap;
    }
    return s_wideXy != NULL && s_wideZ != NULL && s_wideShade != NULL;
}

/* Clone of func_8005A900 with the pool removed: project every vertex into
 * xy[0..count) / z[0..count). Verts are interleaved bone-local SVECTORs, so a
 * gte_ldv3c triple load replaces the stock split DVECTOR-XY/u16-Z load; the
 * store keeps the stock gte_FetchScreen0_1_2_XYZ (XY word + Z halfword) so the
 * output layout — and its PGXP shadow store, keyed by &xy[i] — is identical.
 * The 3-at-a-time source read is staged through a clamped local triple so it
 * never runs past the exactly-sized verts[] allocation. */
static void Pc_WideLm_Transform(const s_WideMesh* mesh, MATRIX* mat, DVECTOR* xy, s16* z)
{
    u32 vc = mesh->vertexCount;
    u32 i;

    if (vc == 0)
    {
        return;
    }

    SetRotMatrix(mat);
    SetTransMatrix(mat);

    for (i = 0; i < vc; i += 3)
    {
        SVECTOR tri[3];
        u32     j;

        for (j = 0; j < 3; j++)
        {
            tri[j] = mesh->verts[WIDE_CLAMP_IDX(i + j, vc)];
        }
        gte_ldv3c(tri);
        gte_rtpt();
        gte_FetchScreen0_1_2_XYZ(&xy[i], &z[i]);
    }
}

/* Clone of func_8005AA08 (arg1 == 0): pack one RGB word per normal into
 * shade[0..count). Wide normals are SVECTORs holding the original s8-range
 * components, so the stock nx<<5 scaling is reproduced exactly. The 3-at-a-time
 * source is clamped so it never runs past the exactly-sized normals[]. */
static void Pc_WideLm_Shade(const s_WideMesh* mesh, s_GteScratchData2* env, s32* shade)
{
    CVECTOR  sp0;
    u32      nc = mesh->normalCount;
    u32      groups;
    u32      g;
    VECTOR3* out;

    if (nc == 0)
    {
        return;
    }

    sp0.cd = 0;
    gte_ldrgb(&sp0);

    groups = (nc + 2) / 3;

    for (g = 0; g < groups; g++)
    {
        u32 j;

        for (j = 0; j < 3; j++)
        {
            const SVECTOR* s = &mesh->normals[WIDE_CLAMP_IDX(g * 3 + j, nc)];

            env->u.normal.field_3E0[j].vx = (s16)(s->vx << 5);
            env->u.normal.field_3E0[j].vy = (s16)(s->vy << 5);
            env->u.normal.field_3E0[j].vz = (s16)(s->vz << 5);
        }

        if (g != 0)
        {
            WIDE_STRGB3_VEC((VECTOR3*)&shade[(g - 1) * 3]); /* store previous nct */
        }
        gte_ldv3c(env->u.normal.field_3E0);
        gte_nct();
    }

    out = (VECTOR3*)&shade[(groups - 1) * 3];
    gte_strgb3(&out->vx, &out->vy, &out->vz); /* final nct */
}

/* Clone of func_8005AC50 with u32 corners. Vertex corners (corner[]) index
 * xy[]/z[]; shade corners (normal[]) index shade[]. Material is already baked
 * into uv[]/clut/flags at parse. Returns 1 when the packet arena is exhausted
 * so the caller stops submitting. */
static int Pc_WideLm_Emit(const s_WideMesh* mesh, s_GteScratchData2* env, GsOT_TAG* ot, s32 arg2,
                          const DVECTOR* xy, const s16* z, const s32* shade, s32 var_a3, u32 uvTpage,
                          s32 farcap, u8* pktEnd)
{
    static int  warned = 0;
    u_widePoly  poly;
    u32         pi;
    int         exhausted = 0;

    poly.packet = GsOUT_PACKET_P;

    for (pi = 0; pi < mesh->primCount; pi++)
    {
        const s_WidePrim* prim = &mesh->prims[pi];
        u32               c0   = prim->corner[0];
        u32               c1   = prim->corner[1];
        u32               c2   = prim->corner[2];
        u32               c3   = prim->corner[3];
        s32               depth;

        /* F1 (MANDATORY): guard the arena before every addPrim. GT4 >= GT3, so
         * one check at loop top covers whichever prim this iteration emits. */
        if ((u8*)poly.packet + sizeof(POLY_GT4) > pktEnd)
        {
            if (!warned)
            {
                SH_DBG("[WIDELM] packet arena exhausted; dropping remaining wide prims");
                warned = 1;
            }
            exhausted = 1;
            break;
        }

        if (c3 == 0xFFFFFFFFu) /* triangle — :4093 */
        {
            s32 r4;

            depth = (z[c0] + z[c1] + z[c2] + z[c2]) >> 2; /* :4095 */
            WIDE_WHOLEMAP_DEPTH_RESCUE(depth, arg2);
            if (depth <= 0 || farcap < depth)
            {
                continue;
            }

            gte_NormalClip(*(s32*)&xy[c0], *(s32*)&xy[c1], *(s32*)&xy[c2], &r4); /* :4107 */
            if (PsyX_PGXP_TriBackface(&xy[c0], &xy[c1], &xy[c2], r4)) /* :4122 */
            {
                continue;
            }

            *(s32*)&poly.gt3->x0 = *(s32*)&xy[c0]; /* :4135 */
            *(s32*)&poly.gt3->x1 = *(s32*)&xy[c1];
            *(s32*)&poly.gt3->x2 = *(s32*)&xy[c2];
            if (WIDE_VERTEX_OOB(*(s32*)&poly.gt3->x0) || WIDE_VERTEX_OOB(*(s32*)&poly.gt3->x1) ||
                WIDE_VERTEX_OOB(*(s32*)&poly.gt3->x2))
            {
                continue;
            }

            Shadow_Copy(&poly.gt3->x0, &xy[c0]); /* :4144 SH_PGXP_PROP3 */
            Shadow_Copy(&poly.gt3->x1, &xy[c1]);
            Shadow_Copy(&poly.gt3->x2, &xy[c2]);

            if (var_a3 != 0) /* :4148 */
            {
                *(s32*)&poly.gt3->r0 = shade[prim->normal[0]];
                *(s32*)&poly.gt3->r1 = shade[prim->normal[1]];
                *(s32*)&poly.gt3->r2 = shade[prim->normal[2]];
            }
            else
            {
                *(s32*)&poly.gt3->r0 = *(s32*)&poly.gt3->r1 = *(s32*)&poly.gt3->r2 = *(s32*)&env->field_3D8;
            }

            poly.gt3->code = ((prim->flags >> 15) * 2) | 0x34; /* :4159 */

            *(s32*)&poly.gt3->u0 = (s32)(((u32)prim->uv[0] | ((u32)prim->clut << 16)) + uvTpage); /* :4161 */
            *(s32*)&poly.gt3->u1 = (s32)(((u32)prim->uv[1] | ((u32)prim->flags << 16)) & 0xFFFFFF); /* :4162 */
            *(u16*)&poly.gt3->u2 = prim->uv[2];                                                     /* :4163 */

            poly.gt3->p1 = WIDE_SCREEN_Z_TO_FOG(z[c1]); /* :4170 */
            poly.gt3->p2 = WIDE_SCREEN_Z_TO_FOG(z[c2]);

            setlen(poly.gt3, 9);
            WIDE_CLAMP_OT_DEPTH(depth, arg2);
            addPrim(&ot[(depth >> arg2) >> 2], poly.gt3); /* :4177 */
            poly.gt3++;
        }
        else /* quad — :4183 */
        {
            s32 sp4  = 0;
            s32 sp4b = 0;

            depth = (z[c0] + z[c1] + z[c2] + z[c3]) >> 2; /* :4185 */
            WIDE_WHOLEMAP_DEPTH_RESCUE(depth, arg2);
            if (depth <= 0 || farcap < depth)
            {
                continue;
            }

            gte_ldsxy3(*(s32*)&xy[c0], *(s32*)&xy[c1], *(s32*)&xy[c2]); /* :4197 */
            gte_nclip();
            gte_stopz(&sp4);
            gte_ldsxy0(*(s32*)&xy[c3]); /* :4209 */
            gte_nclip();
            gte_stopz(&sp4b);
            if (PsyX_PGXP_QuadBackface(&xy[c0], &xy[c1], &xy[c2], &xy[c3], sp4, sp4b)) /* :4212 */
            {
                continue;
            }

            *(s32*)&poly.gt4->x0 = *(s32*)&xy[c0]; /* :4232 */
            *(s32*)&poly.gt4->x1 = *(s32*)&xy[c1];
            *(s32*)&poly.gt4->x2 = *(s32*)&xy[c2];
            *(s32*)&poly.gt4->x3 = *(s32*)&xy[c3];
            if (WIDE_VERTEX_OOB(*(s32*)&poly.gt4->x0) || WIDE_VERTEX_OOB(*(s32*)&poly.gt4->x1) ||
                WIDE_VERTEX_OOB(*(s32*)&poly.gt4->x2) || WIDE_VERTEX_OOB(*(s32*)&poly.gt4->x3))
            {
                continue;
            }

            Shadow_Copy(&poly.gt4->x0, &xy[c0]); /* :4243 SH_PGXP_PROP4 */
            Shadow_Copy(&poly.gt4->x1, &xy[c1]);
            Shadow_Copy(&poly.gt4->x2, &xy[c2]);
            Shadow_Copy(&poly.gt4->x3, &xy[c3]);

            if (var_a3 != 0) /* :4248 */
            {
                *(s32*)&poly.gt4->r0 = shade[prim->normal[0]];
                *(s32*)&poly.gt4->r1 = shade[prim->normal[1]];
                *(s32*)&poly.gt4->r2 = shade[prim->normal[2]];
                *(s32*)&poly.gt4->r3 = shade[prim->normal[3]];
            }
            else
            {
                *(s32*)&poly.gt4->r0 = *(s32*)&poly.gt4->r1 = *(s32*)&poly.gt4->r2 = *(s32*)&poly.gt4->r3 =
                    *(s32*)&env->field_3D8;
            }

            poly.gt4->code = ((prim->flags >> 15) * 2) | 0x3C; /* :4260 */

            *(s32*)&poly.gt4->u0 = (s32)(((u32)prim->uv[0] | ((u32)prim->clut << 16)) + uvTpage); /* :4262 */
            *(s32*)&poly.gt4->u1 = (s32)(((u32)prim->uv[1] | ((u32)prim->flags << 16)) & 0xFFFFFF); /* :4263 */
            *(u16*)&poly.gt4->u2 = prim->uv[2];                                                     /* :4264 */
            *(u16*)&poly.gt4->u3 = prim->uv[3];                                                     /* :4265 */

            poly.gt4->pad2 = WIDE_SCREEN_Z_TO_FOG(z[c0]); /* :4271 */
            poly.gt4->p1   = WIDE_SCREEN_Z_TO_FOG(z[c1]);
            poly.gt4->p2   = WIDE_SCREEN_Z_TO_FOG(z[c2]);
            poly.gt4->p3   = WIDE_SCREEN_Z_TO_FOG(z[c3]);

            setlen(poly.gt4, 12);
            WIDE_CLAMP_OT_DEPTH(depth, arg2);
            addPrim(&ot[(depth >> arg2) >> 2], poly.gt4); /* :4280 */
            poly.gt4++;
        }
    }

    GsOUT_PACKET_P = poly.packet; /* :4302 */
    return exhausted;
}

void Pc_WideLm_DrawPart(s_ModelInfo* modelInfo, GsOT_TAG* otTag, s32 arg2,
                        MATRIX* viewMat, MATRIX* worldMat, u16 arg5)
{
    s_WideLmPart*     part;
    s_GteScratchData2 env;
    u8*               pktEnd;
    s16               var_v1;
    s32               var_a3;
    u32               uvTpage;
    s32               farcap;
    u32               mi;

    if (modelInfo->field_0 < 0) /* clone func_80057090:1759-1762 */
    {
        return;
    }

    part = Pc_WideLm_PartOf(modelInfo->modelHdr);
    if (part == NULL || part->meshCount == 0)
    {
        return;
    }

    /* The drawer clones only the LIT chain; the validator (V12) rejects v7
     * parts that select the flat (field_B_0==0) or third (field_B_4!=0) chains.
     * Guard here too: a part that slipped registration draws nothing rather
     * than being drawn lit with wrong shading. */
    if (modelInfo->modelHdr->field_B_0 == 0 || modelInfo->modelHdr->field_B_4 != 0)
    {
        return;
    }

    pktEnd = (u8*)Pc_PacketBuf_End(); /* F1 */
    if (pktEnd == NULL)
    {
        return;
    }

    /* (A) light-vector setup — clone func_80057090:1774-1777. */
    if (worldMat != NULL && g_WorldEnvWork.field_0 != 0)
    {
        func_80057228(worldMat, g_WorldEnvWork.field_54, &g_WorldEnvWork.field_58, &g_WorldEnvWork.field_60);
    }
    /* Stage-1 handles the field_B_0 lit chain only; the validator (V12) rejects
     * non-lit v7 parts and DrawPart guards field_B_0/field_B_4 above, so control
     * only reaches here for a lit part. Clone :1781. */
    g_WorldEnvWork.field_14C = arg5;

    /* (B) flat-light / GTE env — clone func_8005A21C:3682-3720. Use the no-CPU-
     * fog light alpha so character fog is owned solely by the per-vertex fog
     * bytes emitted below (cloning the #else fog ramp would double-fade). */
    var_v1 = 256 << 4;
    switch (g_WorldEnvWork.field_0)
    {
        case 0:
            func_8005A42C((s_GteScratchData*)&env, var_v1);
            break;

        case 1:
            func_8005A478((s_GteScratchData*)&env, var_v1);
            SetColorMatrix(&g_WorldEnvWork.field_2C);
            gte_lddqa(g_WorldEnvWork.field_4C);
            gte_lddqb_0();
            break;

        case 2:
            func_8005A838((s_GteScratchData*)&env, var_v1);
            SetColorMatrix(&g_WorldEnvWork.field_2C);
            gte_lddqa(g_WorldEnvWork.field_4C);
            gte_lddqb_0();
            break;
    }

    var_a3  = g_WorldEnvWork.field_0;                  /* :4047 emit color selector */
    uvTpage = (u32)g_WorldEnvWork.field_14C << 16;     /* :4048 */

    farcap = 0x79C << (arg2 + 2); /* :4050-4052 */
    if (g_WorldEnvWork.isFogEnabled)
    {
        farcap = MIN(farcap, WIDE_FOG_FAR_DIST());
    }
    WIDE_WHOLEMAP_FARCAP(farcap);

    for (mi = 0; mi < part->meshCount; mi++)
    {
        const s_WideMesh* mesh = &part->meshes[mi];

        if (mesh->vertexCount == 0 || mesh->primCount == 0)
        {
            continue;
        }
        if (!WideBufs_Ensure(mesh->vertexCount, mesh->normalCount))
        {
            continue;
        }

        Pc_WideLm_Transform(mesh, viewMat, s_wideXy, s_wideZ); /* clone :3730 */
        if (var_a3 != 0)
        {
            Pc_WideLm_Shade(mesh, &env, s_wideShade); /* clone :3732-3735 */
        }
        if (Pc_WideLm_Emit(mesh, &env, otTag, arg2, s_wideXy, s_wideZ, s_wideShade, var_a3, uvTpage,
                           farcap, pktEnd)) /* clone :3737 */
        {
            break; /* arena exhausted — stop this part */
        }
    }
}
