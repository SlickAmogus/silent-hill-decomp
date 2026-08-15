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
    s_poolUsed            = 0;
    s_trisThisFrame       = 0;
    s_batchVertsThisFrame = 0;
    s_texBindsThisFrame   = 0;
    s_blendChanges        = 0;
    s_scissorChanges      = 0;
}

void GpuNv2a_FrameEnd(void)
{
    /* One line per 60 frames: enough to see the renderer is being fed and the
     * counts are stable, without turning a USB-backed log into the bottleneck. */
    if ((s_frame % 60) == 0) {
        SH_DBG("[GPU] frame=%u tris=%u verts=%u texbind=%u blend=%u scissor=%u",
               s_frame, s_trisThisFrame, s_batchVertsThisFrame,
               s_texBindsThisFrame, s_blendChanges, s_scissorChanges);
    }
    s_frame++;
}

void GpuNv2a_WaitVbl(void)
{
    /* No swap chain yet, so there is no vblank to wait on. Returning immediately
     * means the game free-runs; frame pacing arrives with the real backend. */
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

int GpuXbox_FbRegionOverlap(int x0, int y0, int x1, int y1)
{
    (void)x0; (void)y0; (void)x1; (void)y1;
    return 0;                          /* nothing in the fb to overlap yet */
}

void GpuXbox_FbReadbackForTexture(int px0, int py0, int px1, int py1)
{
    (void)px0; (void)py0; (void)px1; (void)py1;
}

void GpuXbox_FbReadbackForStore(void) { }
void GpuXbox_FbStoreFrameTick(void)   { }
