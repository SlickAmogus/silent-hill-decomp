/*
 * psx_libgpu_xbox.c - PSX libgpu entry points the game calls that aren't part of
 * the NV2A control surface in gpu_xbox.c.
 *
 * Primitive + draw-env initializers are thin wrappers over the libgpu.h setXxx
 * macros — identical to what PsyCross's libgpu.c does (they only stamp each
 * packet's code/len, no rasterization). VRAM image transfers are stubbed until
 * PSX-VRAM emulation lands (texturing TODO); VSync is a frame counter for now.
 * gpu_xbox.c already provides DrawOTag, ResetGraph, the Put/SetDef env calls,
 * the ClearOTag family, etc.
 */
#include <libgte.h>   /* before libgpu.h: it uses SVECTOR */
#include <libgpu.h>
#include <libetc.h>
#include "sh_log.h"

/* --- Primitive initializers (stamp code+len) --------------------------------*/
void SetPolyF4(POLY_F4* p)   { setPolyF4(p); }
void SetPolyFT4(POLY_FT4* p) { setPolyFT4(p); }
void SetPolyG3(POLY_G3* p)   { setPolyG3(p); }
void SetPolyG4(POLY_G4* p)   { setPolyG4(p); }
void SetTile(TILE* p)        { setTile(p); }

void AddPrim(void* ot, void* p) { addPrim(ot, p); }
void TermPrim(void* p)          { termPrim(p); }

/* --- Draw-environment packets ----------------------------------------------*/
void SetDrawTPage(DR_TPAGE* p, int dfe, int dtd, int tpage)        { setDrawTPage(p, dfe, dtd, tpage); }
void SetDrawMode(DR_MODE* p, int dfe, int dtd, int tpage, RECT* tw){ setDrawMode(p, dfe, dtd, tpage, tw); }

/* DR_AREA/DR_OFFSET/DR_MOVE packet builders: PsyCross's libgpu.h has no
 * setDrawArea/Offset/Move macro, and the NV2A DrawOTag doesn't consume draw-env
 * packets yet (0xE0 dispatch is a TODO), so these are no-ops for now — clip /
 * offset / VRAM-move land with real draw-env support. */
void SetDrawArea(DR_AREA* p, RECT* r)                  { (void)p; (void)r; }
void SetDrawOffset(DR_OFFSET* p, u_short* ofs)         { (void)p; (void)ofs; }
void SetDrawMove(DR_MOVE* p, RECT* rect, int x, int y) { (void)p; (void)rect; (void)x; (void)y; }

/* SetDrawEnv/GetDrawEnv: the active env is driven through gpu_xbox.c's
 * PutDrawEnv; these OT-queued/query forms are no-ops/pass-throughs for now. */
void SetDrawEnv(DR_ENV* dr_env, DRAWENV* env) { (void)dr_env; (void)env; }
DRAWENV* GetDrawEnv(DRAWENV* env)             { return env; }

/* --- VRAM image transfers -> PSX VRAM emulation (psx_vram.c) ----------------*/
extern void PsxVram_Load(int x, int y, int w, int h, const unsigned short* src);
extern void PsxVram_Store(int x, int y, int w, int h, unsigned short* dst);

int LoadImage(RECT* rect, u_long* p)
{
    if (rect) PsxVram_Load(rect->x, rect->y, rect->w, rect->h, (const unsigned short*)p);
    return 0;
}
int StoreImage(RECT* rect, u_long* p)
{
    if (rect) PsxVram_Store(rect->x, rect->y, rect->w, rect->h, (unsigned short*)p);
    return 0;
}
int ClearImage(RECT* rect, u_char r, u_char g, u_char b)  { (void)rect; (void)r; (void)g; (void)b; return 0; }
int ClearImage2(RECT* rect, u_char r, u_char g, u_char b) { (void)rect; (void)r; (void)g; (void)b; return 0; }

/* --- libetc timing + frame present -----------------------------------------*/
/* Forward-declared (not via gpu_nv2a.h) to keep this TU free of the pbkit/
 * windows.h include tangle. */
extern void GpuNv2a_FrameBegin(void);
extern void GpuNv2a_FrameEnd(void);
extern void GpuNv2a_WaitVbl(void);
extern void Pad_Poll(void);       /* refresh the PSX pad buffer (pad_xbox.c) */
extern void Audio_XboxPump(void); /* refill the DirectSound ring (dsound_xbox.c) */

static volatile int s_vblanks = 0;
static void (*s_vsyncCb)(void) = 0;   /* the game's per-vblank callback */

/* PSX VSync():
 *   mode <0 (SyncMode_Count): return the vblank counter WITHOUT blocking. The game
 *     reads this several times per frame for timing — the old code presented on
 *     every call, so we were swapping the framebuffer ~12x/frame -> flicker.
 *   mode 0 (SyncMode_Wait): wait one vblank. mode N>0: wait N vblanks.
 *
 * For the wait modes we present the frame the game just rendered (FrameEnd swaps),
 * hold it for N vblanks, then open the next frame (FrameBegin). The registered
 * VSyncCallback (Screen_VSyncCallback: boot-logo timers counters_1C[] + MIDI pump)
 * is fired once per real vblank — we have no vblank IRQ, so this is its tick. */
int VSync(int mode)
{
    int n, i;

    if (mode < 0)
        return s_vblanks;

    Pad_Poll();
    Audio_XboxPump();               /* keep the DirectSound ring fed (once per frame) */
    GpuNv2a_FrameEnd();              /* present the rendered frame (swap at vblank) */

    n = (mode == 0) ? 1 : mode;
    for (i = 0; i < n; i++) {
        GpuNv2a_WaitVbl();          /* hold the presented frame for one vblank */
        ++s_vblanks;
        /* Commit the RAM-buffered log to HDD ~once/second (here, not on the mode<0
         * timing reads) so SH_DBG stays a cheap memcpy in the hot path. */
        if ((s_vblanks % 60) == 0) { extern void SH_DebugLogFlush(void); SH_DebugLogFlush(); }
        if (s_vsyncCb)
            s_vsyncCb();
    }

    GpuNv2a_FrameBegin();           /* clear + target the next frame's back buffer */
    if ((s_vblanks % 600) == 0)
        SH_DBG("[SH-XBOX] vblank %d", s_vblanks);
    return s_vblanks;
}

int VSyncCallback(void (*f)(void)) { s_vsyncCb = f; return 0; }
