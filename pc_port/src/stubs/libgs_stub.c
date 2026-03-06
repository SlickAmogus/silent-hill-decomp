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
#include <string.h>
#include <stdio.h>
#include <SDL.h>
#include <PsyX/common/glad.h>

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

void GsInit3D(void)
{
    InitGeom();
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

void GsSwapDispBuff(void)
{
    gs_active_buff = gs_active_buff ? 0 : 1;

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

void GsDrawOt(GsOT *ot)
{
    static int drawDbg = 0;
    if (ot && ot->tag)
    {
        if (drawDbg < 10) {
            /* Walk the OT and count primitives */
            int count = 0;
            GsOT_TAG* p = ot->tag;
            while (p && count < 200) {
                if (p->len > 0) count++;
                if (p->addr == 0 || p->addr == (uintptr_t)-1) break;
                p = (GsOT_TAG*)(p->addr);
            }
            printf("[SH] GsDrawOt: tag=%p len=%lu primCount=%d\n",
                (void*)ot->tag, ot->length, count);
            drawDbg++;
        }
        DrawOTag((u_long*)ot->tag);
    }
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

/* TMD rendering stubs - these need full implementation for 3D rendering */
void GsTMDfastG3LFG(void* op, VERT* vp, VERT* np, PACKET* pk, int n, int shift, GsOT* ot, unsigned long* scratch)
{
    /* TODO: Implement Gouraud-shaded triangle rendering */
    (void)op; (void)vp; (void)np; (void)pk; (void)n; (void)shift; (void)ot; (void)scratch;
}

void GsTMDfastTG3LFG(void* op, VERT* vp, VERT* np, PACKET* pk, int n, int shift, GsOT* ot, unsigned long* scratch)
{
    /* TODO: Implement textured Gouraud-shaded triangle rendering */
    (void)op; (void)vp; (void)np; (void)pk; (void)n; (void)shift; (void)ot; (void)scratch;
}

void GsTMDfastG4LFG(void* op, VERT* vp, VERT* np, PACKET* pk, int n, int shift, GsOT* ot, unsigned long* scratch)
{
    /* TODO: Implement Gouraud-shaded quad rendering */
    (void)op; (void)vp; (void)np; (void)pk; (void)n; (void)shift; (void)ot; (void)scratch;
}

void GsTMDfastTG4LFG(void* op, VERT* vp, VERT* np, PACKET* pk, int n, int shift, GsOT* ot, unsigned long* scratch)
{
    /* TODO: Implement textured Gouraud-shaded quad rendering */
    (void)op; (void)vp; (void)np; (void)pk; (void)n; (void)shift; (void)ot; (void)scratch;
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
    /* TODO: Implement priority setting */
    (void)p; (void)a; (void)b;
}

_GsFCALL GsFCALL4;

/* Global matrices */
MATRIX GsWSMATRIX;
MATRIX GsWSMATRIX_ORG;
MATRIX GsIDMATRIX;
MATRIX GsIDMATRIX2;

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

void GsInitCoordinate2(void *super, GsCOORDINATE2 *coord)
{
    if (coord) {
        memset(coord, 0, sizeof(GsCOORDINATE2));
        coord->flg = 0;
    }
}

void GsLinkObject4(GsDOBJ2 *obj, void *data)
{
    (void)obj; (void)data;
}

void GsMapModelingData(void *data)
{
    (void)data;
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
    (void)obj; (void)ot; (void)shift; (void)scratch;
}

void GsSortOt(GsOT *src, GsOT *dst)
{
    (void)src; (void)dst;
}
