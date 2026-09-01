/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Demand-driven texture-pack compose for pool slots. See texpack_lazy.h for
 * why this exists and what the pop-in trade rests on. */

#include <stdlib.h>
#include <SDL.h>
#include <string.h>

#include <SDL_timer.h>

#include "texpack_lazy.h"
#include "hires_override.h"
#include "tex_pack.h"
#include "pc_config.h"
#include "sh_log.h"

/* Retained sources are tiny next to what they replace: the measured pool mix is
 * ~15 KB/slot (4bpp 256x256 = 32 KB pixels + 512 B palette worst case), so a
 * heavy interior holds single-digit MB, and 512 slots at the largest shape the
 * pool can hold (8bpp 256x256) is a structural 36 MB. The cap is a backstop
 * against a shape nobody has seen, not a memory diet - over it a slot keeps its
 * native disc art, the same safe fallback as an uncomposed row. */
#define LAZY_SRC_CAP_BYTES ((long long)64 * 1024 * 1024)

/* Power of two: indices wrap by mask and fullness is (head - tail). Sized above
 * the 8192 distinct (slot,row) pairs because a slot dropped while its keys are
 * still queued leaves them behind as stale (discarded at pop) while the redraw
 * enqueues fresh ones. */
#define LAZY_RING 16384

/* Post-map-load burst window in PUMPED FRAMES, and the multiplier applied to
 * texpack_lazy_ms inside it. Armed by TexPackLazy_MapReset, i.e. by map init -
 * see the header for why an explicit signal is required here. 180 frames = 3 s
 * at 60 fps, which covers the fade-in a cold map spends its first rows in. */
#define LAZY_BURST_FRAMES 180
#define LAZY_BURST_SCALE  3

/* LRU eviction (see HiresOverride_EvictColdestPackRow). A row sampled within
 * this many pumped frames is never a candidate, so an actively drawn working
 * set is never recycled; when the HOT set alone exceeds the budget nothing is
 * evictable and the pump falls back to keeping native art, which is the old
 * behaviour and the correct degradation - far better than evict/recompose
 * thrash. The per-pump cap bounds the glDeleteTextures + native re-upload work
 * a single frame can take on. */
#define LAZY_EVICT_MIN_AGE      8
#define LAZY_EVICT_MAX_PER_PUMP 8

typedef struct {
    unsigned char*  pixels;  /* w16*h halfwords - copy of the TIM pixel block */
    unsigned short* clut;    /* clutW*rows halfwords - copy of the palette block */
    int             w16, h;
    int             clutW, rows;
    int             bpp;
    int             nativeW, nativeH;
    /* EXACT byte count this slot added to g_srcBytes. Stored rather than
     * recomputed on release: a drop-side formula that disagrees with the
     * add-side one drives the running total the wrong way, and on an unsigned
     * total the first over-subtract wraps to ~2^64 and disables retention for
     * the rest of the session. */
    long long       bytes;
    /* Bumped every time this slot is dropped or its source replaced, so a
     * worker job that raced a re-register can be told from a current one. */
    unsigned        gen;
    unsigned int    queued;  /* rows sitting in the ring, not yet serviced */
    unsigned int    done;    /* rows resolved: composed, or confirmed no match */
    char            name[16];
} LazySlot;

static LazySlot  g_slots[HIRES_POOL_SLOT_MAX];
static long long g_srcBytes = 0; /* signed: an accounting bug must go negative, not wrap */

static unsigned int g_ring[LAZY_RING];
static unsigned     g_ringHead = 0;
static unsigned     g_ringTail = 0;

static int g_burstFrames = 0;
static int g_pumpLogs    = 0;

static int ring_push(unsigned int key)
{
    if (g_ringHead - g_ringTail >= LAZY_RING) return 0;
    g_ring[g_ringHead++ & (LAZY_RING - 1)] = key;
    return 1;
}

/* ---- compose worker -------------------------------------------------------
 * A built row costs a file read, a PNG (or BC7) decode and an upscale blit --
 * single-digit ms for most rows, but a heavy pack source runs to tens or
 * hundreds, and on the game thread that is a visible hitch however tight the
 * budget, because the budget can only stop AFTER a row. So the CPU half runs
 * on one worker thread against private copies of the inputs, and the pump
 * collects finished canvases and pays only the GL upload (plus cache insert)
 * out of its wall-clock budget. The compose cache, the entry table scan and
 * every GL call stay on the game thread. texpack_worker = 0 restores the
 * synchronous path for A/B. */
#define LAZY_JOBS 8

typedef struct {
    int                busy;     /* slot claimed by the game thread */
    int                started;  /* worker picked it up */
    int                finished; /* result ready for the collector */
    int                slotId, row;
    unsigned           gen;
    unsigned char*     pixels;   /* private copies: a job outlives any slot */
    unsigned short*    clutRow;
    int                w16, h, clutW, bpp, palMax;
    unsigned long long srcHash, palHash;
    int                buildMs;
    int                matched;
    TpBuildResult      res;
} LazyJob;

static LazyJob     g_jobs[LAZY_JOBS];
static SDL_mutex*  g_jobMx;
static SDL_cond*   g_jobCv;
static SDL_Thread* g_jobThread;

static int LazyWorker(void* unused)
{
    (void)unused;
    SDL_LockMutex(g_jobMx);
    for (;;)
    {
        int i, found = -1;
        for (i = 0; i < LAZY_JOBS; i++)
        {
            if (g_jobs[i].busy && !g_jobs[i].started) { found = i; break; }
        }
        if (found < 0)
        {
            SDL_CondWait(g_jobCv, g_jobMx);
            continue;
        }
        g_jobs[found].started = 1;
        SDL_UnlockMutex(g_jobMx);
        {
            LazyJob* j  = &g_jobs[found];
            Uint64   t0 = SDL_GetPerformanceCounter();
            j->matched = TexPack_BuildCanvasThreaded(j->pixels, j->w16, j->h,
                                                     j->clutRow, j->clutW, j->bpp,
                                                     j->srcHash, j->palHash,
                                                     j->palMax, &j->res);
            j->buildMs = (int)((SDL_GetPerformanceCounter() - t0) * 1000 /
                               SDL_GetPerformanceFrequency());
        }
        SDL_LockMutex(g_jobMx);
        g_jobs[found].finished = 1;
    }
}

static int Lazy_WorkerReady(void)
{
    static int failed = 0;
    if (g_jobThread != NULL) return 1;
    if (failed) return 0;
    g_jobMx = SDL_CreateMutex();
    g_jobCv = SDL_CreateCond();
    if (g_jobMx != NULL && g_jobCv != NULL)
        g_jobThread = SDL_CreateThread(LazyWorker, "texpack_lazy", NULL);
    if (g_jobThread == NULL)
    {
        failed = 1;
        SH_DBG("[TEXPACK/LAZY] worker unavailable (%s) - composing on the game thread",
               SDL_GetError());
        return 0;
    }
    return 1;
}

/* One (slot,row) through the worker path. 1 = handled (job submitted, cache
 * hit registered, or resolved trivially); 0 = every job slot is in flight,
 * put the key back and stop submitting this pump. */
static int Lazy_SubmitRow(int slotId, int row, int* serviced)
{
    LazySlot*            s = &g_slots[slotId];
    unsigned long long   srcHash, palHash;
    int                  palMax;
    const unsigned char* hit = NULL;
    int                  hw = 0, hh = 0;
    int                  i;

    TexPack_EnsureScanned();
    if (!TexPack_HasEntries())
    {
        s->queued &= ~(1u << row);
        s->done   |= 1u << row;
        return 1;
    }

    TexPack_ComposeKeys(s->pixels, s->w16, s->h,
                        s->clut + (size_t)row * (size_t)s->clutW, s->clutW,
                        s->bpp, &srcHash, &palHash, &palMax);

    if (TexPack_CacheProbe(srcHash, palHash, s->bpp, &hit, &hw, &hh))
    {
        s->queued &= ~(1u << row);
        s->done   |= 1u << row;
        HiresOverride_PoolSlotRegisterRGBAKeyed(slotId, row, hit, hw, hh,
                                                s->nativeW, s->nativeH,
                                                TexPack_FoldKey(srcHash, palHash, s->bpp));
        (*serviced)++;
        return 1;
    }

    SDL_LockMutex(g_jobMx);
    for (i = 0; i < LAZY_JOBS; i++)
        if (!g_jobs[i].busy) break;
    if (i == LAZY_JOBS)
    {
        SDL_UnlockMutex(g_jobMx);
        return 0;
    }
    {
        LazyJob* j = &g_jobs[i];
        memset(j, 0, sizeof(*j));
        j->pixels  = (unsigned char*)malloc((size_t)s->w16 * (size_t)s->h * 2);
        j->clutRow = (unsigned short*)malloc((size_t)s->clutW * 2);
        if (j->pixels == NULL || j->clutRow == NULL)
        {
            free(j->pixels);
            free(j->clutRow);
            memset(j, 0, sizeof(*j));
            SDL_UnlockMutex(g_jobMx);
            /* Out of memory: keep native art rather than looping. */
            s->queued &= ~(1u << row);
            s->done   |= 1u << row;
            return 1;
        }
        memcpy(j->pixels, s->pixels, (size_t)s->w16 * (size_t)s->h * 2);
        memcpy(j->clutRow, s->clut + (size_t)row * (size_t)s->clutW,
               (size_t)s->clutW * 2);
        j->slotId  = slotId;
        j->row     = row;
        j->gen     = s->gen;
        j->w16     = s->w16;
        j->h       = s->h;
        j->clutW   = s->clutW;
        j->bpp     = s->bpp;
        j->palMax  = palMax;
        j->srcHash = srcHash;
        j->palHash = palHash;
        j->busy    = 1;
    }
    SDL_CondSignal(g_jobCv);
    SDL_UnlockMutex(g_jobMx);
    /* queued stays set: that is what keeps NoteWanted from re-enqueueing while
     * the job is in flight. The collector clears it. */
    return 1;
}

/* Land finished jobs: GL upload + cache insert, on the game thread, inside the
 * pump's wall-clock budget. */
static void Lazy_Collect(Uint64 start, long long budgetTicks, int* serviced, int* built)
{
    int i;

    if (g_jobThread == NULL) return;
    for (i = 0; i < LAZY_JOBS; i++)
    {
        LazyJob   j;
        LazySlot* s;
        int       take;

        if (*serviced > 0 &&
            (long long)(SDL_GetPerformanceCounter() - start) >= budgetTicks)
            break;

        SDL_LockMutex(g_jobMx);
        take = g_jobs[i].busy && g_jobs[i].finished;
        if (take)
        {
            j = g_jobs[i];
            memset(&g_jobs[i], 0, sizeof(g_jobs[i]));
        }
        SDL_UnlockMutex(g_jobMx);
        if (!take) continue;

        free(j.pixels);
        free(j.clutRow);

        s = &g_slots[j.slotId];
        if (s->gen != j.gen || j.row >= s->rows || !(s->queued & (1u << j.row)))
        {
            /* The slot moved on while the job ran; the result belongs to a
             * source that is no longer there. */
            free(j.res.rgba);
            free(j.res.ddsBytes);
            continue;
        }

        s->queued &= ~(1u << j.row);
        s->done   |= 1u << j.row;
        (*serviced)++;
        if (j.matched && j.res.built) (*built)++;

        if (j.buildMs > 30)
        {
            static int s_slowBuild = 0;
            if (s_slowBuild < 32)
            {
                s_slowBuild++;
                SH_DBG("[TEXPACK/LAZY] heavy source: '%s' row %d built in %d ms (off-thread)",
                       s->name, j.row, j.buildMs);
            }
        }

        if (j.matched && j.res.rgba != NULL)
        {
            Uint64 u0 = SDL_GetPerformanceCounter();
            HiresOverride_PoolSlotRegisterRGBAKeyed(j.slotId, j.row, j.res.rgba,
                                                    j.res.w, j.res.h,
                                                    s->nativeW, s->nativeH,
                                                    TexPack_FoldKey(j.srcHash, j.palHash, j.bpp));
            {
                int upMs = (int)((SDL_GetPerformanceCounter() - u0) * 1000 /
                                 SDL_GetPerformanceFrequency());
                static int s_slowUp = 0;
                if (upMs > 8 && s_slowUp < 32)
                {
                    s_slowUp++;
                    SH_DBG("[TEXPACK/LAZY] slow upload: '%s' row %d %dx%d took %d ms",
                           s->name, j.row, j.res.w, j.res.h, upMs);
                }
            }
            /* The cache takes ownership of the canvas, or frees it; the GL
             * upload above is already done with the bytes. */
            TexPack_CacheInsertOwned(j.srcHash, j.palHash, j.bpp,
                                     j.res.rgba, j.res.w, j.res.h);
        }
        else if (j.matched && j.res.ddsBytes != NULL)
        {
            HiresOverride_PoolSlotRegisterDdsKeyed(j.slotId, j.row,
                                                   j.res.ddsBytes, j.res.ddsSize,
                                                   s->nativeW, s->nativeH,
                                                   TexPack_FoldKey(j.srcHash, j.palHash, j.bpp));
            free(j.res.ddsBytes);
        }
        else
        {
            static int s_missLog2 = 0;
            if (s_missLog2 < 64)
            {
                s_missLog2++;
                SH_DBG("[TEXPACK] %s (pool slot %d): NO replacement for CLUT row %d - keeps native art",
                       s->name, j.slotId, j.row);
            }
        }
    }
}

void TexPackLazy_DropSlot(int slotId)
{
    LazySlot* s;

    if (slotId < 0 || slotId >= HIRES_POOL_SLOT_MAX) return;
    s = &g_slots[slotId];
    if (s->pixels == NULL && s->clut == NULL && s->bytes == 0) return;

    free(s->pixels);
    free(s->clut);
    g_srcBytes -= s->bytes;
    if (g_srcBytes < 0)
    {
        SH_DBG("[TEXPACK/LAZY] retain accounting went negative on slot %d (%lld) - clamped",
               slotId, g_srcBytes);
        g_srcBytes = 0;
    }
    /* Zeroing queued/done here is what makes a ring key that outlives its slot
     * safe: the pump discards any key whose row bit is no longer queued. The
     * generation survives the wipe and steps, so a worker-thread job submitted
     * against the old source can never land in the slot's next occupant. */
    {
        unsigned g = s->gen;
        memset(s, 0, sizeof(*s));
        s->gen = g + 1u;
    }
}

void TexPackLazy_MapReset(void)
{
    int i;
    for (i = 0; i < HIRES_POOL_CHARA_SLOT_BASE; i++)
    {
        TexPackLazy_DropSlot(i);
    }
    g_burstFrames = LAZY_BURST_FRAMES;
}

void TexPackLazy_RegisterSlotSource(int slotId, const char* timName,
                                    const unsigned char* pixels, int w16, int h,
                                    const unsigned short* clut, int clutW, int clutRows,
                                    int bpp, int nativeW, int nativeH)
{
    LazySlot* s;
    size_t    pxBytes, clBytes;

    if (slotId < 0 || slotId >= HIRES_POOL_SLOT_MAX) return;
    if (pixels == NULL || w16 <= 0 || h <= 0) return;
    /* 4/8bpp only, and not a fastidious guard: HiresOverride_LookupByTpageClut
     * rejects the virtual-clut key outright on a 16bpp tpage (tp >= 2), because
     * PSX hardware ignores `clut` in 16bpp mode and framebuffer-sampling prims
     * ship garbage clut bytes there. A 16bpp pool slot can therefore never be
     * wanted, so retaining its source would be dead memory - and the eager loop
     * composing those rows was dead work. */
    if (bpp != 4 && bpp != 8) return;
    if (clut == NULL || clutW <= 0 || clutRows <= 0) return;

    /* A prim selects its palette from a 4-bit row field, so rows past
     * HIRES_POOL_MAX_ROWS can never be addressed on this path (the pack loop
     * capped them the same way). Retaining them would never be read. */
    if (clutRows > HIRES_POOL_MAX_ROWS) clutRows = HIRES_POOL_MAX_ROWS;

    /* Frees the previous occupant and credits back the exact figure it added,
     * so slot recycling cannot drift the running total. */
    TexPackLazy_DropSlot(slotId);

    pxBytes = (size_t)w16 * (size_t)h * 2;
    clBytes = (size_t)clutW * (size_t)clutRows * 2;

    if (g_srcBytes + (long long)(pxBytes + clBytes) > LAZY_SRC_CAP_BYTES)
    {
        static int s_capLog = 0;
        if (!s_capLog)
        {
            s_capLog = 1;
            SH_DBG("[TEXPACK/LAZY] retain cap reached (%lld bytes live) - further slots keep native art",
                   g_srcBytes);
        }
        return;
    }

    s = &g_slots[slotId];
    s->pixels = (unsigned char*)malloc(pxBytes);
    s->clut   = (unsigned short*)malloc(clBytes);
    if (s->pixels == NULL || s->clut == NULL)
    {
        free(s->pixels);
        free(s->clut);
        memset(s, 0, sizeof(*s));
        return;
    }
    memcpy(s->pixels, pixels, pxBytes);
    memcpy(s->clut, clut, clBytes);

    s->w16     = w16;
    s->h       = h;
    s->clutW   = clutW;
    s->rows    = clutRows;
    s->bpp     = bpp;
    s->nativeW = nativeW;
    s->nativeH = nativeH;
    s->bytes   = (long long)(pxBytes + clBytes);
    g_srcBytes += s->bytes;

    if (timName != NULL)
    {
        strncpy(s->name, timName, sizeof(s->name) - 1);
    }
}

/* Put an evicted pack row back to correct NATIVE art and re-arm it for compose.
 *
 * Clearing `done` is what makes eviction a cache rather than a demotion: the
 * next prim that samples the row re-enqueues it, and it composes again if the
 * budget now allows. Without the clear an evicted row would be native forever,
 * which is the very failure eviction exists to remove. */
/* Eviction gate: a row is only recyclable while this slot still holds the TIM
 * blocks its native art is expanded from. */
static int Lazy_CanRestore(int slotId, int row)
{
    const LazySlot* s;

    if (slotId < 0 || slotId >= HIRES_POOL_SLOT_MAX) return 0;
    if (row < 0 || row >= HIRES_POOL_MAX_ROWS) return 0;

    s = &g_slots[slotId];
    return s->pixels != NULL && s->clut != NULL && row < s->rows;
}

void TexPackLazy_RestoreNativeRow(int slotId, int row)
{
    LazySlot* s;

    if (slotId < 0 || slotId >= HIRES_POOL_SLOT_MAX) return;
    if (row < 0 || row >= HIRES_POOL_MAX_ROWS) return;

    s = &g_slots[slotId];
    if (s->pixels != NULL && s->clut != NULL && row < s->rows)
    {
        HiresOverride_PoolSlotRestoreNativeRow(
            slotId, row, s->pixels, s->w16, s->h,
            s->clut + (size_t)row * (size_t)s->clutW, s->clutW,
            s->bpp, s->nativeW, s->nativeH);
    }

    /* Re-arm even when the source is gone (slot dropped under us): the row is
     * simply left to whatever the disc-TIM registration put there. */
    s->done   &= ~(1u << row);
    s->queued &= ~(1u << row);
}

void TexPackLazy_NoteWanted(int slotId, int row)
{
    LazySlot*    s;
    unsigned int m;

    if (slotId < 0 || slotId >= HIRES_POOL_SLOT_MAX) return;
    if (row < 0 || row >= HIRES_POOL_MAX_ROWS) return;

    /* The "nothing retained" early-out is this per-slot pointer test and NOT a
     * test on g_srcBytes: that total is clamped to 0 if the accounting ever
     * goes negative, and gating the want signal on it would let one accounting
     * slip silently kill lazy compose for the whole session - the same class of
     * failure the exact-bytes bookkeeping above exists to prevent. It is also
     * cheaper on the per-prim path (no global load). */
    s = &g_slots[slotId];
    if (s->pixels == NULL || row >= s->rows) return;

    m = 1u << row;
    if ((s->queued | s->done) & m) return;

    /* Mark the row queued ONLY if the ring actually accepted it. Marking first
     * and enqueueing conditionally would make a row refused by a full ring
     * permanently native: the bit test above would never let it be retried. */
    if (ring_push(((unsigned int)slotId << 5) | (unsigned int)row))
    {
        s->queued |= m;
    }
}

void TexPackLazy_Pump(void)
{
    Uint64    freq;
    Uint64    start;
    long long budgetTicks;
    int       budgetMs;
    int       serviced = 0;
    int       built    = 0;

    /* Decays on presented frames, not on work done, so the burst is a fixed
     * wall-clock window after a map load however little there is to do. */
    if (g_burstFrames > 0) g_burstFrames--;

    /* Advance the LRU clock every pump, including the early-out below: rows
     * must keep ageing while there is no compose work, or a quiet stretch would
     * freeze every age and make the next eviction pass see nothing as cold. */
    HiresOverride_Tick();

    budgetMs = g_PcConfig.texpackLazyMs;
    if (budgetMs < 1) budgetMs = 1;
    if (g_burstFrames > 0) budgetMs *= LAZY_BURST_SCALE;

    /* SDL_GetPerformanceCounter, not SDL_GetTicks: the budget is a few
     * milliseconds and a run of compose-cache hits costs tens of microseconds
     * each, which a 1 ms-resolution clock cannot separate from free. Same
     * source the frame limiter and GsGetVcount already use. */
    freq        = SDL_GetPerformanceFrequency();
    budgetTicks = (long long)((freq * (Uint64)(unsigned)budgetMs) / 1000u);
    start       = SDL_GetPerformanceCounter();

    /* Land what the worker finished first: those rows are pure upload now and
     * must not wait behind fresh submissions. Runs even with an empty ring. */
    Lazy_Collect(start, budgetTicks, &serviced, &built);

    if (g_ringHead == g_ringTail) goto pump_done;

    /* Budget spent: recycle the coldest rows instead of giving up on the run.
     *
     * This used to `return` unconditionally, which made the budget a one-way
     * ratchet - once any single scene filled it, every later row kept native
     * art for the rest of the session no matter how far the player walked from
     * whatever filled it ("uses less VRAM, but stops loading textures after
     * some point"). Evicting the least-recently-SAMPLED rows turns the budget
     * into a working-set target: cold art is given back, hot art stays.
     *
     * If nothing is old enough to evict, the hot set alone exceeds the budget;
     * the ring is left INTACT (rather than drained into no-ops that would mark
     * every queued row done) and those rows keep native art until there is room
     * - the old behaviour, now reached only in the case that actually warrants
     * it. */
    if (HiresOverride_PackBudgetExceeded())
    {
        int evicted = 0;
        int vramEvicted = 0;

        /* Evict down to the TARGET, not merely back under the cap — see
         * HiresOverride_PackBudgetOverTarget. Stopping the moment the cap
         * clears leaves no room for the eager VRAM-entry path and recycles one
         * row per pump indefinitely. */
        while (evicted < LAZY_EVICT_MAX_PER_PUMP && HiresOverride_PackBudgetOverTarget())
        {
            int slotId = -1, row = -1;
            if (!HiresOverride_EvictColdestPackRow(LAZY_EVICT_MIN_AGE, Lazy_CanRestore,
                                                   &slotId, &row))
                break;
            /* Mandatory for a POOL row: left empty it resolves to ROW 0's
             * palette in the lookup, not to native art. slotId < 0 means the
             * evictor fell through to a VRAM entry, which needs no restore —
             * with the entry gone the prim samples real VRAM. */
            if (slotId >= 0)
                TexPackLazy_RestoreNativeRow(slotId, row);
            else
                vramEvicted++;
            evicted++;
        }

        if (evicted > 0 && g_pumpLogs < 256)
        {
            g_pumpLogs++;
            SH_DBG("[TEXPACK/LRU] evicted %d cold row(s) (%d VRAM-entry), %lld MB pack GL live",
                   evicted, vramEvicted, HiresOverride_PackBytesLive() >> 20);
        }

        if (HiresOverride_PackBudgetExceeded())
        {
            static int s_budgetLog = 0;
            if (!s_budgetLog)
            {
                s_budgetLog = 1;
                SH_DBG("[TEXPACK] GL byte budget reached and the hot set fills it - "
                       "further pool rows keep native art until something goes cold");
            }
            return;
        }
    }

    while (g_ringHead != g_ringTail)
    {
        unsigned int         key;
        int                  slotId, row, cw = 0, ch = 0;
        LazySlot*            s;
        const unsigned char* canvas;

        /* Always make at least one row of progress per frame, then stop as soon
         * as the wall-clock budget is gone. Checked BEFORE the pop so a row that
         * does not fit stays queued rather than being popped and dropped.
         *
         * The budget deliberately counts every serviced row rather than only
         * the ones TexPack_LastComposeWasBuilt() reports: on a fresh row the
         * glTexImage2D + mip generation of a multi-MB canvas is the larger half
         * of the cost, so gating on "expensive compose" would let a run of cache
         * hits spend several unbudgeted milliseconds of upload. Granularity is
         * one row: a genuinely built row is ~7.6 ms, so a 4 ms budget still
         * costs a full row on the frames that build. */
        if (serviced > 0 &&
            (long long)(SDL_GetPerformanceCounter() - start) >= budgetTicks)
        {
            break;
        }

        key    = g_ring[g_ringTail++ & (LAZY_RING - 1)];
        slotId = (int)(key >> 5);
        row    = (int)(key & 31);
        s      = &g_slots[slotId];

        /* The slot was dropped, or re-registered with a different TIM, while
         * this key sat in the ring. Either way the row bit is no longer set,
         * and a prim that still wants the row has re-enqueued it against the
         * new source. Costs nothing, so it does not count as serviced. */
        if (s->pixels == NULL || row >= s->rows) continue;
        if (!(s->queued & (1u << row))) continue;

        if (g_PcConfig.texpackWorkerThread && Lazy_WorkerReady())
        {
            if (!Lazy_SubmitRow(slotId, row, &serviced))
            {
                /* Every job slot is in flight: put the key back and stop
                 * submitting this pump. If the ring is somehow full as well,
                 * clear the bit so the next sample re-enqueues it. */
                if (!ring_push(key)) s->queued &= ~(1u << row);
                break;
            }
            continue;
        }

        s->queued &= ~(1u << row);
        s->done   |= 1u << row;

        canvas = TexPack_Compose(s->pixels, s->w16, s->h,
                                 s->clut + (size_t)row * (size_t)s->clutW,
                                 s->clutW, s->bpp, &cw, &ch);
        serviced++;
        if (TexPack_LastComposeWasBuilt()) built++;

        if (canvas != NULL)
        {
            /* canvas is owned by the compose cache - no free. Keyed on the
             * compose content hash so an unchanged re-upload skips the
             * glTexImage2D churn. */
            HiresOverride_PoolSlotRegisterRGBAKeyed(slotId, row, canvas, cw, ch,
                                                    s->nativeW, s->nativeH,
                                                    TexPack_LastComposeHash());
        }
        else if (TexPack_LastComposeIsDds())
        {
            /* Whole-upload BC7 pack entry: upload the compressed blocks straight
             * to this slot row. The bytes are owned by tex_pack.c and released
             * by the NEXT TexPack_Compose call, so they must be consumed here,
             * before the loop can iterate. */
            size_t               ddsSize = 0;
            const unsigned char* dds     = TexPack_LastComposeDds(&ddsSize);
            HiresOverride_PoolSlotRegisterDdsKeyed(slotId, row, dds, ddsSize,
                                                   s->nativeW, s->nativeH,
                                                   TexPack_LastComposeHash());
        }
        else
        {
            /* The pack ships no palette match for this row, so it keeps native
             * art. Worth naming: the symptom is one body region of a monster
             * staying at native resolution while the rest is HD, and offline it
             * is a hash hunt. Rows resolve one at a time now, which is why this
             * replaces the old per-upload coverage summary
             * (TexPack_ReportUncoveredRows, still used by the VRAM path). */
            static int s_missLog = 0;
            if (s_missLog < 64)
            {
                s_missLog++;
                SH_DBG("[TEXPACK] %s (pool slot %d): NO replacement for CLUT row %d - keeps native art",
                       s->name, slotId, row);
            }
        }

        /* The retained source is deliberately NOT freed once every row has
         * resolved (it used to be). LRU eviction has to be able to put a
         * recycled row back to correct native art, and the palette expansion
         * that does it needs exactly these pixel+CLUT blocks; without them an
         * evicted row would fall back to ROW 0's palette. The sources are
         * bounded by the live slots and dropped on slot re-register and map
         * reset, so this is a structural few tens of MB against a
         * multi-gigabyte texture budget - see LAZY_SRC_CAP_BYTES.
         *
         * A row can tip the GL budget over mid-slice; stop before composing
         * another one that would only be thrown away. The eviction pass at the
         * top of the next pump reclaims room if any exists. */
        if (HiresOverride_PackBudgetExceeded()) break;
    }

pump_done:
    if (built > 0 && g_pumpLogs < 256)
    {
        g_pumpLogs++;
        SH_DBG("[TEXPACK/LAZY] %d built / %d serviced, %u queued, %s budget %d ms, %lld KB retained",
               built, serviced, g_ringHead - g_ringTail,
               (g_burstFrames > 0) ? "burst" : "steady", budgetMs, g_srcBytes >> 10);
    }
}
