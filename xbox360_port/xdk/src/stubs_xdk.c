/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * stubs_xdk.c - the subsystems not ported yet, and the one place they are
 * allowed to be absent. Same set as stubs_xbox360.c (libXenon); the quit path
 * lives in main_xdk.cpp because here there IS a dashboard to return to.
 *
 * Each stub is INERT, never plausible: FMV_Play reports "did not play" so the
 * game advances past the movie; the XA queries report "nothing playing" so the
 * audio mixer does not stall waiting for a stream to drain.
 */
#include <stdint.h>

#include "sh_log.h"

/* ---- XA / CD audio (xa_xbox.c) ------------------------------------------- */

void XaPlayer_Play(uint16_t xaIdx)
{
    static uint16_t s_lastIdx = 0xFFFF;
    if (xaIdx != s_lastIdx) {
        SH_DBG("[XA] stub: track %u requested, no CD audio backend yet", (unsigned)xaIdx);
        s_lastIdx = xaIdx;
    }
}

void XaPlayer_SetVolume(int16_t volLeft, int16_t volRight) { (void)volLeft; (void)volRight; }
void XaPlayer_Stop(void)   { }
void XaPlayer_Update(void) { }

int Xa_IsVoiceAudioDraining(void) { return 0; }
int Xa_VoiceGapHold(void)         { return 0; }

/* ---- FMV (fmv_xbox.c) ---------------------------------------------------- */

int FMV_Play(int file_idx, int max_frames)
{
    (void)max_frames;
    SH_DBG("[FMV] stub: movie %d skipped, no 360 decoder yet", file_idx);
    return 0;
}

/* ---- RetroAchievements (ra_xbox.c, ra_badge_xbox.c) ---------------------- */

void Pc_Ra_Update(void)         { }
void Pc_Ra_StatusToast(void)    { }
void RaBadge_RenderDirect(void) { }

/* ---- Debug overlay (dbg_overlay_xbox.c) ---------------------------------- */

void DbgOverlay_XboxRender(void) { }
