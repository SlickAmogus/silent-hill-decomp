#include "game.h"

#include <psyq/libetc.h>
#include <psyq/libgs.h>

#include "bodyprog/bodyprog.h"
#include "bodyprog/screen/screen_data.h"
#include "bodyprog/screen/screen_draw.h"
#include "bodyprog/math/math.h"
#include "main/fsqueue.h"

#ifdef SH_PC_PORT
#include "sh_log.h"
#endif

const s32 rodataPad_80024CA0 = 0;
s32       __pad_bss_800B5C2C;

// ========================================
// GLOBAL VARIABLES
// ========================================

q19_12 g_ScreenFadeTimestep;

// ========================================
// STATIC VARIABLES
// ========================================

static q19_12 g_PrevScreenFadeProgress;

#ifdef SH_PC_PORT
/* PsyCross has a 12-byte P_TAG header (addr:8 + len:2 + pgxp:2) instead of PSX's 4-byte tag.
 * Static initializers must match the actual struct layout. */
static DR_MODE D_800A8E5C[2];
static TILE    D_800A8E74[2];
static int     s_fadeStaticsInited = 0;

static void Screen_FadeInitStatics(void)
{
    if (s_fadeStaticsInited) return;
    s_fadeStaticsInited = 1;
    for (int i = 0; i < 2; i++) {
        setcode(&D_800A8E5C[i], 0xE1); // P_TAG code byte — ParsePrimitive dispatches 0xE0 → ProcessDrawEnv
        setlen(&D_800A8E5C[i], 1);     // 1 long: codePtr[0]=code[0]=0xE1000240 (primSubType=1 → tpage=64, BM_SUBTRACT)
        D_800A8E5C[i].code[0] = 0xE1000240;

        setlen(&D_800A8E74[i], 3);
        D_800A8E74[i].r0   = 255;
        D_800A8E74[i].g0   = 0;
        D_800A8E74[i].b0   = 0;
        D_800A8E74[i].code = 0x62;
        /* PSX tile was x0=-SCREEN_WIDTH, w=SCREEN_WIDTH*2 → spans [-320,+320],
         * which exactly meets the 4:3 right edge (320). In Hor+ widescreen the
         * ortho extends to ~320+margin (e.g. 360 @16:9), so the right margin
         * fell outside the fade tile and showed un-faded/stale VRAM during black
         * transitions ("corruption on the right"). Widen well past any widescreen
         * margin so the fade covers the full visible area. Off-screen excess is
         * clipped on 4:3, so this stays PSX-faithful there. */
        D_800A8E74[i].x0   = -SCREEN_WIDTH;
        D_800A8E74[i].y0   = -SCREEN_HEIGHT;
        D_800A8E74[i].w    = SCREEN_WIDTH * 4;
        D_800A8E74[i].h    = SCREEN_HEIGHT * 2;
    }
}
#else
static DR_MODE D_800A8E5C[] = {
    { 0x3000000, { 0xE1000240, 0x0 } },
    { 0x3000000, { 0xE1000240, 0x0 } }
};

static TILE D_800A8E74[] = {
    { 0x3000000, 255, 0, 0, 0x62, -SCREEN_WIDTH, -SCREEN_HEIGHT, SCREEN_WIDTH * 2, SCREEN_HEIGHT * 2 },
    { 0x3000000, 255, 0, 0, 0x62, -SCREEN_WIDTH, -SCREEN_HEIGHT, SCREEN_WIDTH * 2, SCREEN_HEIGHT * 2 }
};
#endif

static q19_12 g_ScreenFadeProgress = Q12(0.0f);

#ifdef SH_PC_PORT
/* The fade step is timestep*dt, so it is naturally fps-independent (~PSX
 * duration at any framerate). Two PC hazards: (1) g_DeltaTimeRaw can read 0/
 * stale inside blocking load loops -> the fade never advances (315-frame hangs
 * the old *4 hack papered over); (2) during a room/area load g_DeltaTimeRaw
 * SPIKES (the load runs alongside the fade), and an un-clamped step jumps
 * progress 1.0->0 in one frame -> the fade is invisible (the reported "no fade
 * on transitions"). Clamp the fade's dt to [1/60, 1/30]: floored so it always
 * advances, capped so a load hitch can't skip it. No *4 — that just made PC
 * fades 4x too fast. */
static q19_12 Screen_FadeDtGet(void)
{
    q19_12 dt = g_DeltaTimeRaw;
    if (dt < Q12(1.0f / 60.0f)) dt = Q12(1.0f / 60.0f);
    if (dt > Q12(1.0f / 30.0f)) dt = Q12(1.0f / 30.0f);
    return dt;
}
#endif

void Screen_FadeDrawModeSet(DR_MODE* drMode) // 0x800325A4
{
    if (IS_SCREEN_FADE_WHITE(g_Screen_FadeStatus))
    {
        SetDrawMode(drMode, 0, 1, 32, NULL);
    }
    else
    {
        SetDrawMode(drMode, 0, 1, 64, NULL);
    }
#ifdef SH_PC_PORT
    /* setDrawMode does NOT set the P_TAG .code byte. Without setcode, ParsePrimitive
     * routes on code&0xF0=0x00 instead of 0xE0, so ProcessDrawEnv never runs and
     * activeDrawEnv.tpage stays 0 → BM_AVERAGE → opaque white TILE flash. */
    setcode(drMode, 0xE1);
    setlen(drMode, 1);
#endif
}

q19_12 Screen_FadeInProgressGet(void) // 0x800325F8
{
    return Q12(1.0f) - g_PrevScreenFadeProgress;
}

void Screen_FadeUpdate(void) // 0x8003260C
{
    s32      queueLength;
    s32      timestep;
    GsOT*    ot;
    TILE*    tile;
    DR_MODE* drMode;

#ifdef SH_PC_PORT
    Screen_FadeInitStatics();
#endif
    drMode                   = &D_800A8E5C[g_ActiveBufferIdx];
    tile                     = &D_800A8E74[g_ActiveBufferIdx];
    g_PrevScreenFadeProgress = g_ScreenFadeProgress;

    switch (g_Screen_FadeStatus)
    {
        case SCREEN_FADE_STATUS(ScreenFadeState_FadeOutStart, false):
        case SCREEN_FADE_STATUS(ScreenFadeState_FadeOutStart, true):
            g_ScreenFadeProgress = Q12(0.0f);
            g_Screen_FadeStatus++;

        case SCREEN_FADE_STATUS(ScreenFadeState_FadeOutSteps, false):
        case SCREEN_FADE_STATUS(ScreenFadeState_FadeOutSteps, true):
            Screen_FadeDrawModeSet(drMode);
            queueLength = Fs_QueueGetLength();

            if (g_ScreenFadeTimestep > Q12(0.0f))
            {
                timestep = g_ScreenFadeTimestep;
            }
            else
            {
                timestep = Q12(3.0f) / (queueLength + 1);
            }

#ifdef SH_PC_PORT
            g_ScreenFadeProgress += Q12_MULT_PRECISE(timestep, Screen_FadeDtGet());
#else
            g_ScreenFadeProgress += Q12_MULT_PRECISE(timestep, g_DeltaTimeRaw);
#endif
            if (g_ScreenFadeProgress >= Q12_CLAMPED(1.0f))
            {
                g_ScreenFadeProgress = Q12_CLAMPED(1.0f);
                g_Screen_FadeStatus++;
            }

            tile->r0 = Q12_TO_Q8(g_ScreenFadeProgress);
            tile->g0 = Q12_TO_Q8(g_ScreenFadeProgress);
            tile->b0 = Q12_TO_Q8(g_ScreenFadeProgress);
            break;

        case SCREEN_FADE_STATUS(ScreenFadeState_ResetTimestep, false):
        case SCREEN_FADE_STATUS(ScreenFadeState_ResetTimestep, true):
            g_ScreenFadeTimestep = Q12(0.0f);

        case SCREEN_FADE_STATUS(ScreenFadeState_FadeInStart, false):
        case SCREEN_FADE_STATUS(ScreenFadeState_FadeInStart, true):
            g_ScreenFadeProgress = Q12_CLAMPED(1.0f);
            g_Screen_FadeStatus++;

        case SCREEN_FADE_STATUS(ScreenFadeState_FadeOutComplete, false):
        case SCREEN_FADE_STATUS(ScreenFadeState_FadeOutComplete, true):
            Screen_FadeDrawModeSet(drMode);
            tile->r0 = Q12_TO_Q8(g_ScreenFadeProgress);
            tile->g0 = Q12_TO_Q8(g_ScreenFadeProgress);
            tile->b0 = Q12_TO_Q8(g_ScreenFadeProgress);
            break;

        case SCREEN_FADE_STATUS(ScreenFadeState_FadeInSteps, false):
        case SCREEN_FADE_STATUS(ScreenFadeState_FadeInSteps, true):
            Screen_FadeDrawModeSet(drMode);

            if (g_ScreenFadeTimestep > Q12(0.0f))
            {
                timestep = g_ScreenFadeTimestep;
            }
            else
            {
                timestep = Q12(3.0f);
            }

#ifdef SH_PC_PORT
            g_ScreenFadeProgress -= Q12_MULT_PRECISE(timestep, Screen_FadeDtGet());
#else
            g_ScreenFadeProgress -= Q12_MULT_PRECISE(timestep, g_DeltaTimeRaw);
#endif

            if (g_ScreenFadeProgress <= Q12(0.0f))
            {
                g_ScreenFadeProgress = Q12(0.0f);
                ScreenFade_Reset();
                return;
            }

            tile->r0 = Q12_TO_Q8(g_ScreenFadeProgress);
            tile->g0 = Q12_TO_Q8(g_ScreenFadeProgress);
            tile->b0 = Q12_TO_Q8(g_ScreenFadeProgress);
            break;

        case SCREEN_FADE_STATUS(ScreenFadeState_Reset, false):
            g_ScreenFadeTimestep = Q12(0.0f);
            g_ScreenFadeProgress = Q12(0.0f);
            g_Screen_FadeStatus  = SCREEN_FADE_STATUS(ScreenFadeState_None, false);
            return;

        case SCREEN_FADE_STATUS(ScreenFadeState_None, false):
            return;
    }

    ot = &g_OtTags0[g_ActiveBufferIdx][5];
    AddPrim(ot, tile);
    AddPrim(ot, drMode);
}
