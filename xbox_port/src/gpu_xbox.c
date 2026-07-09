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

/* Screen transform, derived from the active draw env (cached by PutDrawEnv):
 * vertex = (raw + ofs) * (640/clip.w, 480/clip.h). The PSX GPU adds the draw-env
 * offset to every prim (PsyCross does the same on PC) — ofs is the clip-area
 * center — then we scale that clip area (320x224 in-game, 320x448 interlaced 2D)
 * to fill the 640x480 NV2A framebuffer. Defaults are the common in-game values
 * until the first PutDrawEnv. */
static float s_ofsX = 160.0f, s_ofsY = 112.0f;
static float s_scaleX = 640.0f / 320.0f, s_scaleY = 480.0f / 224.0f;

/* Currently-bound NV2A texture (bind-dedup in ProcessPoly); reset each DrawOTag.
 * -1 = unknown, forcing a bind on the first prim of the walk. */
static const void* s_curTex = (const void*)-1;
static int         s_curBlend = -1;   /* current blend-enable state (dedup) */
/* Current PSX texture page for SPRT sprites — they carry no tpage of their own and
 * inherit it from the most recent DR_TPAGE the game prepends into the OT bucket.
 * Reset per DrawOTag to the draw-env default. */
static int         s_curTpage = 0;

/* World fog colour, fed per-frame from fog.color by game_main.c (InGame). The
 * SH_PC_PORT build strips PSX vertex-colour fog and instead writes per-vertex
 * fog factors (0..127) into the GT-prim pad bytes for the renderer to consume
 * — PsyCross does it in its fragment shader; here ApplyFog() folds (1-f) into
 * the diffuse and puts fogColor*f into the specular attribute, which the final
 * combiner ADDS (out = tex*col*(1-f) + fogC*f — exactly PsyCross's mix()). */
extern float g_PsyX_FogColor[3];

/* --- render census (probe [OTS]/[FOGPAD]/[ABR]/[PRIM?]) --------------------
 * Accumulated over a 150-frame window, dumped as 3 integer-only lines. Replaces
 * the old %180 [BB] sampler, which aliased onto the same DrawOTag call slot
 * every frame (2 calls/frame, 180%2==0) and never measured the world OT. */
extern int g_Nv2aFrameCount;              /* incremented each GpuNv2a_FrameBegin */
static int s_cnFrame = -1;                /* frame id of the current DrawOTag */
static int s_cnCallIdx = 0;               /* DrawOTag call # within the frame */
static int s_cnNodes[2];                  /* nodes walked, call slot 0/1 (last frame) */
static int s_cnCallsMax = 0;              /* max DrawOTag calls seen in one frame */
static int s_cnPrims = 0;                 /* prims parsed in the window */
static int s_cnGt = 0, s_cnFogged = 0;    /* GT prims / with nonzero fog pads */
static int s_cnPadMin = 999, s_cnPadMax = -1;
static int s_cnAbr[4];                    /* semi-trans prims per ABR mode */
static int s_cnLines = 0, s_cnUnk = 0;    /* line prims / unknown-code skips */
static int s_bbMinX = 99999, s_bbMaxX = -99999, s_bbMinY = 99999, s_bbMaxY = -99999;
static int s_unkLogged = 0;               /* [PRIM?] one-shot budget per boot */

static void TrackBB(const ShVertex* v)
{
    int sx = (int)v->pos[0], sy = (int)v->pos[1];
    if (sx < s_bbMinX) s_bbMinX = sx;
    if (sx > s_bbMaxX) s_bbMaxX = sx;
    if (sy < s_bbMinY) s_bbMinY = sy;
    if (sy > s_bbMaxY) s_bbMaxY = sy;
}

static void PutVert(ShVertex* v, int x, int y, int r, int g, int b)
{
    v->pos[0] = ((float)x + s_ofsX) * s_scaleX;
    v->pos[1] = ((float)y + s_ofsY) * s_scaleY;
    v->pos[2] = 0.0f;
    v->col[0] = (float)r * (1.0f / 255.0f);
    v->col[1] = (float)g * (1.0f / 255.0f);
    v->col[2] = (float)b * (1.0f / 255.0f);
    v->col[3] = 1.0f;
    v->tex[0] = 0.0f;
    v->tex[1] = 0.0f;
    v->spec[0] = 0.0f; v->spec[1] = 0.0f; v->spec[2] = 0.0f; v->spec[3] = 0.0f;
    TrackBB(v);
}

/* Textured vertex: texel UV (the decoded page is 256x256, PSX UVs are 0..255 so
 * they index it 1:1) + PSX texel modulation, where colour 0x80 = 1.0, so we scale
 * the prim colour by 2 (clamped). Texel 0x0000 already decoded to transparent. */
extern unsigned int* PsxVram_GetTexture(int tpage, int clut);

static void PutVertUV(ShVertex* v, int x, int y, int r, int g, int b, int u, int tv)
{
    v->pos[0] = ((float)x + s_ofsX) * s_scaleX;
    v->pos[1] = ((float)y + s_ofsY) * s_scaleY;
    v->pos[2] = 0.0f;
    r <<= 1; if (r > 255) r = 255;
    g <<= 1; if (g > 255) g = 255;
    b <<= 1; if (b > 255) b = 255;
    v->col[0] = (float)r * (1.0f / 255.0f);
    v->col[1] = (float)g * (1.0f / 255.0f);
    v->col[2] = (float)b * (1.0f / 255.0f);
    v->col[3] = 1.0f;
    v->tex[0] = (float)u;
    v->tex[1] = (float)tv;
    v->spec[0] = 0.0f; v->spec[1] = 0.0f; v->spec[2] = 0.0f; v->spec[3] = 0.0f;
    TrackBB(v);
}

/* Per-vertex distance fog: fold (1-f) into the diffuse, put fogColor*f into the
 * specular (final combiner adds it). pad is the game's 0..127 fog factor. */
static void ApplyFog(ShVertex* v, int pad)
{
    float f;
    if (pad <= 0) return;
    if (pad > 127) pad = 127;
    f = (float)pad * (1.0f / 127.0f);
    v->col[0] *= 1.0f - f;
    v->col[1] *= 1.0f - f;
    v->col[2] *= 1.0f - f;
    v->spec[0] = g_PsyX_FogColor[0] * f;
    v->spec[1] = g_PsyX_FogColor[1] * f;
    v->spec[2] = g_PsyX_FogColor[2] * f;
    s_cnFogged++;
    if (pad < s_cnPadMin) s_cnPadMin = pad;
    if (pad > s_cnPadMax) s_cnPadMax = pad;
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

/* Prim length in longs (excl. tag) for polygon codes — needed by the early-out
 * paths (null-FT3 / size rejection) before the emit path returns it. */
static int PolyLen(int gouraud, int textured, int quad)
{
    if (textured) return gouraud ? (quad ? 12 : 9) : (quad ? 9 : 7);
    if (gouraud)  return quad ? 8 : 6;
    return quad ? 5 : 4;
}

/* Real PSX GPU rejects prims spanning >=1024px in x or >=512 in y (pre-offset).
 * GTE-clamped verts (SXY saturates at +-1024) can produce such spans; the PSX
 * silently drops them instead of smearing a degenerate tri across the screen. */
static int PolyOversized(const VERTTYPE* xy, int stridePairs, int n)
{
    int i, minX = 32767, maxX = -32768, minY = 32767, maxY = -32768;
    for (i = 0; i < n; i++) {
        int x = xy[i * stridePairs], y = xy[i * stridePairs + 1];
        if (x < minX) minX = x;
        if (x > maxX) maxX = x;
        if (y < minY) minY = y;
        if (y > maxY) maxY = y;
    }
    return (maxX - minX) >= 1024 || (maxY - minY) >= 512;
}

static int ProcessPoly(P_TAG* tag)
{
    const int code     = tag->code;
    const int gouraud  = code & POLY_GOURAUD;
    const int quad     = code & POLY_QUAD;
    const int textured = code & POLY_TEXTURE;
    const int abe      = code & 0x02;   /* semi-transparency (ABE) */
    const int primLen  = PolyLen(gouraud, textured, quad);
    ShVertex  v[4];
    const void* texAddr = 0;   /* decoded PSX texture for this prim, else white */
    int       blendTpage = s_curTpage;  /* ABR bits source (prim's own if textured) */
    int       i;

    if (!gouraud && !textured) {
        /* POLY_F3 / POLY_F4 */
        POLY_F4* p = (POLY_F4*)tag;
        const VERTTYPE* xy = &p->x0;
        if (PolyOversized(xy, 2, quad ? 4 : 3)) return primLen;
        for (i = 0; i < (quad ? 4 : 3); i++)
            PutVert(&v[i], xy[i * 2], xy[i * 2 + 1], p->r0, p->g0, p->b0);
    } else if (!gouraud && textured) {
        /* POLY_FT3 / POLY_FT4 (flat textured) */
        POLY_FT4* p = (POLY_FT4*)tag;
        /* Official SCE "null polygon" hack (PsyCross honors it): an FT3 with all
         * six coords -1 is a DR_TPAGE carrier, not geometry — take its tpage for
         * subsequent SPRTs and draw nothing. */
        if (!quad && p->x0 == -1 && p->y0 == -1 && p->x1 == -1 && p->y1 == -1 &&
            p->x2 == -1 && p->y2 == -1) {
            s_curTpage = p->tpage;
            return primLen;
        }
        s_curTpage = blendTpage = p->tpage;   /* SPRTs inherit the latest tpage */
        {
            const VERTTYPE xy[8] = { p->x0, p->y0, p->x1, p->y1, p->x2, p->y2, p->x3, p->y3 };
            if (PolyOversized(xy, 2, quad ? 4 : 3)) return primLen;
        }
        texAddr = PsxVram_GetTexture(p->tpage, p->clut);
        PutVertUV(&v[0], p->x0, p->y0, p->r0, p->g0, p->b0, p->u0, p->v0);
        PutVertUV(&v[1], p->x1, p->y1, p->r0, p->g0, p->b0, p->u1, p->v1);
        PutVertUV(&v[2], p->x2, p->y2, p->r0, p->g0, p->b0, p->u2, p->v2);
        if (quad) PutVertUV(&v[3], p->x3, p->y3, p->r0, p->g0, p->b0, p->u3, p->v3);
    } else if (gouraud && !textured) {
        /* POLY_G3 / POLY_G4 */
        POLY_G4* p = (POLY_G4*)tag;
        {
            const VERTTYPE xy[8] = { p->x0, p->y0, p->x1, p->y1, p->x2, p->y2, p->x3, p->y3 };
            if (PolyOversized(xy, 2, quad ? 4 : 3)) return primLen;
        }
        PutVert(&v[0], p->x0, p->y0, p->r0, p->g0, p->b0);
        PutVert(&v[1], p->x1, p->y1, p->r1, p->g1, p->b1);
        PutVert(&v[2], p->x2, p->y2, p->r2, p->g2, p->b2);
        if (quad) PutVert(&v[3], p->x3, p->y3, p->r3, p->g3, p->b3);
    } else {
        /* POLY_GT3 / POLY_GT4 (gouraud textured) — the world/character prims.
         * Their pad bytes carry the SH_PC_PORT per-vertex fog factors. */
        POLY_GT4* p4 = (POLY_GT4*)tag;
        POLY_GT3* p3 = (POLY_GT3*)tag;
        s_cnGt++;
        if (quad) {
            s_curTpage = blendTpage = p4->tpage;
            {
                const VERTTYPE xy[8] = { p4->x0, p4->y0, p4->x1, p4->y1, p4->x2, p4->y2, p4->x3, p4->y3 };
                if (PolyOversized(xy, 2, 4)) return primLen;
            }
            texAddr = PsxVram_GetTexture(p4->tpage, p4->clut);
            PutVertUV(&v[0], p4->x0, p4->y0, p4->r0, p4->g0, p4->b0, p4->u0, p4->v0);
            PutVertUV(&v[1], p4->x1, p4->y1, p4->r1, p4->g1, p4->b1, p4->u1, p4->v1);
            PutVertUV(&v[2], p4->x2, p4->y2, p4->r2, p4->g2, p4->b2, p4->u2, p4->v2);
            PutVertUV(&v[3], p4->x3, p4->y3, p4->r3, p4->g3, p4->b3, p4->u3, p4->v3);
            /* GT4 fog pads: v0 in pad2's low byte (v0's own slot is `code`),
             * v1..v3 in p1/p2/p3 (bodyprog_80055028.c PC_FACE_FOG_VERTS). */
            ApplyFog(&v[0], (int)(p4->pad2 & 0xFF));
            ApplyFog(&v[1], p4->p1);
            ApplyFog(&v[2], p4->p2);
            ApplyFog(&v[3], p4->p3);
        } else {
            s_curTpage = blendTpage = p3->tpage;
            {
                const VERTTYPE xy[6] = { p3->x0, p3->y0, p3->x1, p3->y1, p3->x2, p3->y2 };
                if (PolyOversized(xy, 2, 3)) return primLen;
            }
            texAddr = PsxVram_GetTexture(p3->tpage, p3->clut);
            PutVertUV(&v[0], p3->x0, p3->y0, p3->r0, p3->g0, p3->b0, p3->u0, p3->v0);
            PutVertUV(&v[1], p3->x1, p3->y1, p3->r1, p3->g1, p3->b1, p3->u1, p3->v1);
            PutVertUV(&v[2], p3->x2, p3->y2, p3->r2, p3->g2, p3->b2, p3->u2, p3->v2);
            /* GT3 fog pads: v0 borrows v1's (p1); v2 = p2 (PsyCross parity). */
            ApplyFog(&v[0], p3->p1);
            ApplyFog(&v[1], p3->p1);
            ApplyFog(&v[2], p3->p2);
        }
    }

#ifdef SH_GPU_PRIM_TRACE
    {   /* Sample a few prims periodically (incl. in-game) — code tells us if the OT
         * walk is reading valid polys or garbage; coords show typical vs outlier.
         * OFF by default: even throttled (5/4096) this produced 213k unbuffered HDD
         * writes in one run, a real perf drag. Define SH_GPU_PRIM_TRACE to re-enable. */
        static unsigned n = 0;
        if ((n++ & 0xFFF) < 5)
            SH_DBG("[GPU] code=0x%02x quad=%d scr=(%d,%d)(%d,%d)(%d,%d)",
                   code, quad ? 1 : 0,
                   (int)v[0].pos[0], (int)v[0].pos[1],
                   (int)v[1].pos[0], (int)v[1].pos[1],
                   (int)v[2].pos[0], (int)v[2].pos[1]);
    }
#endif

    /* Semi-transparent (ABE) prims use the tpage's ABR mode (bits 5-6): 0 =
     * 0.5B+0.5F average, 1 = B+F additive (fire/flashlight glow), 2 = B-F
     * subtractive (shadow darkening), 3 = B+0.25F. Textured prims carry the STP
     * bit per texel (alpha 0x80/0xFF from the decode); untextured ABR0 prims get
     * vertex alpha 0.5 so SRC_ALPHA blending averages them. Dedup'd per walk. */
    {
        const int wantBlend = abe ? 1 + ((blendTpage >> 5) & 3) : 0;
        if (abe) s_cnAbr[(blendTpage >> 5) & 3]++;
        if (abe && !textured && ((blendTpage >> 5) & 3) == 0) {
            const int n = quad ? 4 : 3;
            for (i = 0; i < n; i++)
                v[i].col[3] = 0.5f;
        }
        if (wantBlend != s_curBlend) {
            GpuNv2a_SetBlendMode(wantBlend);
            s_curBlend = wantBlend;
        }
    }

    /* Bind this prim's texture (or restore white for untextured), but only when it
     * actually changes — re-binding on every prim (incl. untextured) was the main
     * texture-pipeline slowdown. s_curTex is reset per DrawOTag. */
    if (texAddr != s_curTex) {
        if (texAddr)
            GpuNv2a_BindTexture(texAddr, 256, 256);
        else
            GpuNv2a_BindWhite();
        s_curTex = texAddr;
    }

    if (quad)
        EmitQuad(&v[0], &v[1], &v[2], &v[3]);
    else
        EmitTri(&v[0], &v[1], &v[2]);

    return primLen;
}

/* DR_MODE / DR_TPAGE (code 0xE0/0xE1) — the GPU draw-mode packets. We only need
 * the texture page out of them so subsequent SPRTs sample the right VRAM region. */
static int ProcessDrawMode(P_TAG* tag)
{
    /* The GP0 code words live in the prim's code[] array, which begins P_LEN
     * longs into the packet — NOT one long in. Under USE_EXTENDED_PRIM_POINTERS
     * (this build) the tag header is P_LEN==2 longs (uintptr_t addr + len/pgxp),
     * so DR_TPAGE/DR_MODE.code[0] is at offset +P_LEN. Reading +1 landed on the
     * len/pgxp_index field, so (w>>24)==0xE1 never matched and s_curTpage was
     * never updated — every SPRT (title image, menu/HUD text) then sampled the
     * draw-env default tpage instead of the page the game prepended, decoding
     * the wrong VRAM region (invisible / garbage). */
    const u_long* w = ((const u_long*)tag) + P_LEN;
    const int     n = getlen(tag);
    int           i;

    for (i = 0; i < n; i++)
        if ((w[i] >> 24) == 0xE1)               /* GP0(E1) = set draw mode (tpage) */
            s_curTpage = (int)(w[i] & 0x1FF);
    return n;
}

/* SPRT (textured sprite) / TILE (flat rect), codes 0x60-0x77. Menu / inventory /
 * HUD text are SPRTs (the game runs in standard-res glyph mode) — these were being
 * skipped, so all 2D text was missing while polygon UI (portraits) rendered. SPRT
 * carries no tpage; it inherits s_curTpage from the preceding DR_TPAGE. */
static int ProcessSprtTile(P_TAG* tag)
{
    const int code     = tag->code;
    const int abe      = code & 0x02;
    const int textured = code & 0x04;           /* SPRT (textured) vs TILE (flat) */
    const int sizeMode = (code >> 3) & 3;       /* 0=var 1=1x1 2=8x8 3=16x16 */
    const int fixedSz  = sizeMode == 1 ? 1 : sizeMode == 2 ? 8 : sizeMode == 3 ? 16 : 0;
    int x0, y0, w, h, r, g, b, u0 = 0, v0 = 0, clut = 0;
    const void* texAddr = 0;
    ShVertex v[4];

    if (textured) {
        SPRT* p = (SPRT*)tag;
        int   uw, vh;
        x0 = p->x0; y0 = p->y0; r = p->r0; g = p->g0; b = p->b0;
        u0 = p->u0; v0 = p->v0; clut = p->clut;
        w = fixedSz ? fixedSz : p->w;
        h = fixedSz ? fixedSz : p->h;
        /* Clamp the UV span (not the screen size) so the bottom-right texel never
         * samples past the 256x256 decoded page — a 16-tall glyph at v0=240 would
         * otherwise land on row 256, one past the page (mirrors PsyCross
         * MakeTexcoordRect). Screen w/h stay, so the glyph covers the same pixels. */
        uw = w; vh = h;
        if (u0 + uw > 255) uw = 255 - u0;
        if (v0 + vh > 255) vh = 255 - v0;
        texAddr = PsxVram_GetTexture(s_curTpage, clut);
        PutVertUV(&v[0], x0,     y0,     r, g, b, u0,      v0);
        PutVertUV(&v[1], x0 + w, y0,     r, g, b, u0 + uw, v0);
        PutVertUV(&v[2], x0,     y0 + h, r, g, b, u0,      v0 + vh);
        PutVertUV(&v[3], x0 + w, y0 + h, r, g, b, u0 + uw, v0 + vh);
    } else {
        TILE* p = (TILE*)tag;
        x0 = p->x0; y0 = p->y0; r = p->r0; g = p->g0; b = p->b0;
        w = fixedSz ? fixedSz : p->w;
        h = fixedSz ? fixedSz : p->h;
        PutVert(&v[0], x0,     y0,     r, g, b);
        PutVert(&v[1], x0 + w, y0,     r, g, b);
        PutVert(&v[2], x0,     y0 + h, r, g, b);
        PutVert(&v[3], x0 + w, y0 + h, r, g, b);
    }

    {
        /* SPRT/TILE carry no tpage: ABR comes from the inherited s_curTpage. */
        const int abr = (s_curTpage >> 5) & 3;
        const int wantBlend = abe ? 1 + abr : 0;
        if (abe) s_cnAbr[abr]++;
        if (abe && !textured && abr == 0) {
            v[0].col[3] = v[1].col[3] = v[2].col[3] = v[3].col[3] = 0.5f;
        }
        if (wantBlend != s_curBlend) {
            GpuNv2a_SetBlendMode(wantBlend);
            s_curBlend = wantBlend;
        }
    }
    if (texAddr != s_curTex) {
        if (texAddr)
            GpuNv2a_BindTexture(texAddr, 256, 256);
        else
            GpuNv2a_BindWhite();
        s_curTex = texAddr;
    }

    EmitQuad(&v[0], &v[1], &v[2], &v[3]);
    return getlen(tag);
}

/* Line primitives 0x40-0x5F, expanded to 1px-wide quads exactly as PsyCross's
 * MakeLineArray: horizontal-major lines thicken downward (+1 y, x1+1),
 * otherwise rightward (+1 x, y1+1). Options brightness bars, inventory borders
 * and map-screen routes are these — they were invisible while skipped.
 * Handled: LINE_F2 (len 3), F3 (5), F4 (6) as 1/2/3 segments; G2 (len 4).
 * G3 (7) / G4 (9) skip by length (PsyCross parity: it stubs them too). */
static void EmitLineSeg(int x0, int y0, int r0, int g0, int b0,
                        int x1, int y1, int r1, int g1, int b1)
{
    ShVertex v[4];
    const int dx = x1 - x0, dy = y1 - y0;
    if (dx > (dy < 0 ? -dy : dy)) {         /* horizontal-major */
        PutVert(&v[0], x0,     y0,     r0, g0, b0);
        PutVert(&v[1], x1 + 1, y1,     r1, g1, b1);
        PutVert(&v[2], x0,     y0 + 1, r0, g0, b0);
        PutVert(&v[3], x1 + 1, y1 + 1, r1, g1, b1);
    } else {                                 /* vertical-major */
        PutVert(&v[0], x0,     y0,     r0, g0, b0);
        PutVert(&v[1], x0 + 1, y0,     r0, g0, b0);
        PutVert(&v[2], x1,     y1 + 1, r1, g1, b1);
        PutVert(&v[3], x1 + 1, y1 + 1, r1, g1, b1);
    }
    EmitQuad(&v[0], &v[1], &v[2], &v[3]);
}

static int ProcessLine(P_TAG* tag)
{
    const int code    = tag->code;
    const int gouraud = code & 0x10;
    const int abe     = code & 0x02;
    const int len     = getlen(tag);

    s_cnLines++;

    /* Blend + white texture state (lines are untextured). */
    {
        const int abr = (s_curTpage >> 5) & 3;
        const int wantBlend = abe ? 1 + abr : 0;
        if (wantBlend != s_curBlend) {
            GpuNv2a_SetBlendMode(wantBlend);
            s_curBlend = wantBlend;
        }
    }
    if (s_curTex != 0) {
        GpuNv2a_BindWhite();
        s_curTex = 0;
    }

    if (!gouraud) {
        LINE_F2* p = (LINE_F2*)tag;
        const VERTTYPE* xy = &p->x0;
        int segs = (len == 3) ? 1 : (len == 5) ? 2 : (len == 6) ? 3 : 0;
        int s;
        for (s = 0; s < segs; s++)
            EmitLineSeg(xy[s * 2], xy[s * 2 + 1], p->r0, p->g0, p->b0,
                        xy[s * 2 + 2], xy[s * 2 + 3], p->r0, p->g0, p->b0);
    } else if (len == 4) {
        LINE_G2* p = (LINE_G2*)tag;
        EmitLineSeg(p->x0, p->y0, p->r0, p->g0, p->b0,
                    p->x1, p->y1, p->r1, p->g1, p->b1);
    }
    /* G3/G4 (len 7/9): skip by length. */
    return len;
}

/* VRAM ops the OT can carry (psx_vram.c / psx_libgpu_xbox.c). */
extern void PsxVram_Move(int sx, int sy, int w, int h, int dx, int dy);
extern int  LoadImage(RECT* rect, u_long* p);

/* Returns the parsed primitive's length in longs (excl. tag), or the tag's
 * declared length for unhandled prims so the packet walk stays in sync. */
static int ParsePrim(P_TAG* tag)
{
    const int primType = tag->code & 0xF0;

    s_cnPrims++;

    switch (primType) {
    case 0x00: {
        /* sub-code 0x01 = DR_MOVE (VRAM->VRAM copy, water refraction etc.);
         * sub-code 0x00 = 3-long cache-flush packet (skip). Word layout per
         * PsyCross SetDrawMove/ParsePrimitivesLinkedList. */
        if ((tag->code & 0x0F) == 0x01) {
            const u_long* w = ((const u_long*)tag) + P_LEN;
            const short*  sxy = (const short*)&w[2];   /* src x,y  */
            const short*  swh = (const short*)&w[4];   /* w,h      */
            const int     dx  = (int)(w[3] & 0xFFFF);
            const int     dy  = (int)((w[3] >> 16) & 0xFFFF);
            PsxVram_Move(sxy[0], sxy[1], swh[0], swh[1], dx, dy);
            return 5;
        }
        return getlen(tag) ? getlen(tag) : 3;
    }
    case 0x20: /* flat polygons   */
    case 0x30: /* gouraud polygons*/
        return ProcessPoly(tag);
    case 0x40: /* flat lines    */
    case 0x50: /* gouraud lines */
        return ProcessLine(tag);
    case 0x60: /* SPRT / TILE (variable + 1x1) */
    case 0x70: /* SPRT / TILE (8x8 / 16x16)    */
        return ProcessSprtTile(tag);
    case 0xA0: {
        /* DR_LOAD: in-OT VRAM upload (image data follows the rect words). */
        DR_LOAD* dl = (DR_LOAD*)tag;
        RECT     r;
        const u_long* w = ((const u_long*)tag) + P_LEN;
        const short*  rxy = (const short*)&w[1];
        r.x = rxy[0]; r.y = rxy[1]; r.w = rxy[2]; r.h = rxy[3];
        LoadImage(&r, (u_long*)dl->p);
        return getlen(tag);
    }
    case 0xE0: /* DR_MODE / DR_TPAGE / DR_ENV — track the texture page for SPRTs */
        return ProcessDrawMode(tag);
    default:
        /* Unknown/garbage tag: skip by declared length, log the first few once
         * per boot — after the draw-env packet builders were fixed, any hit
         * here means something still addPrims junk into the OT. */
        s_cnUnk++;
        if (s_unkLogged < 24) {
            s_unkLogged++;
            SH_DBG("[PRIM?] code=0x%02x len=%d", tag->code, getlen(tag));
        }
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

    /* Render census: detect the frame boundary via the NV2A frame counter and
     * dump the accumulated window once per 150 frames (~5s at 30fps). The old
     * %180 [BB] sampler aliased onto the same call slot every frame (2 calls
     * per frame, 180%2==0) and never measured the world OT. */
    if (g_Nv2aFrameCount != s_cnFrame) {
        if (s_cnFrame >= 0 && (s_cnFrame % 600) == 0 && s_cnPrims > 0) {
            SH_DBG("[OTS] f=%d calls=%d n0=%d n1=%d prims=%d bb=%d,%d,%d,%d",
                   s_cnFrame, s_cnCallsMax, s_cnNodes[0], s_cnNodes[1], s_cnPrims,
                   s_bbMinX, s_bbMaxX, s_bbMinY, s_bbMaxY);
            SH_DBG("[FOGPAD] gt=%d fogged=%d padMin=%d padMax=%d",
                   s_cnGt, s_cnFogged, s_cnPadMin > 128 ? -1 : s_cnPadMin, s_cnPadMax);
            SH_DBG("[ABR] avg=%d add=%d sub=%d q=%d lines=%d unk=%d",
                   s_cnAbr[0], s_cnAbr[1], s_cnAbr[2], s_cnAbr[3], s_cnLines, s_cnUnk);
            s_cnPrims = 0; s_cnGt = 0; s_cnFogged = 0;
            s_cnPadMin = 999; s_cnPadMax = -1;
            s_cnAbr[0] = s_cnAbr[1] = s_cnAbr[2] = s_cnAbr[3] = 0;
            s_cnLines = 0; s_cnUnk = 0; s_cnCallsMax = 0;
            s_bbMinX = 99999; s_bbMaxX = -99999; s_bbMinY = 99999; s_bbMaxY = -99999;
        }
        s_cnFrame = g_Nv2aFrameCount;
        s_cnCallIdx = 0;
    }

    s_curTex   = (const void*)-1; /* force a texture bind on the first prim */
    s_curBlend = -1;              /* force a blend-state set on the first prim */
    s_curTpage = g_activeDrawEnv.tpage; /* SPRTs use this until a DR_TPAGE updates it */

    if (s_otLog) SH_DBG("[OT] DrawOTag head=%p (walking; per-node trace off)", (void*)base);

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

        /* Per-node tracing is intentionally OFF: a real game OT is ~1000+ buckets,
         * and one unbuffered D: write per node makes the first frame crawl (and
         * never completed on hardware). The one-shot summary below is enough to
         * confirm the walk terminates. */
        next = getaddr(base);
        if (next == (uintptr_t)0xffffffff || next < 0x10000)
            break;
        base = next;
    }

    if (s_otLog) { SH_DBG("[OT] walk done after %d nodes (cap hit=%d)", safety, safety >= 16384); s_otLog = 0; }

    /* Census: record this walk's node count in its call slot (0 = first
     * DrawOTag of the frame = OT0/world in-game, 1 = the 2D/UI OT). */
    if (s_cnCallIdx < 2) s_cnNodes[s_cnCallIdx] = safety;
    s_cnCallIdx++;
    if (s_cnCallIdx > s_cnCallsMax) s_cnCallsMax = s_cnCallIdx;
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

/* Map the game's drawing space to the 640x480 framebuffer. The PSX draws prims at
 * (raw + draw-env offset) in VRAM, and the DISPLAY env (disp.x/y/w/h) selects the
 * VRAM region actually shown on screen — so screen = (raw + drawofs - disp.xy)
 * scaled to fill 640x480. This is what positions the menu correctly: its 2D
 * content sits in a part of VRAM that the display window, not the draw clip,
 * defines. Falls back to the draw clip until a display env is set. */
static void RecomputeTransform(void)
{
    int dw = g_activeDispEnv.disp.w;
    int dh = g_activeDispEnv.disp.h;

    if (dw > 0 && dh > 0) {
        s_ofsX   = (float)(g_activeDrawEnv.ofs[0] - g_activeDispEnv.disp.x);
        s_ofsY   = (float)(g_activeDrawEnv.ofs[1] - g_activeDispEnv.disp.y);
        s_scaleX = 640.0f / (float)dw;
        s_scaleY = 480.0f / (float)dh;
    } else {
        s_ofsX   = (float)g_activeDrawEnv.ofs[0];
        s_ofsY   = (float)g_activeDrawEnv.ofs[1];
        s_scaleX = 640.0f / (g_activeDrawEnv.clip.w ? (float)g_activeDrawEnv.clip.w : 320.0f);
        s_scaleY = 480.0f / (g_activeDrawEnv.clip.h ? (float)g_activeDrawEnv.clip.h : 240.0f);
    }
}

DISPENV* PutDispEnv(DISPENV* env)
{
    g_activeDispEnv = *env;
    RecomputeTransform();
    return env;
}

DRAWENV* PutDrawEnv(DRAWENV* env)
{
    g_activeDrawEnv = *env;
    RecomputeTransform();
    /* ([CLR] probe removed: the double-buffered draw envs ping-pong the tuple
     * every PutDrawEnv, which made "log on change" fire 2.3M times in one run.
     * [FOGST-GPU] in FrameBegin covers the clear-colour chain sufficiently.) */
    /* Draw-clip scissor (mirrors PsyCross GR_SetupClipMode's scissorOn): only
     * when the clip is genuinely SMALLER than the display do sub-region draws
     * (map/item screens, refraction regions) get clipped; the common full-
     * display clip resets to the whole surface so normal rendering is
     * untouched. Screen rect goes through the same disp-relative transform as
     * vertices. */
    {
        int dispW = g_activeDispEnv.disp.w > 0 ? g_activeDispEnv.disp.w : 320;
        int dispH = g_activeDispEnv.disp.h > 0 ? g_activeDispEnv.disp.h : 240;
        if (env->clip.w > 0 && env->clip.h > 0 &&
            (env->clip.w < dispW || env->clip.h < dispH)) {
            int sx = (int)(((float)env->clip.x - (float)g_activeDispEnv.disp.x) * s_scaleX);
            int sy = (int)(((float)env->clip.y - (float)g_activeDispEnv.disp.y) * s_scaleY);
            int sw = (int)((float)env->clip.w * s_scaleX + 0.5f);
            int sh = (int)((float)env->clip.h * s_scaleY + 0.5f);
            GpuNv2a_SetScissor(sx, sy, sw, sh);
        } else {
            GpuNv2a_SetScissor(0, 0, 640, 480);
        }
    }
    return env;
}

/* Frame clear colour for GpuNv2a_FrameBegin: the PSX draw-env isbg background
 * (GsSortClear routes background2dColor = fog.color through PutDrawEnv), else
 * opaque black. This is what turns the beyond-fog void into a fog wall. */
unsigned int GpuXbox_GetClearColor(void)
{
    if (g_activeDrawEnv.isbg)
        return 0xFF000000u | ((unsigned)g_activeDrawEnv.r0 << 16)
                           | ((unsigned)g_activeDrawEnv.g0 << 8)
                           |  (unsigned)g_activeDrawEnv.b0;
    return 0xFF000000u;
}

/* ClearImage support (psx_libgpu_xbox.c): draw an immediate flat quad through
 * the current screen transform when the rect intersects the display area, so
 * colour wipes/flashes land in the visible frame (the VRAM-side fill is done
 * by the caller). Blend off; restores no state (per-walk dedup re-establishes). */
void GpuXbox_ClearRectOnScreen(int x, int y, int w, int h, int r, int g, int b)
{
    ShVertex v[4];
    /* Only rects overlapping the DISPLAY area reach the visible frame; clears
     * of off-screen VRAM (texture staging) must not flash on screen. */
    {
        int dx = g_activeDispEnv.disp.x, dy = g_activeDispEnv.disp.y;
        int dw = g_activeDispEnv.disp.w ? g_activeDispEnv.disp.w : 320;
        int dh = g_activeDispEnv.disp.h ? g_activeDispEnv.disp.h : 240;
        if (x >= dx + dw || y >= dy + dh || x + w <= dx || y + h <= dy)
            return;
    }
    GpuNv2a_SetBlendMode(0);
    GpuNv2a_BindWhite();
    s_curBlend = 0;
    s_curTex   = 0;
    /* ClearImage rects are absolute VRAM coords; the screen transform expects
     * draw-offset-relative ones, so strip the active offset first. */
    PutVert(&v[0], x - (int)s_ofsX,     y - (int)s_ofsY,     r, g, b);
    PutVert(&v[1], x + w - (int)s_ofsX, y - (int)s_ofsY,     r, g, b);
    PutVert(&v[2], x - (int)s_ofsX,     y + h - (int)s_ofsY, r, g, b);
    PutVert(&v[3], x + w - (int)s_ofsX, y + h - (int)s_ofsY, r, g, b);
    EmitQuad(&v[0], &v[1], &v[2], &v[3]);
}
