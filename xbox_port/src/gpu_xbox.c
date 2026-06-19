/*
 * gpu_xbox.c - PSX libgpu (DrawOTag + primitive dispatch) on the NV2A backend.
 *
 * Replaces PsyCross's GL libgpu. The game (and the reused libgs scene graph)
 * builds PSX GPU primitives into ordering tables; DrawOTag walks the OT linked
 * list and converts each primitive into screen-space triangles for gpu_nv2a.
 * The OT-walk mirrors PsyCross's ParsePrimitivesLinkedList: follow the P_TAG
 * `addr` links, and at each node with len>0 parse (len+P_LEN) longs of prims,
 * advancing by each prim's (primLength + P_LEN) longs.
 *
 * MILESTONE 3 (current): polygons only (F3/F4/FT3/FT4/G3/G4/GT3/GT4), rendered
 * UNTEXTURED (flat/gouraud colour; textured prims use their colour). Lines,
 * sprites, tiles, draw-env (tpage/clut) and real texturing come next.
 */
#include <libgte.h>
#include <libgpu.h>
#include <stdint.h>

#include "gpu_nv2a.h"
#include "sh_log.h"

/* Active environments + display state (PSX libgpu bookkeeping). Frame present is
 * driven by GpuNv2a_FrameBegin/End; these are wired to the present when the game
 * loop is integrated. */
static int     g_gpuDisabled = 0;
static DISPENV g_activeDispEnv;
static DRAWENV g_activeDrawEnv;

/* OT terminator the termPrim() macro points at (PsyCross defines this in its GL
 * PsyX_GPU.cpp, which we don't build). addr = -1 ends a DrawOTag walk. */
OT_TAG prim_terminator = { (uintptr_t)-1, 0 };

/* --- primitive -> triangle conversion ------------------------------------- */

static void PutVert(ShVertex* v, int x, int y, int r, int g, int b)
{
    v->pos[0] = (float)x;
    v->pos[1] = (float)y;
    v->pos[2] = 0.0f;
    v->col[0] = (float)r * (1.0f / 255.0f);
    v->col[1] = (float)g * (1.0f / 255.0f);
    v->col[2] = (float)b * (1.0f / 255.0f);
    v->col[3] = 1.0f;
    v->tex[0] = 0.0f;
    v->tex[1] = 0.0f;
}

/* PSX quad verts are in a Z/strip order (0,1,2,3) -> two triangles. Culling is
 * off, so winding does not matter. */
static void EmitTri(ShVertex* a, ShVertex* b, ShVertex* c)
{
    ShVertex tri[3];
    tri[0] = *a; tri[1] = *b; tri[2] = *c;
    GpuNv2a_EmitTris(tri, 3);
}

static void EmitQuad(ShVertex* v0, ShVertex* v1, ShVertex* v2, ShVertex* v3)
{
    ShVertex q[6];
    q[0] = *v0; q[1] = *v1; q[2] = *v2;
    q[3] = *v1; q[4] = *v2; q[5] = *v3;
    GpuNv2a_EmitTris(q, 6);
}

/* code bit flags for polygon primitives (0x20-0x3F) */
#define POLY_GOURAUD 0x10
#define POLY_QUAD    0x08
#define POLY_TEXTURE 0x04

static int ProcessPoly(P_TAG* tag)
{
    const int code     = tag->code;
    const int gouraud  = code & POLY_GOURAUD;
    const int quad     = code & POLY_QUAD;
    const int textured = code & POLY_TEXTURE;
    ShVertex  v[4];
    int       i;

    if (!gouraud && !textured) {
        /* POLY_F3 / POLY_F4 */
        POLY_F4* p = (POLY_F4*)tag;
        const VERTTYPE* xy = &p->x0;
        for (i = 0; i < (quad ? 4 : 3); i++)
            PutVert(&v[i], xy[i * 2], xy[i * 2 + 1], p->r0, p->g0, p->b0);
    } else if (!gouraud && textured) {
        /* POLY_FT3 / POLY_FT4 (textured: colour only for now) */
        POLY_FT4* p = (POLY_FT4*)tag;
        PutVert(&v[0], p->x0, p->y0, p->r0, p->g0, p->b0);
        PutVert(&v[1], p->x1, p->y1, p->r0, p->g0, p->b0);
        PutVert(&v[2], p->x2, p->y2, p->r0, p->g0, p->b0);
        if (quad) PutVert(&v[3], p->x3, p->y3, p->r0, p->g0, p->b0);
    } else if (gouraud && !textured) {
        /* POLY_G3 / POLY_G4 */
        POLY_G4* p = (POLY_G4*)tag;
        PutVert(&v[0], p->x0, p->y0, p->r0, p->g0, p->b0);
        PutVert(&v[1], p->x1, p->y1, p->r1, p->g1, p->b1);
        PutVert(&v[2], p->x2, p->y2, p->r2, p->g2, p->b2);
        if (quad) PutVert(&v[3], p->x3, p->y3, p->r3, p->g3, p->b3);
    } else {
        /* POLY_GT3 / POLY_GT4 (textured gouraud: colour only for now) */
        POLY_GT4* p = (POLY_GT4*)tag;
        PutVert(&v[0], p->x0, p->y0, p->r0, p->g0, p->b0);
        PutVert(&v[1], p->x1, p->y1, p->r1, p->g1, p->b1);
        PutVert(&v[2], p->x2, p->y2, p->r2, p->g2, p->b2);
        if (quad) PutVert(&v[3], p->x3, p->y3, p->r3, p->g3, p->b3);
    }

    {
        static int logN = 8;
        if (logN > 0) {
            logN--;
            SH_DBG("[GPU] poly code=0x%02x quad=%d v0=(%d,%d) v1=(%d,%d) v2=(%d,%d)",
                   code, quad ? 1 : 0,
                   (int)v[0].pos[0], (int)v[0].pos[1],
                   (int)v[1].pos[0], (int)v[1].pos[1],
                   (int)v[2].pos[0], (int)v[2].pos[1]);
        }
    }

    if (quad)
        EmitQuad(&v[0], &v[1], &v[2], &v[3]);
    else
        EmitTri(&v[0], &v[1], &v[2]);

    /* prim length in longs (excl. tag), matching the libgpu.h static_asserts */
    if (textured) return gouraud ? (quad ? 12 : 9) : (quad ? 9 : 7);
    if (gouraud)  return quad ? 8 : 6;
    return quad ? 5 : 4;
}

/* Returns the parsed primitive's length in longs (excl. tag), or the tag's
 * declared length for unhandled prims so the packet walk stays in sync. */
static int ParsePrim(P_TAG* tag)
{
    const int primType = tag->code & 0xF0;

    switch (primType) {
    case 0x20: /* flat polygons   */
    case 0x30: /* gouraud polygons*/
        return ProcessPoly(tag);
    default:
        /* lines (0x40/0x50), sprites/tiles (0x60/0x70), DR_LOAD (0xA0),
         * draw-env (0xE0): not handled yet — skip by declared length. */
        return getlen(tag);
    }
}

/* --- public PSX libgpu API ------------------------------------------------ */

static int s_otLog = 1; /* one-shot OT-walk trace */

void DrawOTag(u_long* p)
{
    uintptr_t base = (uintptr_t)p;
    int       safety;

    if (g_gpuDisabled)
        return;

    if (s_otLog) SH_DBG("[OT] DrawOTag head=%p", (void*)base);

    for (safety = 0; safety < 16384; safety++) {
        const int len = getlen(base);
        uintptr_t next;
        if (len > 0 && len <= 32) {
            uintptr_t cur = base;
            const uintptr_t end = base + (len + P_LEN) * sizeof(u_int);
            while (cur < end) {
                const int pl = ParsePrim((P_TAG*)cur);
                if (pl <= 0) break;
                cur += (pl + P_LEN) * sizeof(u_int);
            }
        }

        next = getaddr(base);
        if (s_otLog) SH_DBG("[OT] node=%p len=%d code=0x%02x next=%p",
                            (void*)base, len, ((P_TAG*)base)->code, (void*)next);
        if (next == (uintptr_t)0xffffffff || next < 0x10000)
            break;
        base = next;
    }

    if (s_otLog) { SH_DBG("[OT] walk done after %d nodes", safety); s_otLog = 0; }
}

void DrawOTagEnv(u_long* p, DRAWENV* env)
{
    (void)env; /* tpage/clip from env handled with draw-env support (next) */
    DrawOTag(p);
}

void DrawPrim(void* p)
{
    ParsePrim((P_TAG*)p);
}

/* --- GPU control surface (PSX libgpu API; semantics per PsyCross libgpu.c) -- */

int ResetGraph(int mode)
{
    (void)mode;
    g_gpuDisabled = 0;
    return 0;
}

int SetGraphDebug(int level)
{
    (void)level;
    return 0;
}

void SetDispMask(int mask)
{
    g_gpuDisabled = (mask == 0);
}

int DrawSync(int mode)
{
    (void)mode; /* drawing is flushed at frame end (GpuNv2a_FrameEnd) */
    return 0;
}

int GetODE(void)
{
    return 0;
}

/* OT buckets are OT_TAG (P_LEN longs each, extended pointers). ClearOTagR builds
 * a reverse-linked list (DrawOTag(&ot[n-1]) walks high->low bucket); ClearOTag
 * forward. */
u_long* ClearOTagR(u_long* ot, int n)
{
    OT_TAG* t = (OT_TAG*)ot;
    int     i;

    if (n == 0)
        return NULL;

    termPrim(&t[0]);
    setlen(&t[0], 0);
    for (i = 1; i < n; ++i) {
        setaddr(&t[i], &t[i - 1]);
        setlen(&t[i], 0);
    }
    return NULL;
}

u_long* ClearOTag(u_long* ot, int n)
{
    OT_TAG* t = (OT_TAG*)ot;
    int     i;

    if (n == 0)
        return NULL;

    termPrim(&t[n - 1]);
    setlen(&t[n - 1], 0);
    for (i = n - 1; i >= 0; --i) {
        setaddr(&t[i], &t[i + 1]);
        setlen(&t[i], 0);
    }
    return NULL;
}

DISPENV* SetDefDispEnv(DISPENV* env, int x, int y, int w, int h)
{
    env->disp.x = x;   env->disp.y = y;   env->disp.w = w;   env->disp.h = h;
    env->screen.x = 0; env->screen.y = 0; env->screen.w = 0; env->screen.h = 0;
    env->isrgb24 = 0;  env->isinter = 0;  env->pad0 = 0;     env->pad1 = 0;
    return env;
}

DRAWENV* SetDefDrawEnv(DRAWENV* env, int x, int y, int w, int h)
{
    env->clip.x = x; env->clip.y = y; env->clip.w = w; env->clip.h = h;
    env->tw.x = 0; env->tw.y = 0; env->tw.w = 0; env->tw.h = 0;
    env->r0 = 0; env->g0 = 0; env->b0 = 0;
    env->dtd = 1;
    env->dfe = (h < 289) ? 1 : 0; /* NTSC */
    env->ofs[0] = x; env->ofs[1] = y;
    env->tpage = 10;
    env->isbg = 0;
    return env;
}

DISPENV* PutDispEnv(DISPENV* env)
{
    g_activeDispEnv = *env;
    return env;
}

DRAWENV* PutDrawEnv(DRAWENV* env)
{
    g_activeDrawEnv = *env;
    return env;
}
