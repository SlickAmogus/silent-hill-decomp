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

/* Draw-env packet builders, ported from PsyCross libgpu.c. These packets are
 * built in the shared per-frame packet arena and addPrim'd into the OT
 * (bodyprog_8005E0DC.c, water.c, several maps) — the old no-ops left the
 * P_TAG len/code as STALE BYTES from previous frames: the exact garbage-length
 * OT-corruption/crash class PsyCross documents fixing twice on PC. Every
 * builder must at minimum stamp a well-formed tag. */
void SetDrawArea(DR_AREA* p, RECT* r)
{
    p->code[0] = ((r->x & 0x3FF) | ((r->y & 0x3FF) << 10)) | 0xE3000000;
    p->code[1] = (((r->x + r->w) & 0x3FF) | (((r->y + r->h) & 0x3FF) << 10)) | 0xE4000000;
    setlen(p, 2);
}

/* Render-neutral (PsyCross parity): the PSX draw-offset positions rendering
 * into the double-buffer halves; our transform derives position from the
 * draw/display envs, so applying the raw offset would shift everything. Emit a
 * well-formed 2-long no-op so the OT walk consumes it correctly. */
void SetDrawOffset(DR_OFFSET* p, u_short* ofs)
{
    (void)ofs;
    p->code[0] = 0;
    p->code[1] = 0;
    setlen(p, 2);
}

void SetDrawMove(DR_MOVE* p, RECT* rect, int x, int y)
{
    int len = 5;
    if (rect->w == 0 || rect->h == 0)
        len = 0;
    p->code[0] = 0x01000000;
    p->code[1] = 0x80000000;
    p->code[2] = *(u_int*)&rect->x;
    p->code[3] = ((u_int)y << 16) | ((u_int)x & 0xFFFFu);
    p->code[4] = *(u_int*)&rect->w;
    setlen(p, len);
}

void SetDrawEnv(DR_ENV* dr_env, DRAWENV* env)
{
    dr_env->code[0] = (((env->clip.y & 0x3FF) << 10) | (env->clip.x & 0x3FF)) | 0xE3000000;
    dr_env->code[1] = ((((env->clip.y + env->clip.h - 1) & 0x3FF) << 10)
                       | ((env->clip.x + env->clip.w - 1) & 0x3FF)) | 0xE4000000;
    dr_env->code[2] = (((env->ofs[1] & 0x3FF) << 11) | (env->ofs[0] & 0x7FF)) | 0xE5000000;
    dr_env->code[3] = (32 * (((256 - env->tw.h) >> 3) & 0x1F)) | (((256 - env->tw.w) >> 3) & 0x1F)
                      | (((env->tw.y >> 3) & 0x1F) << 15) | (((env->tw.x >> 3) & 0x1F) << 10) | 0xE2000000;
    dr_env->code[4] = ((env->dtd != 0) << 9) | ((env->dfe != 0) << 10) | (env->tpage & 0x1FF) | 0xE1000000;
    setlen(dr_env, 5);
}

/* Must stamp the tag (len 2, code 0xE6): a no-op left an uninitialised tag —
 * garbage length — and the OT walker crashed on PC (map4_s03 cult-TV cutscene). */
void SetDrawStp(DR_STP* p, int pbw)
{
    setDrawStp(p, pbw);
}

DRAWENV* GetDrawEnv(DRAWENV* env)             { return env; }

/* --- VRAM image transfers -> PSX VRAM emulation (psx_vram.c) ----------------*/
extern void PsxVram_Load(int x, int y, int w, int h, const unsigned short* src);
extern void PsxVram_Store(int x, int y, int w, int h, unsigned short* dst);
extern void PsxVram_Move(int sx, int sy, int w, int h, int dx, int dy);
extern void PsxVram_Fill(int x, int y, int w, int h, unsigned short c);
extern void GpuXbox_ClearRectOnScreen(int x, int y, int w, int h, int r, int g, int b);
/* Framebuffer readback (gpu_xbox.c): StoreImage grabs of the framebuffer pages
 * must see rendered output, not just uploaded VRAM. */
extern int  GpuXbox_FbRegionOverlap(int x0, int y0, int x1, int y1);
extern void GpuXbox_FbReadbackForStore(void);
extern void GpuXbox_FbStoreFrameTick(void);

int LoadImage(RECT* rect, u_long* p)
{
    if (rect) PsxVram_Load(rect->x, rect->y, rect->w, rect->h, (const unsigned short*)p);
    return 0;
}
int StoreImage(RECT* rect, u_long* p)
{
    /* Probe: witness display-area grabs (pause/save/crossfade backgrounds). */
    static int s_storeLogged = 0;
    if (rect && s_storeLogged < 16) {
        s_storeLogged++;
        SH_DBG("[STORE] rect=(%d,%d %dx%d)", rect->x, rect->y, rect->w, rect->h);
    }
    /* Grabs of the framebuffer pages must return RENDERED output: pull the
     * just-composed back buffer into s_vram first (gpu_xbox.c; gated by the
     * game's g_PsxSkipFramebufferStore + once per frame). NOTE the grabs the
     * [STORE] probe caught on hardware — rect=(320,256 160x240) around the
     * paper-map pickup / FMV (game_sys_states.c, map0_s01_events.c) — are NOT
     * framebuffer grabs and correctly do not trigger this: x>=320 lies right
     * of every framebuffer page (progressive pages (0,32)/(0,256) 320x224,
     * interlaced (0,32) 320x448). That rect is the staging strip where the
     * 640x480 4-bit paper-map TIM lands (640px at 4bpp = 160 VRAM words wide,
     * halves at (320,16)/(320,256)): the game saves the room-texture strip the
     * TIM/FMV clobbers and restores it after — it wants UPLOADED data, which
     * s_vram round-trips byte-perfect. */
    if (rect && GpuXbox_FbRegionOverlap(rect->x, rect->y,
                                        rect->x + rect->w, rect->y + rect->h))
        GpuXbox_FbReadbackForStore();
    if (rect) PsxVram_Store(rect->x, rect->y, rect->w, rect->h, (unsigned short*)p);
    return 0;
}

/* ClearImage: fill the VRAM rect (keeps StoreImage consumers consistent) and,
 * when it overlaps the display area, draw the flat quad into the live frame so
 * colour wipes/flashes are visible (title transitions, interlaced clears). */
static int ClearImageImpl(RECT* rect, u_char r, u_char g, u_char b)
{
    unsigned short c;
    if (!rect) return 0;
    c = (unsigned short)(((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3));
    PsxVram_Fill(rect->x, rect->y, rect->w, rect->h, c);
    GpuXbox_ClearRectOnScreen(rect->x, rect->y, rect->w, rect->h, r, g, b);
    return 0;
}
int ClearImage(RECT* rect, u_char r, u_char g, u_char b)  { return ClearImageImpl(rect, r, g, b); }
int ClearImage2(RECT* rect, u_char r, u_char g, u_char b) { return ClearImageImpl(rect, r, g, b); }

int MoveImage(RECT* rect, int x, int y)
{
    if (rect) PsxVram_Move(rect->x, rect->y, rect->w, rect->h, x, y);
    return 0;
}

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

    /* One present per call, N vblanks held. Do NOT try to be clever about
     * skipping the present when "nothing was drawn": the FMV player, the boot
     * logos and the 2D screens all render through paths other than DrawOTag, so
     * gating on a draw flag left them never presented (white screen) and — since
     * Pad_Poll lived behind the same gate — killed input entirely.
     *
     * The multi-vblank case is handled here instead, via mode > 0: the 30fps
     * pacing loop asks for 2 vblanks, and answering that as one present + two
     * waits is exactly right. Calling this twice with mode 0 would present the
     * freshly-cleared back buffer on the second call (a black frame every other
     * frame), which is why game_main.c coalesces the wait into a single call. */
    Pad_Poll();
    {   /* time the audio ring pump (software SPU mix) — part of closing the
         * per-frame cost gap; logs only when it spikes above 5ms. */
        extern int GpuNv2a_Ms(void);
        int _ap = GpuNv2a_Ms();
        Audio_XboxPump();            /* keep the DirectSound ring fed */
        {
            int ms = GpuNv2a_Ms() - _ap;
            static int s_apLog = 0;
            if (ms > 5 && (s_apLog++ & 7) == 0)
                SH_DBG("[POST] audioPumpMs=%d", ms);
        }
    }
    GpuNv2a_FrameEnd();              /* present the rendered frame (swap at vblank) */
    GpuXbox_FbStoreFrameTick();      /* latch+reset the per-frame TIM-protect gate
                                      * (g_PsxSkipFramebufferStore) — PsyX_EndScene parity */

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
    if ((s_vblanks % 600) == 0) {
        extern void Xbox_MemReport(const char*);
        SH_DBG("[SH-XBOX] vblank %d", s_vblanks);
        Xbox_MemReport("tick");     /* leak watch: free RAM should stay flat */
    }
    return s_vblanks;
}

int VSyncCallback(void (*f)(void)) { s_vsyncCb = f; return 0; }
