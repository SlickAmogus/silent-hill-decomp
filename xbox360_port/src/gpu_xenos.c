/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * gpu_xenos.c - the 360 side of the PSX libgpu, replacing gpu_nv2a.c.
 *
 * PHASE 1: A COUNTING NO-OP. Nothing is drawn yet.
 *
 * That is deliberate, not a placeholder left by accident. gpu_xbox.c -- the half
 * of the renderer that walks the ordering table and decodes PSX primitives -- is
 * hardware-agnostic and compiles for PPC unchanged, so the game can run its
 * entire boot, load its data, build its OTs and emit primitives with no GPU
 * behind them at all. Doing that first separates two very different failures:
 *
 *   - the game does not reach the render loop      (game/HAL/endian problem)
 *   - the game emits primitives but nothing appears (Xenos backend problem)
 *
 * Each hardware test costs a BadUpdate run, so being able to read "OT walked,
 * 4700 prims emitted" out of a log is worth more than a half-working renderer
 * that cannot say whether the geometry reaching it was correct.
 *
 * The per-frame counters are the payload of this phase. Once they look sane, the
 * Xenos work (xenos_init is already done in main; shaders, EDRAM resolve, texture
 * upload) replaces the bodies here one at a time.
 */
#include <stdio.h>
#include <string.h>
#include <malloc.h>

#include <xenos/xenos.h>
#include <console/console.h>

#include "gpu_nv2a.h"
#include "sh_log.h"
#include "sh_hwperf.h"

/* Per-frame counters, read and reset by GpuNv2a_FrameEnd. */
static unsigned s_frame;
static unsigned s_trisThisFrame;
static unsigned s_batchVertsThisFrame;
static unsigned s_batchCalls;
static unsigned s_texBindsThisFrame;
static unsigned s_blendChanges;
static unsigned s_scissorChanges;

/* Vertex staging. gpu_xbox.c writes primitives in place through BatchAlloc, so
 * this has to be real writable memory of the right stride even though nothing
 * consumes it yet. It wraps rather than growing: the contents are discarded, and
 * a wrap must never look like an allocation failure to the caller. */
#define GPU_POOL_VERTS 32768
static ShVertex s_pool[GPU_POOL_VERTS];
static int      s_poolUsed;

/* State gpu_xbox.c reads directly. The content rect is the drawable area inside
 * the framebuffer (the Xbox port uses it for widescreen pillarboxing); full
 * surface until there is a real surface to inset. */
int                g_Nv2aFrameCount;
int                g_Nv2aContentX = 0;
int                g_Nv2aContentW = 640;
int                g_Nv2aContentH = 480;
unsigned long long g_Nv2aDrawCycles;

void GpuNv2a_Init(void)
{
    /* xenos_init() already ran in main_xbox360.c to bring the console up before
     * anything could log. Re-initialising here would tear down the display the
     * boot log is being written to. */
    s_frame = 0;
    SH_DBG("[GPU] xenos backend: counting no-op, %d-vertex pool, %d B/vertex",
           GPU_POOL_VERTS, (int)sizeof(ShVertex));
}

void GpuNv2a_FrameBegin(void)
{
    /* Doubles as psx_vram.c's texture-cache LRU clock, so it has to tick even
     * with no rendering behind it or every cached page looks equally stale. */
    g_Nv2aFrameCount++;
    s_poolUsed            = 0;
    s_trisThisFrame       = 0;
    s_batchVertsThisFrame = 0;
    s_batchCalls          = 0;
    s_texBindsThisFrame   = 0;
    s_blendChanges        = 0;
    s_scissorChanges      = 0;
}

void GpuNv2a_FrameEnd(void)
{
    /* One line per 60 frames: enough to see the renderer is being fed and the
     * counts are stable, without turning a USB-backed log into the bottleneck. */
    if ((s_frame % 60) == 0) {
        /* verts/prims come from BatchAlloc, which is the path gpu_xbox.c
         * actually uses. An earlier version reported triangles counted in
         * EmitTris -- which NOTHING calls -- so it read a constant zero and
         * looked like a rendering failure when it was a dead counter. */
        SH_DBG("[GPU] frame=%u prims=%u verts=%u (tris~%u) texbind=%u blend=%u scissor=%u",
               s_frame, s_batchCalls, s_batchVertsThisFrame,
               s_batchVertsThisFrame / 3,
               s_texBindsThisFrame, s_blendChanges, s_scissorChanges);
    }
    s_frame++;
}

void GpuNv2a_WaitVbl(void)
{
    /* There is no swap chain yet, so there is no real vblank to wait on -- but
     * returning immediately let the loop free-run to TEN MILLION frames in one
     * session. That makes every timing-dependent behaviour in the log
     * meaningless and buries the few useful lines under per-frame spam, so pace
     * to 60 Hz off the time base until the real backend can block on a flip. */
    static unsigned long long s_next;
    const unsigned long long  period = PPC_TIMEBASE_FREQ / 60;
    unsigned long long        now    = mftb();

    /* Resync rather than sprint to catch up: a blocking CD load stalls the loop
     * for many frames, and replaying them at full speed is worse than dropping. */
    if (s_next == 0 || now > s_next + period * 8)
        s_next = now;

    s_next += period;
    while (mftb() < s_next) { }
}

void GpuNv2a_EmitTris(const ShVertex* verts, int count)
{
    (void)verts;
    if (count > 0)
        s_trisThisFrame += (unsigned)count / 3u;
}

ShVertex* GpuNv2a_BatchAlloc(int count)
{
    ShVertex* p;

    if (count <= 0 || count > GPU_POOL_VERTS)
        return NULL;
    if (s_poolUsed + count > GPU_POOL_VERTS)
        s_poolUsed = 0;               /* wrap; contents are discarded anyway */

    p = &s_pool[s_poolUsed];
    s_poolUsed += count;
    s_batchVertsThisFrame += (unsigned)count;
    s_batchCalls++;
    return p;
}

void GpuNv2a_BindPaletted(const void* page, const void* palette)
{
    (void)page; (void)palette;
    s_texBindsThisFrame++;
}

void GpuNv2a_BindTexture(const void* addr, int w, int h)
{
    (void)addr; (void)w; (void)h;
    s_texBindsThisFrame++;
}

void GpuNv2a_BindWhite(void) { }

/* psx_vram.c really uses this memory, so it must be a genuine allocation even in
 * the no-op phase. 128-byte aligned so the same pointers stay usable when the
 * real backend starts DMAing out of them. */
void* GpuNv2a_AllocTexMem(int bytes)
{
    void* p;
    if (bytes <= 0)
        return NULL;
    p = memalign(128, (size_t)bytes);
    if (!p)
        SH_DBG("[GPU] AllocTexMem(%d) FAILED", bytes);
    return p;
}

void GpuNv2a_SetBlendMode(int mode) { (void)mode; s_blendChanges++; }

void GpuNv2a_SetScissor(int x, int y, int w, int h)
{
    (void)x; (void)y; (void)w; (void)h;
    s_scissorChanges++;
}

/* Readback has no surface to read yet. Callers treat NULL as "grab unavailable"
 * and skip the effect, which is why this reports the failure honestly rather
 * than handing back a stale or zeroed buffer that would render as a black pane. */
const void* GpuNv2a_ReadbackSurface(int fromLastQueued, int* w, int* h, int* pitchBytes)
{
    (void)fromLastQueued;
    if (w) *w = 0;
    if (h) *h = 0;
    if (pitchBytes) *pitchBytes = 0;
    return NULL;
}

int GpuNv2a_Ms(void) { return 0; }

/* The GpuXbox_Fb* entry points are NOT defined here: gpu_xbox.c owns them (it
 * holds the draw/display envs and the screen transform), and it is compiled into
 * this build unchanged. Defining them here too is what the first full link
 * caught. */

/* Nothing is queued, so the drain is already complete. */
void GpuNv2a_DrainGpu(void) { }

void GpuNv2a_SetDepthWrite(int enable)        { (void)enable; }
void GpuNv2a_SetPaletteDmaVariant(int variant) { (void)variant; }

/* Freeze-frame capture (pause/save backgrounds). 0 = "no frame captured", so
 * callers fall back to drawing the scene live instead of blitting a buffer that
 * was never filled -- which would show as a black pane. */
int  GpuNv2a_FreezeCapture(void) { return 0; }
void GpuNv2a_FreezeBlit(void)    { }
void GpuNv2a_FreezeRelease(void) { }
