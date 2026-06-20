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

/* --- VRAM image transfers (stubbed; real VRAM emulation = texturing TODO) ---*/
int LoadImage(RECT* rect, u_long* p)  { (void)rect; (void)p; return 0; }
int StoreImage(RECT* rect, u_long* p) { (void)rect; (void)p; return 0; }
int ClearImage(RECT* rect, u_char r, u_char g, u_char b)  { (void)rect; (void)r; (void)g; (void)b; return 0; }
int ClearImage2(RECT* rect, u_char r, u_char g, u_char b) { (void)rect; (void)r; (void)g; (void)b; return 0; }

/* --- libetc timing + frame present -----------------------------------------*/
/* Forward-declared (not via gpu_nv2a.h) to keep this TU free of the pbkit/
 * windows.h include tangle. */
extern void GpuNv2a_FrameBegin(void);
extern void GpuNv2a_FrameEnd(void);
extern void Pad_Poll(void);   /* refresh the PSX pad buffer (pad_xbox.c) */

static volatile int s_vsyncCount = 0;
static void (*s_vsyncCb)(void) = 0;   /* the game's per-vblank callback */

/* The game calls VSync(0) once per frame to wait for vblank; we use it as the
 * present point — finish + swap the frame the game just rendered, then open the
 * next. main_xbox.c opens the very first frame before entering MainLoop.
 *
 * On PSX the registered VSyncCallback fires on every vblank interrupt; the game
 * relies on it (Screen_VSyncCallback: increments counters_1C[] — the boot-logo
 * timers — and pumps the MIDI sequencer). We have no vblank IRQ, so fire it here
 * once per frame. Without this, all frame timers freeze and boot logos never
 * advance. */
int VSync(int mode)
{
    (void)mode;
    Pad_Poll();
    if (s_vsyncCb)
        s_vsyncCb();
    GpuNv2a_FrameEnd();
    GpuNv2a_FrameBegin();
    ++s_vsyncCount;
    if ((s_vsyncCount % 600) == 0)   /* ~10s heartbeat: confirms MainLoop loops */
        SH_DBG("[SH-XBOX] frame %d", s_vsyncCount);
    return s_vsyncCount;
}

int VSyncCallback(void (*f)(void)) { s_vsyncCb = f; return 0; }
