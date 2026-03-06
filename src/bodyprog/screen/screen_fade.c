#include "game.h"
#ifdef SH_PC_PORT
#include <stdio.h>
#endif

#include <psyq/libetc.h>
#include <psyq/libgs.h>

#include "bodyprog/bodyprog.h"
#include "bodyprog/screen/screen_data.h"
#include "bodyprog/screen/screen_draw.h"
#include "bodyprog/math/math.h"
#include "main/fsqueue.h"

const s32 rodataPad_80024CA0 = 0;
s32       pad_bss_800B5C2C;

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
        setlen(&D_800A8E5C[i], 3);
        D_800A8E5C[i].code[0] = 0xE1000240;
        D_800A8E5C[i].code[1] = 0x0;

        setlen(&D_800A8E74[i], 3);
        D_800A8E74[i].r0   = 255;
        D_800A8E74[i].g0   = 0;
        D_800A8E74[i].b0   = 0;
        D_800A8E74[i].code = 0x62;
        D_800A8E74[i].x0   = -SCREEN_WIDTH;
        D_800A8E74[i].y0   = -SCREEN_HEIGHT;
        D_800A8E74[i].w    = SCREEN_WIDTH * 2;
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
    /* setDrawMode sets len=3, but DR_MODE only has 2 u_ints of payload (code[0], code[1]).
     * PsyCross's parser uses len to bound its inner loop. With tw=NULL, code[1]=0 acts
     * as a terminator, but the parser still tries to parse the remaining difference.
     * Set len to 1 since only code[0] is meaningful when tw is NULL. */
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

#ifdef SH_PC_PORT
    {
        static int prevStatus = -1;
        if (g_Screen_FadeStatus != prevStatus) {
            printf("[SH] Screen_FadeUpdate: status=%d->%d progress=%d dtRaw=%d timestep=%d\n",
                prevStatus, g_Screen_FadeStatus, g_ScreenFadeProgress, g_DeltaTimeRaw, g_ScreenFadeTimestep);
            prevStatus = g_Screen_FadeStatus;
        }
    }
#endif
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
            /* Accelerate fades on PC: ensure minimum delta and use larger timestep
             * to avoid 315-frame waits in internal loops where delta time may be stale. */
            {
                s32 effectiveDt = g_DeltaTimeRaw > 0 ? g_DeltaTimeRaw : Q12(1.0f / 60.0f);
                g_ScreenFadeProgress += Q12_MULT_PRECISE(timestep * 4, effectiveDt);
            }
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
            {
                s32 effectiveDt = g_DeltaTimeRaw > 0 ? g_DeltaTimeRaw : Q12(1.0f / 60.0f);
                s32 decrement = Q12_MULT_PRECISE(timestep * 4, effectiveDt);
                g_ScreenFadeProgress -= decrement;
                {
                    static int fadeInDbg = 0;
                    if (fadeInDbg < 10) {
                        printf("[SH] FadeIn: progress=%d dec=%d ts=%d dt=%d\n",
                            g_ScreenFadeProgress, decrement, timestep, effectiveDt);
                        fadeInDbg++;
                    }
                }
            }
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
