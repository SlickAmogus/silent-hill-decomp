/*
 * libgs_stub.c - GS (Graphics System) library stub implementations
 *
 * These provide PC implementations of PSY-Q libgs functions using PsyCross.
 * The GS library is a higher-level graphics API built on top of libgpu.
 */
#include "common.h"
#include "gpu.h"
#include <libgpu.h>
#include <libgte.h>
#include <libetc.h>
#include <libcd.h>     /* CdlLOC for StGetBackloc stub */
#include <string.h>
#include <stdio.h>
#include <SDL.h>
#include <PsyX/common/glad.h>
#include "sh_log.h"

/* Screenshot helper - captures back buffer (call before EndScene/swap) */
void SH_TakeScreenshot(const char* filename)
{
    extern SDL_Window* g_window;
    int w, h;
    SDL_GetWindowSize(g_window, &w, &h);
    unsigned char* px = (unsigned char*)malloc(w * h * 3);
    glReadBuffer(GL_FRONT);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px);
    /* Flip vertically (OpenGL reads bottom-up) */
    unsigned char* flipped = (unsigned char*)malloc(w * h * 3);
    for (int y = 0; y < h; y++) {
        memcpy(flipped + y * w * 3, px + (h - 1 - y) * w * 3, w * 3);
    }
    SDL_Surface* s = SDL_CreateRGBSurfaceFrom(flipped, w, h, 24, w * 3, 0xFF, 0xFF00, 0xFF0000, 0);
    SDL_SaveBMP(s, filename);
    SDL_FreeSurface(s);
    free(flipped);
    free(px);
    printf("[SH] Screenshot saved: %s (%dx%d)\n", filename, w, h);
}

/* Double-buffered display */
static int gs_active_buff = 0;
static DISPENV gs_disp_env[2];
static DRAWENV gs_draw_env[2];
static int gs_screen_w = 320, gs_screen_h = 240;


/* Packet allocation pointer */
PACKET* GsOUT_PACKET_P = NULL;

/* Lighting state */
static long gs_ambient_r = 0, gs_ambient_g = 0, gs_ambient_b = 0;
static int gs_light_mode = 0;
static GsF_LIGHT gs_lights[3];
static MATRIX gs_light_matrix;

/* VCount emulation - simulate PSX H-blank counter using real time */
#include <SDL.h>
#define H_BLANKS_PER_SECOND 15780
static Uint64 gs_vcount_start = 0;
static int gs_vcount_active = 0;

void GsInitGraph(int x, int y, int mode, int a, int b)
{
    ResetGraph(0);

    gs_screen_w = x;
    gs_screen_h = y;

    /* PC: No VRAM double-buffering. Both buffers use (0,0) with offset at
     * screen center so (0,0) maps to the center of the display — matching
     * PSX SDK behavior where GsDefDispBuff sets ofs = (clip + w/2, clip + h/2). */
    SetDefDispEnv(&gs_disp_env[0], 0, 0, x, y);
    SetDefDispEnv(&gs_disp_env[1], 0, 0, x, y);

    SetDefDrawEnv(&gs_draw_env[0], 0, 0, x, y);
    SetDefDrawEnv(&gs_draw_env[1], 0, 0, x, y);

    /* Center the coordinate system: (0,0) = screen center */
    gs_draw_env[0].ofs[0] = x / 2;
    gs_draw_env[0].ofs[1] = y / 2;
    gs_draw_env[1].ofs[0] = x / 2;
    gs_draw_env[1].ofs[1] = y / 2;

    gs_draw_env[0].isbg = 1;
    gs_draw_env[1].isbg = 1;

    /* On PSX, dfe controls display-during-draw (interlace flicker).
     * PsyCross uses dfe to decide on-screen vs off-screen FBO.
     * Force dfe=1 so all rendering goes to the on-screen target. */
    gs_draw_env[0].dfe = 1;
    gs_draw_env[1].dfe = 1;

    gs_active_buff = 0;

    /* Sync global env structs so game code modifications are seeded correctly */
    GsDRAWENV = gs_draw_env[0];
    GsDISPENV = gs_disp_env[0];
}

/* Forward decls — bodies live further down this file */
void GsTMDfastF3LFG(void* op, VERT* vp, VERT* np, PACKET* pk, int n, int shift, GsOT* ot, unsigned long* scratch);
void GsTMDfastG3LFG(void* op, VERT* vp, VERT* np, PACKET* pk, int n, int shift, GsOT* ot, unsigned long* scratch);
void GsTMDfastF4LFG(void* op, VERT* vp, VERT* np, PACKET* pk, int n, int shift, GsOT* ot, unsigned long* scratch);
void GsTMDfastG4LFG(void* op, VERT* vp, VERT* np, PACKET* pk, int n, int shift, GsOT* ot, unsigned long* scratch);
void GsTMDfastTF3LFG(void* op, VERT* vp, VERT* np, PACKET* pk, int n, int shift, GsOT* ot, unsigned long* scratch);
void GsTMDfastTG3LFG(void* op, VERT* vp, VERT* np, PACKET* pk, int n, int shift, GsOT* ot, unsigned long* scratch);
void GsTMDfastTG4LFG(void* op, VERT* vp, VERT* np, PACKET* pk, int n, int shift, GsOT* ot, unsigned long* scratch);
void GsTMDfastNF3(void* op, VERT* vp, PACKET* pk, int n, int shift, GsOT* ot, unsigned long* scratch);
void GsTMDfastNG3(void* op, VERT* vp, PACKET* pk, int n, int shift, GsOT* ot, unsigned long* scratch);
void GsTMDfastNF4(void* op, VERT* vp, PACKET* pk, int n, int shift, GsOT* ot, unsigned long* scratch);
void GsTMDfastNG4(void* op, VERT* vp, PACKET* pk, int n, int shift, GsOT* ot, unsigned long* scratch);

void GsInit3D(void)
{
    InitGeom();
    /* On PSX, GsInit3D sets geom offset to screen center (160, 120).
     * However, PsyCross adds activeDrawEnv.ofs to every vertex (PsyX_GPU.cpp),
     * and draw env ofs is already set to screen center (160, 112).
     * Setting geom offset to (0, 0) avoids double-centering. */
    SetGeomOffset(0, 0);
    SetGeomScreen(240);

    /* Populate GsFCALL4 dispatch table used by GsSortObject4J.
     * PC has no DIV variants and no separate NORMAL/FOG/LOFF paths — all
     * LMODE slots and both DIV slots point at the same implementation.
     * Textured no-light variants (nt*) intentionally left NULL; the
     * dispatcher null-checks before calling and just skips unknown prim
     * types — items and weapons we need for the inventory are all lit. */
    memset(&GsFCALL4, 0, sizeof(GsFCALL4));
    {
        int d, l;
        for (d = 0; d < 2; d++) {
            for (l = 0; l < 3; l++) {
                GsFCALL4.f3[d][l]  = (void(*)())GsTMDfastF3LFG;
                GsFCALL4.g3[d][l]  = (void(*)())GsTMDfastG3LFG;
                GsFCALL4.f4[d][l]  = (void(*)())GsTMDfastF4LFG;
                GsFCALL4.g4[d][l]  = (void(*)())GsTMDfastG4LFG;
                GsFCALL4.tf3[d][l] = (void(*)())GsTMDfastTF3LFG;
                GsFCALL4.tg3[d][l] = (void(*)())GsTMDfastTG3LFG;
                /* tf4 (textured flat quad) has no impl; dispatcher skips NULL */
                GsFCALL4.tg4[d][l] = (void(*)())GsTMDfastTG4LFG;
            }
            GsFCALL4.nf3[d] = (void(*)())GsTMDfastNF3;
            GsFCALL4.ng3[d] = (void(*)())GsTMDfastNG3;
            GsFCALL4.nf4[d] = (void(*)())GsTMDfastNF4;
            GsFCALL4.ng4[d] = (void(*)())GsTMDfastNG4;
        }
    }
}

void GsInitVcount(void)
{
    gs_vcount_start = SDL_GetPerformanceCounter();
    gs_vcount_active = 1;
}

int GsGetVcount(void)
{
    if (!gs_vcount_active) return 0;
    Uint64 now = SDL_GetPerformanceCounter();
    Uint64 freq = SDL_GetPerformanceFrequency();
    /* Convert elapsed time to H-blank count (15780 per second on PSX) */
    return (int)(((now - gs_vcount_start) * H_BLANKS_PER_SECOND) / freq);
}

void GsClearVcount(void)
{
    gs_vcount_start = SDL_GetPerformanceCounter();
}

void GsDrawOt_ResetFrameCount(void);

void GsSwapDispBuff(void)
{
    gs_active_buff = gs_active_buff ? 0 : 1;
    GsDrawOt_ResetFrameCount();

    /* Sync background color from GsDRAWENV (game modifies this global).
     * Do NOT sync clip or ofs — the game's clip.h=224 override is a PSX
     * interlace optimization (one field at a time). On PC we render the
     * full 448-line space with ofs at center. */
    gs_draw_env[gs_active_buff].isbg = GsDRAWENV.isbg;
    gs_draw_env[gs_active_buff].r0 = GsDRAWENV.r0;
    gs_draw_env[gs_active_buff].g0 = GsDRAWENV.g0;
    gs_draw_env[gs_active_buff].b0 = GsDRAWENV.b0;
    gs_draw_env[gs_active_buff].dfe = 1; /* Always force on-screen */

    PutDispEnv(&gs_disp_env[gs_active_buff]);
    PutDrawEnv(&gs_draw_env[gs_active_buff]);
}

int GsGetActiveBuff(void)
{
    return gs_active_buff;
}

static int gs_drawot_call_count = 0;

void GsDrawOt(GsOT *ot)
{
    if (ot && ot->tag)
    {
#ifdef SH_PC_PORT
        /* PsyCross bug: PsyX_EndScene is never called (commented out in DrawSync),
         * so GR_BeginScene only runs on the first frame. Clear depth/stencil each
         * draw call so geometry renders correctly. */
        glClearDepth(1.0f);
        glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
#endif
        DrawOTag((u_long*)ot->tag);
    }
}

void GsDrawOt_ResetFrameCount(void)
{
    gs_drawot_call_count = 0;
}

void GsClearOt(int offset, int point, GsOT *ot)
{
    if (ot && ot->org)
    {
        int n = 1 << ot->length;
        ClearOTagR((u_long*)ot->org, n);
        ot->tag = &ot->org[n - 1];
    }
}

void GsSortClear(unsigned char r, unsigned char g, unsigned char b, GsOT *ot)
{
    gs_draw_env[gs_active_buff].r0 = r;
    gs_draw_env[gs_active_buff].g0 = g;
    gs_draw_env[gs_active_buff].b0 = b;
    gs_draw_env[gs_active_buff].isbg = 1;
    /* Push updated clear color to PsyCross so PsyX_BeginScene uses it */
    PutDrawEnv(&gs_draw_env[gs_active_buff]);
}

void GsSetAmbient(long r, long g, long b)
{
    gs_ambient_r = r;
    gs_ambient_g = g;
    gs_ambient_b = b;
    SetBackColor(r >> 4, g >> 4, b >> 4);
}

void GsSetFlatLight(int id, GsF_LIGHT *lt)
{
    if (id >= 0 && id < 3 && lt)
    {
        gs_lights[id] = *lt;
    }
}

void GsSetLightMode(int mode)
{
    gs_light_mode = mode;
}

void GsSetLightMatrix(MATRIX *m)
{
    if (m)
    {
        gs_light_matrix = *m;
        SetLightMatrix(m);
    }
}

void GsSetView2(GsVIEW2 *v)
{
    if (v)
    {
        SetRotMatrix(&v->view);
        SetTransMatrix(&v->view);
    }
}

void GsSetRefView2(GsRVIEW2 *rv)
{
    /* TODO: Compute view matrix from reference view parameters */
    (void)rv;
}

void GsSetProjection(long h)
{
    SetGeomScreen(h);
}

/* =====================================================================
 * TMD cache — resolves 32-bit PSX TMD layout to 64-bit TMD_STRUCT
 * ===================================================================== */
#define GS_TMD_CACHE_SLOTS  8
#define GS_TMD_MAX_OBJS    48
 
typedef struct {
    u_long          *base;  /* key: pointer passed to GsMapModelingData */
    int              nobj;
    struct TMD_STRUCT objs[GS_TMD_MAX_OBJS];
} GsTmdCacheEntry;
 
static GsTmdCacheEntry gs_tmd_cache[GS_TMD_CACHE_SLOTS];
static int gs_tmd_cache_count = 0;
 
struct TMD_STRUCT* GsGetTMDObject(u_long *base, int index)
{
    int i;
    SH_DBG("[GSGET] base=%p idx=%d cache_count=%d", (void*)base, index, gs_tmd_cache_count);
    for (i = 0; i < gs_tmd_cache_count; i++) {
        if (gs_tmd_cache[i].base == base && index < gs_tmd_cache[i].nobj) {
            struct TMD_STRUCT *o = &gs_tmd_cache[i].objs[index];
            SH_DBG("[GSGET] hit slot=%d nobj=%d vertop=%p vern=%lu primtop=%p primn=%lu",
                   i, gs_tmd_cache[i].nobj, (void*)o->vertop, (unsigned long)o->vern,
                   (void*)o->primtop, (unsigned long)o->primn);
            return o;
        }
    }
    SH_DBG("[GSGET] miss");
    return NULL;
}
 
/* =====================================================================
 * TMD primitive rendering helpers
 * ===================================================================== */
 
/* Clamp s32 to u8 */
static inline unsigned char clamp8(int v) { return (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v)); }
 
/* Flat-shaded triangle — lit + fog */
void GsTMDfastF3LFG(void* op, VERT* vp, VERT* np, PACKET* pk, int n, int shift, GsOT* ot, unsigned long* scratch)
{
    TMD_P_F3 *prim = (TMD_P_F3*)op;
    SVECTOR  *vtx  = (SVECTOR*)vp;
    SVECTOR  *nrm  = (SVECTOR*)np;
    int i;
    (void)scratch;
 
    for (i = 0; i < n; i++, prim++) {
        long sxy0, sxy1, sxy2, p, flg, otz, nclip;
        CVECTOR col_in, col_out;
        POLY_F3 *poly;
 
        RotTransPers3(&vtx[prim->v0], &vtx[prim->v1], &vtx[prim->v2],
                      &sxy0, &sxy1, &sxy2, &p, &flg);
        nclip = NormalClip(sxy0, sxy1, sxy2);
        if (nclip <= 0) continue;
 
        col_in.r = prim->r0; col_in.g = prim->g0; col_in.b = prim->b0; col_in.cd = prim->code;
        NormalColorCol(&nrm[prim->n0], &col_in, &col_out);
 
        otz = p >> shift;
        if (otz <= 0 || otz >= (1 << ot->length)) continue;
 
        poly = (POLY_F3*)GsOUT_PACKET_P;
        setPolyF3(poly);
        setRGB0(poly, col_out.r, col_out.g, col_out.b);
        *(long*)&poly->x0 = sxy0; *(long*)&poly->x1 = sxy1; *(long*)&poly->x2 = sxy2;
        addPrim(&ot->org[otz], poly);
        GsOUT_PACKET_P = (u8*)poly + sizeof(POLY_F3);
    }
}
 
/* Gouraud-shaded triangle — lit + fog */
void GsTMDfastG3LFG(void* op, VERT* vp, VERT* np, PACKET* pk, int n, int shift, GsOT* ot, unsigned long* scratch)
{
    TMD_P_G3 *prim = (TMD_P_G3*)op;
    SVECTOR  *vtx  = (SVECTOR*)vp;
    SVECTOR  *nrm  = (SVECTOR*)np;
    int i;
    (void)pk; (void)scratch;
 
    for (i = 0; i < n; i++, prim++) {
        long sxy0, sxy1, sxy2, p, flg, otz, nclip;
        CVECTOR col_in, c0, c1, c2;
        POLY_G3 *poly;
 
        RotTransPers3(&vtx[prim->v0], &vtx[prim->v1], &vtx[prim->v2],
                      &sxy0, &sxy1, &sxy2, &p, &flg);
        nclip = NormalClip(sxy0, sxy1, sxy2);
        if (nclip <= 0) continue;
 
        col_in.r = prim->r0; col_in.g = prim->g0; col_in.b = prim->b0; col_in.cd = prim->code;
        NormalColorCol3(&nrm[prim->n0], &nrm[prim->n1], &nrm[prim->n2],
                        &col_in, &c0, &c1, &c2);
 
        otz = p >> shift;
        if (otz <= 0 || otz >= (1 << ot->length)) continue;
 
        poly = (POLY_G3*)GsOUT_PACKET_P;
        setPolyG3(poly);
        setRGB0(poly, c0.r, c0.g, c0.b);
        setRGB1(poly, c1.r, c1.g, c1.b);
        setRGB2(poly, c2.r, c2.g, c2.b);
        *(long*)&poly->x0 = sxy0; *(long*)&poly->x1 = sxy1; *(long*)&poly->x2 = sxy2;
        addPrim(&ot->org[otz], poly);
        GsOUT_PACKET_P = (u8*)poly + sizeof(POLY_G3);
    }
}
 
/* Flat-shaded quad — lit + fog */
void GsTMDfastF4LFG(void* op, VERT* vp, VERT* np, PACKET* pk, int n, int shift, GsOT* ot, unsigned long* scratch)
{
    TMD_P_F4 *prim = (TMD_P_F4*)op;
    SVECTOR  *vtx  = (SVECTOR*)vp;
    SVECTOR  *nrm  = (SVECTOR*)np;
    int i;
    (void)pk; (void)scratch;
 
    for (i = 0; i < n; i++, prim++) {
        long sxy0, sxy1, sxy2, sxy3, p, flg, otz, nclip;
        CVECTOR col_in, col_out;
        POLY_F4 *poly;
 
        RotTransPers3(&vtx[prim->v0], &vtx[prim->v1], &vtx[prim->v2],
                      &sxy0, &sxy1, &sxy2, &p, &flg);
        RotTransPers(&vtx[prim->v3], (int*)&sxy3, &p, &flg);
        nclip = NormalClip(sxy0, sxy1, sxy2);
        if (nclip <= 0) continue;
 
        col_in.r = prim->r0; col_in.g = prim->g0; col_in.b = prim->b0; col_in.cd = prim->code;
        NormalColorCol(&nrm[prim->n0], &col_in, &col_out);
 
        otz = p >> shift;
        if (otz <= 0 || otz >= (1 << ot->length)) continue;
 
        poly = (POLY_F4*)GsOUT_PACKET_P;
        setPolyF4(poly);
        setRGB0(poly, col_out.r, col_out.g, col_out.b);
        *(long*)&poly->x0 = sxy0; *(long*)&poly->x1 = sxy1;
        *(long*)&poly->x2 = sxy2; *(long*)&poly->x3 = sxy3;
        addPrim(&ot->org[otz], poly);
        GsOUT_PACKET_P = (u8*)poly + sizeof(POLY_F4);
    }
}
 
/* Gouraud-shaded quad — lit + fog (untextured) */
void GsTMDfastG4LFG(void* op, VERT* vp, VERT* np, PACKET* pk, int n, int shift, GsOT* ot, unsigned long* scratch)
{
    TMD_P_G4 *prim = (TMD_P_G4*)op;
    SVECTOR  *vtx  = (SVECTOR*)vp;
    SVECTOR  *nrm  = (SVECTOR*)np;
    int i;
    (void)pk; (void)scratch;
 
    for (i = 0; i < n; i++, prim++) {
        long sxy0, sxy1, sxy2, sxy3, p, flg, otz, nclip;
        CVECTOR col_in, c0, c1, c2;
        POLY_G4 *poly;
 
        RotTransPers3(&vtx[prim->v0], &vtx[prim->v1], &vtx[prim->v2],
                      &sxy0, &sxy1, &sxy2, &p, &flg);
        RotTransPers(&vtx[prim->v3], (int*)&sxy3, &p, &flg);
        nclip = NormalClip(sxy0, sxy1, sxy2);
        if (nclip <= 0) continue;
 
        col_in.r = prim->r0; col_in.g = prim->g0; col_in.b = prim->b0; col_in.cd = prim->code;
        NormalColorCol3(&nrm[prim->n0], &nrm[prim->n1], &nrm[prim->n2],
                        &col_in, &c0, &c1, &c2);
 
        otz = p >> shift;
        if (otz <= 0 || otz >= (1 << ot->length)) continue;
 
        poly = (POLY_G4*)GsOUT_PACKET_P;
        setPolyG4(poly);
        setRGB0(poly, c0.r, c0.g, c0.b);
        setRGB1(poly, c1.r, c1.g, c1.b);
        setRGB2(poly, c2.r, c2.g, c2.b);
        /* v3 gets c2 color (PSX convention for quads) */
        setRGB3(poly, c2.r, c2.g, c2.b);
        *(long*)&poly->x0 = sxy0; *(long*)&poly->x1 = sxy1;
        *(long*)&poly->x2 = sxy2; *(long*)&poly->x3 = sxy3;
        addPrim(&ot->org[otz], poly);
        GsOUT_PACKET_P = (u8*)poly + sizeof(POLY_G4);
    }
}
 
/* Textured flat-shaded triangle — lit + fog */
void GsTMDfastTF3LFG(void* op, VERT* vp, VERT* np, PACKET* pk, int n, int shift, GsOT* ot, unsigned long* scratch)
{
    TMD_P_TF3 *prim = (TMD_P_TF3*)op;
    SVECTOR   *vtx  = (SVECTOR*)vp;
    SVECTOR   *nrm  = (SVECTOR*)np;
    int i;
    (void)pk; (void)scratch;
 
    for (i = 0; i < n; i++, prim++) {
        long sxy0, sxy1, sxy2, p, flg, otz, nclip;
        CVECTOR col_in, col_out;
        POLY_FT3 *poly;
 
        RotTransPers3(&vtx[prim->v0], &vtx[prim->v1], &vtx[prim->v2],
                      &sxy0, &sxy1, &sxy2, &p, &flg);
        nclip = NormalClip(sxy0, sxy1, sxy2);
        if (nclip <= 0) continue;
 
        /* Textured prims have no base color; use neutral 0x80 (half-bright
         * so NormalColorCol output stays in-range after lighting). */
        col_in.r = 0x80; col_in.g = 0x80; col_in.b = 0x80; col_in.cd = prim->cd;
        NormalColorCol(&nrm[prim->n0], &col_in, &col_out);

        otz = p >> shift;
        if (otz <= 0 || otz >= (1 << ot->length)) continue;

        poly = (POLY_FT3*)GsOUT_PACKET_P;
        setPolyFT3(poly);
        setRGB0(poly, col_out.r, col_out.g, col_out.b);
        setUV3(poly, prim->tu0, prim->tv0, prim->tu1, prim->tv1, prim->tu2, prim->tv2);
        poly->tpage = prim->tpage;
        poly->clut  = prim->clut;
        *(long*)&poly->x0 = sxy0; *(long*)&poly->x1 = sxy1; *(long*)&poly->x2 = sxy2;
        addPrim(&ot->org[otz], poly);
        GsOUT_PACKET_P = (u8*)poly + sizeof(POLY_FT3);
    }
}
 
/* Textured Gouraud-shaded triangle — lit + fog */
void GsTMDfastTG3LFG(void* op, VERT* vp, VERT* np, PACKET* pk, int n, int shift, GsOT* ot, unsigned long* scratch)
{
    TMD_P_TG3 *prim = (TMD_P_TG3*)op;
    SVECTOR   *vtx  = (SVECTOR*)vp;
    SVECTOR   *nrm  = (SVECTOR*)np;
    int i;
    (void)pk; (void)scratch;
 
    for (i = 0; i < n; i++, prim++) {
        long sxy0, sxy1, sxy2, p, flg, otz, nclip;
        CVECTOR col_in, c0, c1, c2;
        POLY_GT3 *poly;
 
        RotTransPers3(&vtx[prim->v0], &vtx[prim->v1], &vtx[prim->v2],
                      &sxy0, &sxy1, &sxy2, &p, &flg);
        nclip = NormalClip(sxy0, sxy1, sxy2);
        if (nclip <= 0) continue;
 
        col_in.r = 0x80; col_in.g = 0x80; col_in.b = 0x80; col_in.cd = prim->cd;
        NormalColorCol3(&nrm[prim->n0], &nrm[prim->n1], &nrm[prim->n2],
                        &col_in, &c0, &c1, &c2);

        otz = p >> shift;
        if (otz <= 0 || otz >= (1 << ot->length)) continue;

        poly = (POLY_GT3*)GsOUT_PACKET_P;
        setPolyGT3(poly);
        setRGB0(poly, c0.r, c0.g, c0.b);
        setRGB1(poly, c1.r, c1.g, c1.b);
        setRGB2(poly, c2.r, c2.g, c2.b);
        setUV3(poly, prim->tu0, prim->tv0, prim->tu1, prim->tv1, prim->tu2, prim->tv2);
        poly->tpage = prim->tpage;
        poly->clut  = prim->clut;
        *(long*)&poly->x0 = sxy0; *(long*)&poly->x1 = sxy1; *(long*)&poly->x2 = sxy2;
        addPrim(&ot->org[otz], poly);
        GsOUT_PACKET_P = (u8*)poly + sizeof(POLY_GT3);
    }
}
 
/* Textured Gouraud-shaded quad — lit + fog.
 * NOTE: agent originally named this TF4LFG but the body parses TMD_P_TG4
 * (per-vertex normals + gouraud). Renamed to match its actual behavior;
 * the tf4 dispatch slot stays NULL (no real textured-flat-quad impl). */
void GsTMDfastTG4LFG(void* op, VERT* vp, VERT* np, PACKET* pk, int n, int shift, GsOT* ot, unsigned long* scratch)
{
    TMD_P_TG4 *prim = (TMD_P_TG4*)op;
    SVECTOR   *vtx  = (SVECTOR*)vp;
    SVECTOR   *nrm  = (SVECTOR*)np;
    int i;
    (void)pk; (void)scratch;
 
    for (i = 0; i < n; i++, prim++) {
        long sxy0, sxy1, sxy2, sxy3, p, flg, otz, nclip;
        CVECTOR col_in, c0, c1, c2;
        POLY_GT4 *poly;
 
        RotTransPers3(&vtx[prim->v0], &vtx[prim->v1], &vtx[prim->v2],
                      &sxy0, &sxy1, &sxy2, &p, &flg);
        RotTransPers(&vtx[prim->v3], (int*)&sxy3, &p, &flg);
        nclip = NormalClip(sxy0, sxy1, sxy2);
        if (nclip <= 0) continue;
 
        col_in.r = 0x80; col_in.g = 0x80; col_in.b = 0x80; col_in.cd = prim->cd;
        NormalColorCol3(&nrm[prim->n0], &nrm[prim->n1], &nrm[prim->n2],
                        &col_in, &c0, &c1, &c2);

        otz = p >> shift;
        if (otz <= 0 || otz >= (1 << ot->length)) continue;

        poly = (POLY_GT4*)GsOUT_PACKET_P;
        setPolyGT4(poly);
        setRGB0(poly, c0.r, c0.g, c0.b);
        setRGB1(poly, c1.r, c1.g, c1.b);
        setRGB2(poly, c2.r, c2.g, c2.b);
        setRGB3(poly, c2.r, c2.g, c2.b);
        setUV4(poly, prim->tu0, prim->tv0, prim->tu1, prim->tv1,
                     prim->tu2, prim->tv2, prim->tu3, prim->tv3);
        poly->tpage = prim->tpage;
        poly->clut  = prim->clut;
        *(long*)&poly->x0 = sxy0; *(long*)&poly->x1 = sxy1;
        *(long*)&poly->x2 = sxy2; *(long*)&poly->x3 = sxy3;
        addPrim(&ot->org[otz], poly);
        GsOUT_PACKET_P = (u8*)poly + sizeof(POLY_GT4);
    }
}
 
/* No-light flat tri */
void GsTMDfastNF3(void* op, VERT* vp, PACKET* pk, int n, int shift, GsOT* ot, unsigned long* scratch)
{
    TMD_P_NF3 *prim = (TMD_P_NF3*)op;
    SVECTOR   *vtx  = (SVECTOR*)vp;
    int i;
    (void)pk; (void)scratch;
 
    for (i = 0; i < n; i++, prim++) {
        long sxy0, sxy1, sxy2, p, flg, otz, nclip;
        POLY_F3 *poly;
 
        RotTransPers3(&vtx[prim->v0], &vtx[prim->v1], &vtx[prim->v2],
                      &sxy0, &sxy1, &sxy2, &p, &flg);
        nclip = NormalClip(sxy0, sxy1, sxy2);
        if (nclip <= 0) continue;
 
        otz = p >> shift;
        if (otz <= 0 || otz >= (1 << ot->length)) continue;
 
        poly = (POLY_F3*)GsOUT_PACKET_P;
        setPolyF3(poly);
        setRGB0(poly, prim->r0, prim->g0, prim->b0);
        *(long*)&poly->x0 = sxy0; *(long*)&poly->x1 = sxy1; *(long*)&poly->x2 = sxy2;
        addPrim(&ot->org[otz], poly);
        GsOUT_PACKET_P = (u8*)poly + sizeof(POLY_F3);
    }
}
 
/* No-light gouraud tri */
void GsTMDfastNG3(void* op, VERT* vp, PACKET* pk, int n, int shift, GsOT* ot, unsigned long* scratch)
{
    TMD_P_NG3 *prim = (TMD_P_NG3*)op;
    SVECTOR   *vtx  = (SVECTOR*)vp;
    int i;
    (void)pk; (void)scratch;
 
    for (i = 0; i < n; i++, prim++) {
        long sxy0, sxy1, sxy2, p, flg, otz, nclip;
        POLY_G3 *poly;
 
        RotTransPers3(&vtx[prim->v0], &vtx[prim->v1], &vtx[prim->v2],
                      &sxy0, &sxy1, &sxy2, &p, &flg);
        nclip = NormalClip(sxy0, sxy1, sxy2);
        if (nclip <= 0) continue;
 
        otz = p >> shift;
        if (otz <= 0 || otz >= (1 << ot->length)) continue;
 
        poly = (POLY_G3*)GsOUT_PACKET_P;
        setPolyG3(poly);
        setRGB0(poly, prim->r0, prim->g0, prim->b0);
        setRGB1(poly, prim->r1, prim->g1, prim->b1);
        setRGB2(poly, prim->r2, prim->g2, prim->b2);
        *(long*)&poly->x0 = sxy0; *(long*)&poly->x1 = sxy1; *(long*)&poly->x2 = sxy2;
        addPrim(&ot->org[otz], poly);
        GsOUT_PACKET_P = (u8*)poly + sizeof(POLY_G3);
    }
}
 
/* No-light flat quad */
void GsTMDfastNF4(void* op, VERT* vp, PACKET* pk, int n, int shift, GsOT* ot, unsigned long* scratch)
{
    TMD_P_NF4 *prim = (TMD_P_NF4*)op;
    SVECTOR   *vtx  = (SVECTOR*)vp;
    int i;
    (void)pk; (void)scratch;
 
    for (i = 0; i < n; i++, prim++) {
        long sxy0, sxy1, sxy2, sxy3, p, flg, otz, nclip;
        POLY_F4 *poly;
 
        RotTransPers3(&vtx[prim->v0], &vtx[prim->v1], &vtx[prim->v2],
                      &sxy0, &sxy1, &sxy2, &p, &flg);
        RotTransPers(&vtx[prim->v3], (int*)&sxy3, &p, &flg);
        nclip = NormalClip(sxy0, sxy1, sxy2);
        if (nclip <= 0) continue;
 
        otz = p >> shift;
        if (otz <= 0 || otz >= (1 << ot->length)) continue;
 
        poly = (POLY_F4*)GsOUT_PACKET_P;
        setPolyF4(poly);
        setRGB0(poly, prim->r0, prim->g0, prim->b0);
        *(long*)&poly->x0 = sxy0; *(long*)&poly->x1 = sxy1;
        *(long*)&poly->x2 = sxy2; *(long*)&poly->x3 = sxy3;
        addPrim(&ot->org[otz], poly);
        GsOUT_PACKET_P = (u8*)poly + sizeof(POLY_F4);
    }
}
 
/* No-light gouraud quad */
void GsTMDfastNG4(void* op, VERT* vp, PACKET* pk, int n, int shift, GsOT* ot, unsigned long* scratch)
{
    TMD_P_NG4 *prim = (TMD_P_NG4*)op;
    SVECTOR   *vtx  = (SVECTOR*)vp;
    int i;
    (void)pk; (void)scratch;
 
    for (i = 0; i < n; i++, prim++) {
        long sxy0, sxy1, sxy2, sxy3, p, flg, otz, nclip;
        POLY_G4 *poly;
 
        RotTransPers3(&vtx[prim->v0], &vtx[prim->v1], &vtx[prim->v2],
                      &sxy0, &sxy1, &sxy2, &p, &flg);
        RotTransPers(&vtx[prim->v3], (int*)&sxy3, &p, &flg);
        nclip = NormalClip(sxy0, sxy1, sxy2);
        if (nclip <= 0) continue;
 
        otz = p >> shift;
        if (otz <= 0 || otz >= (1 << ot->length)) continue;
 
        poly = (POLY_G4*)GsOUT_PACKET_P;
        setPolyG4(poly);
        setRGB0(poly, prim->r0, prim->g0, prim->b0);
        setRGB1(poly, prim->r1, prim->g1, prim->b1);
        setRGB2(poly, prim->r2, prim->g2, prim->b2);
        setRGB3(poly, prim->r3, prim->g3, prim->b3);
        *(long*)&poly->x0 = sxy0; *(long*)&poly->x1 = sxy1;
        *(long*)&poly->x2 = sxy2; *(long*)&poly->x3 = sxy3;
        addPrim(&ot->org[otz], poly);
        GsOUT_PACKET_P = (u8*)poly + sizeof(POLY_G4);
    }

}

void GsSortObject4(GsDOBJ2 *obj, GsOT *ot, int shift, unsigned long *scratch)
{
    /* TODO: Implement TMD object sorting and rendering */
    (void)obj; (void)ot; (void)shift; (void)scratch;
}

void GsGetLw(GsCOORDINATE2 *coord, MATRIX *m)
{
    if (coord && m)
    {
        *m = coord->coord;
    }
}

void GsGetLs(GsCOORDINATE2 *coord, MATRIX *m)
{
    if (coord && m)
    {
        *m = coord->coord;
    }
}

void GsGetLws(GsCOORDINATE2 *coord, MATRIX *lw, MATRIX *ls)
{
    GsGetLw(coord, lw);
    GsGetLs(coord, ls);
}

void SetPriority(PACKET* p, int a, int b)
{
    /* SetPriority creates a DR_MODE that sets the ABR (semi-transparency rate).
     * We must set len=0 because SetDrawMode sets len=3, but DR_MODE only has
     * code[2]. PsyCross's ProcessDrawEnv reads `len` words, so len=3 would
     * read past the struct causing garbage GP0 commands and crashes.
     * Setting len=0 makes this an OT pass-through node. The tpage/ABR is
     * instead handled by the adjacent SetDrawMode calls in the game code.
     *
     * BUT we also need to clear the code byte. Otherwise PsyCross's
     * ParsePrimitivesLinkedList reads the leftover code from whatever was
     * previously at this packet-buffer location, dispatches to that prim
     * type's processor (e.g. POLY_F3 returns 4 longs), and advances 20
     * bytes — past the 4-byte tagLength endpacket. Result: diff=-16
     * errors and eventually corrupted vertex/split state that crashes
     * GsDrawOt. Set code=0xFF so primType=0xF0 hits the default switch
     * case in ParsePrimitive, which returns 0 and breaks the inner
     * loop cleanly. */
    setlen(p, 0);
    setcode(p, 0xFF);
    (void)a; (void)b;
}

_GsFCALL GsFCALL4;

/* Global matrices */
MATRIX GsWSMATRIX;
MATRIX GsWSMATRIX_ORG;
/* Identity matrix (Q12 fixed-point: 4096 = 1.0) */
MATRIX GsIDMATRIX = {
    {{4096, 0, 0}, {0, 4096, 0}, {0, 0, 4096}},
    {0, 0, 0}
};
/* Identity matrix with NTSC aspect ratio correction.
 * PSX NTSC pixels are ~1.094x taller than wide (320x240 displayed as 4:3).
 * Y scale = 4096 * 3/4 = 3072 (PSX libgs convention). */
MATRIX GsIDMATRIX2 = {
    {{4096, 0, 0}, {0, 3072, 0}, {0, 0, 4096}},
    {0, 0, 0}
};

/* Global GS state */
unsigned long GsLMODE, GsLIGNR, GsLIOFF, GsZOVER, GsBACKC, GsNDIV;

/* Display/draw environment globals */
DISPENV GsDISPENV;
DRAWENV GsDRAWENV;

void GsInitGraph2(unsigned short x, unsigned short y, unsigned short intmode, unsigned short dith, unsigned short vrammode)
{
    GsInitGraph(x, y, intmode, dith, vrammode);
}

void GsDefDispBuff2(unsigned short x0, unsigned short y0, unsigned short x1, unsigned short y1)
{
    /* PC: No VRAM double-buffering. Both buffers use (0,0) with offset at
     * screen center. CLUT/texture data in VRAM is protected by skipping
     * GR_ClearVRAM in ClearImage (via PSYX_SKIP_FRAMEBUFFER_STORE). */
    SetDefDispEnv(&gs_disp_env[0], 0, 0, gs_screen_w, gs_screen_h);
    SetDefDispEnv(&gs_disp_env[1], 0, 0, gs_screen_w, gs_screen_h);
    SetDefDrawEnv(&gs_draw_env[0], 0, 0, gs_screen_w, gs_screen_h);
    SetDefDrawEnv(&gs_draw_env[1], 0, 0, gs_screen_w, gs_screen_h);
    gs_draw_env[0].ofs[0] = gs_screen_w / 2;
    gs_draw_env[0].ofs[1] = gs_screen_h / 2;
    gs_draw_env[1].ofs[0] = gs_screen_w / 2;
    gs_draw_env[1].ofs[1] = gs_screen_h / 2;
    gs_draw_env[0].isbg = 1;
    gs_draw_env[1].isbg = 1;
    gs_draw_env[0].dfe = 1;
    gs_draw_env[1].dfe = 1;

    /* Sync global env structs */
    GsDRAWENV = gs_draw_env[0];
    GsDISPENV = gs_disp_env[0];
}

/* PSY-Q libgte TransposeMatrix - not implemented in PsyCross */
MATRIX* TransposeMatrix(MATRIX *m0, MATRIX *m1)
{
    m1->m[0][0] = m0->m[0][0]; m1->m[0][1] = m0->m[1][0]; m1->m[0][2] = m0->m[2][0];
    m1->m[1][0] = m0->m[0][1]; m1->m[1][1] = m0->m[1][1]; m1->m[1][2] = m0->m[2][1];
    m1->m[2][0] = m0->m[0][2]; m1->m[2][1] = m0->m[1][2]; m1->m[2][2] = m0->m[2][2];
    return m1;
}

/* PSY-Q libgte VectorNormal - not implemented in PsyCross. PSX original
 * runs on the GTE; we just compute it in C. v0 is the input vector;
 * v1 receives the normalized result scaled to Q12 (where 1.0 = 4096).
 * Returns the squared magnitude (matches PSX register semantics — the
 * one consumer in our codebase, Vc_StereoBalanceGet, ignores the
 * return value, so the exact return matches PSX docs but isn't
 * load-bearing). */
long VectorNormal(VECTOR *v0, VECTOR *v1)
{
    long sq = (long)v0->vx * v0->vx
            + (long)v0->vy * v0->vy
            + (long)v0->vz * v0->vz;
    long mag = SquareRoot0((int)sq);
    if (mag != 0) {
        v1->vx = ((long)v0->vx << 12) / mag;
        v1->vy = ((long)v0->vy << 12) / mag;
        v1->vz = ((long)v0->vz << 12) / mag;
    } else {
        v1->vx = 0;
        v1->vy = 0;
        v1->vz = 0;
    }
    v1->pad = 0;
    return sq;
}

/* ==========================================================================
 * libcd streaming (St*) and libpress FMV decoder (Dec*) no-op stubs.
 *
 * These were ALL stubbed as `u8 NAME[256] = {0}` in data_stubs.c, same
 * latent NX-fault bug class as VectorNormal. None of them are reached
 * in normal PC gameplay (FMV is handled by fmv_player.cpp / xa_player.c)
 * but ANY path that triggers the original Stream_* machinery in
 * screens/stream/stream.c would jump into .data and crash. No-op
 * implementations are safe because the surrounding code already gates
 * actual playback through the PC FMV path.
 *
 * Signatures pulled from pc_port/PsyCross/include/psx/libcd.h:203-217
 * and include/psyq/libpress.h:8-12,61.
 * ==========================================================================*/

void StSetRing(u_int* ring_addr, u_int ring_size) { (void)ring_addr; (void)ring_size; }
void StUnSetRing(void) {}
void StSetStream(u_int mode, u_int start_frame, u_int end_frame,
                 void (*func1)(), void (*func2)()) {
    (void)mode; (void)start_frame; (void)end_frame; (void)func1; (void)func2;
}
u_int StFreeRing(u_int* base) { (void)base; return 0; }
u_int StGetNext(u_int** addr, u_int** header) {
    (void)addr; (void)header; return 0;
}
void StCdInterrupt(void) {}
int StGetBackloc(CdlLOC* loc) { (void)loc; return 0; }

void DecDCTReset(int mode) { (void)mode; }
void DecDCTvlc(u_int* bs, u_int* buf) { (void)bs; (void)buf; }
void DecDCTin(u_int* buf, int mode) { (void)buf; (void)mode; }
void DecDCTout(u_int* buf, int size) { (void)buf; (void)size; }
int DecDCToutCallback(void (*func)()) { (void)func; return 0; }

void GsInitCoordinate2(void *super, GsCOORDINATE2 *coord)
{
    if (coord) {
        memset(coord, 0, sizeof(GsCOORDINATE2));
        /* Identity matrix: diagonal = 4096 (Q12 1.0) */
        coord->coord.m[0][0] = 4096;
        coord->coord.m[1][1] = 4096;
        coord->coord.m[2][2] = 4096;
        coord->super = (GsCOORDINATE2*)super;
        /* If no parent (root of hierarchy), mark as pre-computed so
         * Vw_CoordHierarchyMatrixCompute stops traversal here. */
        coord->flg = (super == NULL) ? 1 : 0;
    }
}

void GsLinkObject4(unsigned long tmd_base, GsDOBJ2 *objp, int n)
{
    /* On PC, tmd_base is a TMD_STRUCT* cast to unsigned long.
     * Since unsigned long is 32 bits on Windows, the caller must use
     * the PC-specific GsLinkObject4_PC wrapper for 64-bit pointers. */
    (void)tmd_base; (void)objp; (void)n;
}
 
void GsLinkObject4_PC(struct TMD_STRUCT *tmd, GsDOBJ2 *obj)
{
    SH_DBG("[GSLINK] tmd=%p obj=%p", (void*)tmd, (void*)obj);
    if (tmd && obj) {
        obj->tmd = (u_long*)tmd;
    }
}
 
void GsMapModelingData(unsigned long *p)
{
    SH_DBG("[GSMAP] p=%p p[0]=0x%lx p[1]=0x%lx", (void*)p,
           p ? (unsigned long)p[0] : 0UL, p ? (unsigned long)p[1] : 0UL);
    u_long nobj;
    u8    *obj_table;
    int    i, slot;
    GsTmdCacheEntry *entry;
 
    if (!p) return;
    /* p[0] = flags, p[1] = nobj */
    nobj = p[1];
    if (nobj == 0 || nobj > GS_TMD_MAX_OBJS) return;
 
    /* Object table starts right after flags + nobj (2 u_longs = 8 bytes) */
    obj_table = (u8*)&p[2];
 
    /* Find existing slot or allocate new one */
    slot = -1;
    for (i = 0; i < gs_tmd_cache_count; i++) {
        if (gs_tmd_cache[i].base == p) { slot = i; break; }
    }
    if (slot < 0) {
        if (gs_tmd_cache_count >= GS_TMD_CACHE_SLOTS) slot = 0;
        else slot = gs_tmd_cache_count++;
    }
 
    entry = &gs_tmd_cache[slot];
    entry->base = p;
    entry->nobj = (int)nobj;
 
    /* Parse raw 28-byte TMD objects (7 × u32).
     * On PSX, TMD_STRUCT has 32-bit pointer fields matching the file layout.
     * On 64-bit PC, pointer fields are 8 bytes, so we parse manually. */
    for (i = 0; i < (int)nobj; i++) {
        u32 *raw = (u32*)(obj_table + i * 28);
        /* raw[0..6] = vertop_off, vern, nortop_off, norn, primtop_off, primn, scale
         * Offsets are relative to the object table start. */
        entry->objs[i].vertop  = (u_long*)(obj_table + raw[0]);
        entry->objs[i].vern    = raw[1];
        entry->objs[i].nortop  = (u_long*)(obj_table + raw[2]);
        entry->objs[i].norn    = raw[3];
        entry->objs[i].primtop = (u_long*)(obj_table + raw[4]);
        entry->objs[i].primn   = raw[5];
        entry->objs[i].scale   = raw[6];
    }
}

void GsSetLsMatrix(MATRIX *m)
{
    if (m) {
        SetRotMatrix(m);
        SetTransMatrix(m);
    }
}

void GsSortFastSprite(GsSPRITE *spr, GsOT *ot, int shift)
{
    (void)spr; (void)ot; (void)shift;
}

void GsSortObject4J(GsDOBJ2 *obj, GsOT *ot, int shift, unsigned long *scratch)
{
    struct TMD_STRUCT *tmd;
    SVECTOR *vp, *np;
    u8      *pp;
    int      primn;
    int      divmode = 0; /* always NDIV for now */
    int      lmode   = gs_light_mode;
    PACKET  *pk;

    if (!obj || !obj->tmd || !ot) { return; }

    tmd = (struct TMD_STRUCT*)obj->tmd;
    vp  = (SVECTOR*)tmd->vertop;
    np  = (SVECTOR*)tmd->nortop;
    pp  = (u8*)tmd->primtop;
    primn = (int)tmd->primn;
    pk  = GsOUT_PACKET_P;

    if (!vp || !pp) { return; }
    if (lmode < 0 || lmode > 2) lmode = 0;
 
    while (primn > 0) {
        u8 olen = pp[0];
        u8 ilen = pp[1];
        u8 flag = pp[2];
        u8 mode = pp[3];
        int batch, lsc, iip, qd, tme;
        u8 *next;
        typedef PACKET* (*tmd_func_lit)(void*, VERT*, VERT*, PACKET*, int, int, GsOT*, u_long*);
        typedef PACKET* (*tmd_func_nl)(void*, VERT*, PACKET*, int, int, GsOT*, u_long*);
 
        if (ilen == 0) break;
 
        /* Count consecutive same-type primitives */
        batch = 1;
        next  = pp + ilen * 4;
        while (batch < primn) {
            if (next[1] != ilen || next[3] != mode || next[2] != flag) break;
            batch++;
            next += ilen * 4;
        }
 
        lsc = flag & 1;         /* light source calculation */
        iip = mode & 1;         /* gouraud */
        qd  = (mode >> 1) & 1;  /* quad */
        tme = (mode >> 2) & 1;  /* textured */
 
        if (lsc) {
            /* Lit primitives — dispatch through GsFCALL4 */
            tmd_func_lit fn = NULL;
            if (!tme) {
                if (!qd)
                    fn = (tmd_func_lit)(iip ? GsFCALL4.g3[divmode][lmode] : GsFCALL4.f3[divmode][lmode]);
                else
                    fn = (tmd_func_lit)(iip ? GsFCALL4.g4[divmode][lmode] : GsFCALL4.f4[divmode][lmode]);
            } else {
                if (!qd)
                    fn = (tmd_func_lit)(iip ? GsFCALL4.tg3[divmode][lmode] : GsFCALL4.tf3[divmode][lmode]);
                else
                    fn = (tmd_func_lit)(iip ? GsFCALL4.tg4[divmode][lmode] : GsFCALL4.tf4[divmode][lmode]);
            }
            if (fn) fn(pp, (VERT*)vp, (VERT*)np, pk, batch, shift, ot, scratch);
        } else {
            /* No-light primitives */
            tmd_func_nl fn = NULL;
            if (!tme) {
                if (!qd)
                    fn = (tmd_func_nl)(iip ? GsFCALL4.ng3[divmode] : GsFCALL4.nf3[divmode]);
                else
                    fn = (tmd_func_nl)(iip ? GsFCALL4.ng4[divmode] : GsFCALL4.nf4[divmode]);
            } else {
                if (!qd)
                    fn = (tmd_func_nl)(iip ? GsFCALL4.ntg3[divmode] : GsFCALL4.ntf3[divmode]);
                else
                    fn = (tmd_func_nl)(iip ? GsFCALL4.ntg4[divmode] : GsFCALL4.ntf4[divmode]);
            }
            if (fn) fn(pp, (VERT*)vp, pk, batch, shift, ot, scratch);
        }
 
        pk = GsOUT_PACKET_P;
        pp = next;
        primn -= batch;
    }
}

/* Bounds for the OT0 sanitizer (game_main.c) so it knows the OT1 storage
 * region is a valid chain target. Without this, the sanitizer would flag
 * OT1's bucket nodes as wild pointers and truncate the OT0 chain mid-walk —
 * killing pickup-screen item rendering even though GsSortOt did its job.
 * Set by GsSortOt below; queried via GsSortOt_GetSubrootBounds(). */
static uintptr_t s_GsSortOt_subrootLo = 0;
static uintptr_t s_GsSortOt_subrootHi = 0;

void GsSortOt_GetSubrootBounds(uintptr_t* lo, uintptr_t* hi)
{
    if (lo) *lo = s_GsSortOt_subrootLo;
    if (hi) *hi = s_GsSortOt_subrootHi;
}

void GsSortOt(GsOT *src, GsOT *dst)
{
#ifdef SH_PC_PORT
    /* PSX libgs GsSortOt(subroot, root): registers `subroot` as a sub-OT
     * of `root` so that DrawOTag, when walking root, will dive into the
     * subroot's chain at the splice point. The pickup-screen pipeline in
     * item_screens_cam.c func_8004BD74 (arg2==2 path) is:
     *
     *     GsClearOt(OT1);                   // empty out subroot
     *     GsSortOt(OT1, OT0);               // register subroot now
     *     GsSortObject4J(item, OT1, ...);   // populate subroot AFTER
     *
     * — so this function MUST splice on empty buckets and rely on
     * GsSortObject4J's later addPrim()s into OT1 propagating through the
     * link we set up here.
     *
     * Mechanics: src->tag is the head of src's chain (at &src->org[N-1]),
     * src->org[0] is the chain terminator end. We splice the whole src
     * bucket-chain into dst->org[0]:
     *     setaddr(src->org[0], dst->org[0].next)   # tail of src → dst's old next
     *     setaddr(dst->org[0],     src->tag)       # dst's next → head of src
     *
     * This was a no-op stub before — that's why pickup-screen items
     * (health drink, keys, etc.) rendered invisible: the TMD got sorted
     * into OT1 but OT1 was orphaned.
     *
     * Bounds export for the OT0 sanitizer (game_main.c Pc_OtSentinelScan
     * and the inline OT0 walker pre-GsDrawOt): src->org and src->tag
     * point into OT1's backing storage (FS_BUFFER_1 / PSX_ADDR space),
     * which is outside the packet-buffer + OT0-array windows the
     * sanitizer recognises. Export the OT1 range so the sanitizer can
     * accept those nodes instead of flagging them as wild and
     * truncating the chain. */
    GsOT_TAG* dst_slot0;
    uintptr_t dst_slot0_next;
    GsOT_TAG* src_tail;
    int       n_src;

    if (src == NULL || dst == NULL) return;
    if (src->org == NULL || dst->org == NULL) return;
    if (src->tag == NULL) return;

    n_src    = 1 << src->length;
    src_tail = &src->org[0];                     /* terminator end of src chain */
    dst_slot0      = (GsOT_TAG*)&dst->org[0];    /* nearest bucket of dst */
    dst_slot0_next = getaddr(dst_slot0);

    /* Splice src into dst at dst.org[0]. src->tag will be the head of
     * the inserted chain and src->org[0] (already a terminator before
     * GsSortObject4J runs) becomes the rejoin point back into dst. */
    setaddr(src_tail,  dst_slot0_next);
    setaddr(dst_slot0, src->tag);

    /* Publish src's storage bounds for the OT0 sanitizer. Cover the
     * full src->org[] array. */
    s_GsSortOt_subrootLo = (uintptr_t)src->org;
    s_GsSortOt_subrootHi = (uintptr_t)(src->org + n_src);

    {
        static int s_logCount = 0;
        if (s_logCount < 4) {
            SH_DBG("[GSSORTOT] spliced subroot src=%p tag=%p (n=%d) into dst[0]=%p (had next=%p) bounds=[%p..%p)",
                   (void*)src, (void*)src->tag, n_src,
                   (void*)dst_slot0, (void*)dst_slot0_next,
                   (void*)s_GsSortOt_subrootLo, (void*)s_GsSortOt_subrootHi);
            s_logCount++;
        }
    }
#else
    (void)src; (void)dst;
#endif
}
