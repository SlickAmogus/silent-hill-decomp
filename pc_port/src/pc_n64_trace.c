/* SPDX-License-Identifier: GPL-3.0-or-later */
/* TEMPORARY N64 comparison capture. See pc_n64_trace.h. Remove with its four
 * call sites when the N64 framing is settled. */

#include "pc_n64_trace.h"

#include "game.h"
#include "sh_log.h"

/* Boot-into-a-map lands in gameplay well before the chunk streamer has caught
 * up, and a capture taken then is of half a room. Two seconds of gameplay with
 * the world actually on screen every frame is a cheap way to be sure the
 * geometry being measured is the geometry the player sees. */
#define N64_TRACE_SETTLE_FRAMES  120
#define N64_TRACE_SLICE_FRAMES   60   /* one "second" at the PSX rate */
#define N64_TRACE_SLICES         5
#define N64_TRACE_VANG_PER_SLICE 4

enum { N64_WAITING = 0, N64_CAPTURING, N64_DONE };

static int s_phase;
static int s_settled;      /* consecutive good frames before arming */
static int s_frame;        /* frames since arming */
static int s_slice;        /* which slice we are in */
static int s_goldDone;     /* GOLD already emitted this slice */
static int s_vangLeft;     /* VANG budget left this slice */
static int s_bonesDone;

/* Per-frame census, summed across every func_8005AC50 call. */
static int s_prims, s_passed, s_tris, s_depthRej, s_backRej, s_oobRej;

int N64Trace_Active(void)
{
    return s_phase == N64_CAPTURING;
}

int N64Trace_GoldWant(void)
{
    if (s_phase != N64_CAPTURING || s_goldDone)
        return 0;
    s_goldDone = 1;
    return 1;
}

int N64Trace_VangWant(void)
{
    if (s_phase != N64_CAPTURING || s_vangLeft <= 0)
        return 0;
    s_vangLeft--;
    return 1;
}

int N64Trace_BonesWant(void)
{
    if (s_phase != N64_CAPTURING || s_bonesDone)
        return 0;
    s_bonesDone = 1;
    return 1;
}

void N64Trace_AddCensus(int prims, int passed, int tris, int depthRej,
                        int backfaceRej, int oobRej)
{
    if (s_phase != N64_CAPTURING)
        return;
    s_prims    += prims;
    s_passed   += passed;
    s_tris     += tris;
    s_depthRej += depthRej;
    s_backRej  += backfaceRej;
    s_oobRej   += oobRej;
}

void N64Trace_Tick(void)
{
    extern int g_PcWorldDrawnThisFrame;
    int good;

    if (s_phase == N64_DONE)
        return;

    good = (g_GameWork.gameState == GameState_InGame &&
            g_SysWork.sysState   == SysState_Gameplay &&
            g_PcWorldDrawnThisFrame);

    if (s_phase == N64_WAITING)
    {
        /* Any interruption restarts the count: a door, a cutscene or a stall
         * mid-settle would otherwise let the capture start on the frame the
         * world came back, which is exactly the half-loaded case. */
        s_settled = good ? s_settled + 1 : 0;
        if (s_settled < N64_TRACE_SETTLE_FRAMES)
            return;

        s_phase    = N64_CAPTURING;
        s_frame    = 0;
        s_slice    = 0;
        s_goldDone = 0;
        s_vangLeft = N64_TRACE_VANG_PER_SLICE;
        SH_LOG("[N64TRACE] armed after %d settled frames — map=%d, %d slices "
               "of %d frames", N64_TRACE_SETTLE_FRAMES,
               g_SavegamePtr != NULL ? (int)g_SavegamePtr->mapIdx : -1,
               N64_TRACE_SLICES, N64_TRACE_SLICE_FRAMES);
        return;
    }

    /* Census first: it describes the frame that just finished. */
    if (s_prims > 0)
    {
        /* tris counts a passed quad as two, which is what a triangle-rate
         * comparison against the N64 needs; prims is the raw primitive count. */
        SH_LOG("[DRAW] tris=%d prims=%d passed=%d backface=%d depth=%d oob=%d",
               s_tris, s_prims, s_passed, s_backRej, s_depthRej, s_oobRej);
    }
    s_prims = s_passed = s_tris = s_depthRej = s_backRej = s_oobRej = 0;

    s_frame++;
    if ((s_frame % N64_TRACE_SLICE_FRAMES) == 0)
    {
        s_slice++;
        if (s_slice >= N64_TRACE_SLICES)
        {
            s_phase = N64_DONE;
            SH_LOG("[N64TRACE] capture complete — %d slices; nothing further "
                   "will print this session", N64_TRACE_SLICES);
            return;
        }
        s_goldDone = 0;
        s_vangLeft = N64_TRACE_VANG_PER_SLICE;
    }
}
