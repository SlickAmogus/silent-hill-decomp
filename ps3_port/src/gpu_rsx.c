/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * gpu_rsx.c - the PS3 side of the PSX libgpu, replacing gpu_nv2a.c.
 *
 * PHASE 1: A COUNTING NO-OP. Nothing is drawn yet.
 *
 * That is deliberate, not a placeholder left by accident. gpu_xbox.c -- the half
 * of the renderer that walks the ordering table and decodes PSX primitives -- is
 * hardware-agnostic and compiles for the PPU unchanged, so the game can run its
 * entire boot, load its data, build its OTs and emit primitives with no GPU
 * behind them at all. Doing that first separates two very different failures:
 *
 *   - the game does not reach the render loop      (game/HAL/endian problem)
 *   - the game emits primitives but nothing appears (RSX backend problem)
 *
 * Keeping them apart matters more than it sounds. The 360 shipped a GTE fix,
 * saw tris=0, and spent the effort on the renderer before noticing the counter
 * was dead and the real fault was in a shared header: the "Fast" primitive
 * setters composed a wide store in little-endian field order, so x and y were
 * swapped in every 2D primitive and PolyOversized correctly rejected them. A
 * phase that reports "OT walked, N primitives emitted, bounding box B" answers
 * that question before any GPU code exists to be blamed.
 *
 * When the real backend lands it goes here: the RSX is an NV47/G70 part, so it
 * is a much closer relative of the Xbox's NV2A than of the 360's Xenos -- both
 * are NVIDIA FIFO-pushbuffer designs programmed by writing class methods into a
 * command buffer. gpu_nv2a.c is therefore the template to follow, with librsx
 * calls in place of pbkit and cgcomp-built vertex/fragment programs in place of
 * the NV2A shaders.
 */
#include <stdio.h>
#include <string.h>
#include <malloc.h>

#include "gpu_nv2a.h"
#include "sh_log.h"
#include "sh_hwperf.h"
#include "ps3_hal.h"

/* Per-frame counters, read and reset by GpuNv2a_FrameEnd. */
static unsigned s_frame;
static unsigned s_trisThisFrame;
static unsigned s_batchVertsThisFrame;
static unsigned s_texBindsThisFrame;
static unsigned s_blendChanges;
static unsigned s_scissorChanges;

/* Screen-space bounding box of everything staged this frame. This is the probe
 * that would have caught the 360's swapped-axis bug immediately: on a 640x480
 * surface a healthy box stays inside it, and field-swapped primitives blow it
 * out to the coordinate range of the OTHER axis. */
static int s_bbX0, s_bbY0, s_bbX1, s_bbY1;
static int s_bbValid;

/* The batch handed out last time, not yet measured: BatchAlloc's caller fills
 * the vertices AFTER we return, so a batch can only be read on the following
 * call (or at frame end). */
static int  s_pendStart = -1;
static int  s_pendCount;
static void FlushPendingBBox(void);

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
    s_frame = 0;
    if (Ps3Rsx_Init()) {
        /* The framebuffer is whatever mode the console is in, so the content
         * rect follows it instead of the 640x480 the Xbox ports assume.
         * gpu_xbox.c derives its whole screen transform from these. */
        g_Nv2aContentX = 0;
        g_Nv2aContentW = Ps3Rsx_Width();
        g_Nv2aContentH = Ps3Rsx_Height();
        SH_DBG("[GPU] rsx up: %dx%d, %d-vertex pool, %d B/vertex",
               g_Nv2aContentW, g_Nv2aContentH, GPU_POOL_VERTS, (int)sizeof(ShVertex));
    } else {
        SH_DBG("[GPU] rsx INIT FAILED - counting only, nothing will be presented");
    }
}

void GpuNv2a_FrameBegin(void)
{
    /* Doubles as psx_vram.c's texture-cache LRU clock, so it has to tick even
     * with no rendering behind it or every cached page looks equally stale. */
    g_Nv2aFrameCount++;
    s_poolUsed            = 0;
    s_trisThisFrame       = 0;
    s_batchVertsThisFrame = 0;
    s_texBindsThisFrame   = 0;
    s_blendChanges        = 0;
    s_scissorChanges      = 0;
    s_bbValid             = 0;
    s_pendStart           = -1;

    /* gpu_xbox.c owns the PSX draw env, so the clear colour is the game's own
     * background (the fog colour in-game) rather than anything chosen here.
     * Opaque alpha: the surface is X8R8G8B8 and a zero alpha reads as a black
     * frame on some display paths. */
    if (Ps3Rsx_Ready())
        Ps3Rsx_FrameBegin(0xFF000000u | (GpuXbox_GetClearColor() & 0x00FFFFFFu));
}

void GpuNv2a_FrameEnd(void)
{
    FlushPendingBBox();   /* the frame's last batch is only readable now */

    /* One line per 60 frames: enough to see the renderer is being fed and the
     * counts are stable, without drowning the log. The bounding box rides along
     * because "primitives emitted" alone cannot distinguish healthy geometry
     * from field-swapped garbage. */
    if ((s_frame % 60) == 0) {
        if (s_bbValid) {
            SH_DBG("[GPU] frame=%u tris=%u verts=%u texbind=%u blend=%u scissor=%u bbox=%d,%d..%d,%d",
                   s_frame, s_trisThisFrame, s_batchVertsThisFrame,
                   s_texBindsThisFrame, s_blendChanges, s_scissorChanges,
                   s_bbX0, s_bbY0, s_bbX1, s_bbY1);
        } else {
            SH_DBG("[GPU] frame=%u tris=%u verts=%u texbind=%u blend=%u scissor=%u bbox=none",
                   s_frame, s_trisThisFrame, s_batchVertsThisFrame,
                   s_texBindsThisFrame, s_blendChanges, s_scissorChanges);
        }
    }
    s_frame++;

    if (Ps3Rsx_Ready())
        Ps3Rsx_FrameEnd();
}

void GpuNv2a_WaitVbl(void)
{
    /* There is no flip to wait on yet, and returning immediately lets the loop
     * free-run -- the 360 measured TEN MILLION frames in one session that way,
     * which makes every timing-dependent behaviour in the log meaningless and
     * buries the useful lines. Pace to 60 Hz off the time base until the real
     * backend can block on a flip.
     *
     * Unlike the 360 this SLEEPS rather than spinning: GameOS schedules our
     * audio and pad service threads, and a busy-wait here would starve them. */
    static unsigned long long s_next;
    static unsigned long long s_period;
    unsigned long long        now;

    /* With a real display up, the flip IS the vblank: gcmSetFlipMode(VSYNC)
     * retires it on the scanout, so waiting on it paces the loop exactly and
     * costs no PPU. The time-base fallback below only runs when the RSX failed
     * to initialise, where free-running would otherwise reach millions of
     * frames and make every timing line in the log meaningless. */
    if (Ps3Rsx_Ready()) {
        Ps3Rsx_WaitFlip();
        return;
    }

    now = SH_CYCLES();
    if (!s_period)
        s_period = Ps3_TimebaseFreq() / 60;

    /* Resync rather than sprint to catch up: a blocking CD load stalls the loop
     * for many frames, and replaying them at full speed is worse than dropping. */
    if (s_next == 0 || now > s_next + s_period * 8)
        s_next = now;

    s_next += s_period;
    while ((now = SH_CYCLES()) < s_next) {
        /* s_period, not a fresh Ps3_TimebaseFreq() call: the frequency is a
         * SYSCALL, and asking for it once per spin iteration showed up in
         * RPCS3's syscall census as 4098 sys_time_get_timebase_frequency in a
         * one-second boot -- one per usleep. It cannot change at runtime. */
        unsigned long long left = s_next - now;
        unsigned ms = (unsigned)((left * 1000ULL) / (s_period * 60ULL));
        /* Sleep only the whole milliseconds; yield out the remainder so the
         * wake-up granularity cannot push us past the deadline every frame. */
        if (ms > 1)
            Ps3_SleepMs(ms - 1);
        else
            Ps3_SleepMs(0);           /* yield */
    }
}

void GpuNv2a_EmitTris(const ShVertex* verts, int count)
{
    (void)verts;
    if (count > 0)
        s_trisThisFrame += (unsigned)count / 3u;
}

static void TrackBBox(const ShVertex* v, int count)
{
    int i;
    for (i = 0; i < count; i++) {
        int x = (int)v[i].pos[0];
        int y = (int)v[i].pos[1];
        if (!s_bbValid) {
            s_bbX0 = s_bbX1 = x;
            s_bbY0 = s_bbY1 = y;
            s_bbValid = 1;
        } else {
            if (x < s_bbX0) s_bbX0 = x;
            if (x > s_bbX1) s_bbX1 = x;
            if (y < s_bbY0) s_bbY0 = y;
            if (y > s_bbY1) s_bbY1 = y;
        }
    }
}

static void FlushPendingBBox(void)
{
    if (s_pendStart >= 0) {
        TrackBBox(&s_pool[s_pendStart], s_pendCount);
        s_pendStart = -1;
    }
}

ShVertex* GpuNv2a_BatchAlloc(int count)
{
    ShVertex* p;

    if (count <= 0 || count > GPU_POOL_VERTS)
        return NULL;

    FlushPendingBBox();

    if (s_poolUsed + count > GPU_POOL_VERTS)
        s_poolUsed = 0;               /* wrap; contents are discarded anyway */

    p = &s_pool[s_poolUsed];
    s_pendStart = s_poolUsed;
    s_pendCount = count;
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
 * real backend starts DMAing out of them; the RSX wants its sources aligned. */
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
 * this build unchanged. Defining them here too is what the 360's first full link
 * caught. */

/* Nothing is queued, so the drain is already complete. */
void GpuNv2a_DrainGpu(void) { }

void GpuNv2a_SetDepthWrite(int enable)         { (void)enable; }
void GpuNv2a_SetPaletteDmaVariant(int variant) { (void)variant; }

/* Freeze-frame capture (pause/save backgrounds). 0 = "no frame captured", so
 * callers fall back to drawing the scene live instead of blitting a buffer that
 * was never filled -- which would show as a black pane. */
int  GpuNv2a_FreezeCapture(void) { return 0; }
void GpuNv2a_FreezeBlit(void)    { }
void GpuNv2a_FreezeRelease(void) { }
