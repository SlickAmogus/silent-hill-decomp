/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * stubs_ps3.c - the subsystems not ported yet, and the one place they are
 * allowed to be absent.
 *
 * Everything here corresponds to an xbox_port file that is nxdk-bound and has
 * no PS3 version: xa_xbox.c (CD audio), fmv_xbox.c (movies), ra_xbox.c +
 * ra_badge_xbox.c (RetroAchievements), dbg_overlay_xbox.c (on-screen console).
 * None of them are on the path to a first boot.
 *
 * Each stub is INERT, never plausible: no fake success that the caller would act
 * on. FMV_Play reports "did not play" so the game advances past the movie rather
 * than waiting on frames that never arrive; the XA queries report "nothing
 * playing" so the audio mixer does not stall waiting for a stream to drain.
 * The alternative -- claiming success -- turns a missing subsystem into a hang.
 */
#include <stdint.h>

#include <sys/process.h>

#include "sh_log.h"

/* ---- XA / CD audio (xa_xbox.c) ------------------------------------------- */

void XaPlayer_Play(uint16_t xaIdx)
{
    static uint16_t s_lastIdx = 0xFFFF;
    if (xaIdx != s_lastIdx) {            /* called repeatedly for the same track */
        SH_DBG("[XA] stub: track %u requested, no CD audio backend yet", (unsigned)xaIdx);
        s_lastIdx = xaIdx;
    }
}

void XaPlayer_SetVolume(int16_t volLeft, int16_t volRight) { (void)volLeft; (void)volRight; }
void XaPlayer_Stop(void)   { }
void XaPlayer_Update(void) { }

/* "Nothing is draining / no gap to hold" -- reporting the opposite would park
 * the voice mixer waiting on a stream that will never finish. */
int Xa_IsVoiceAudioDraining(void) { return 0; }
int Xa_VoiceGapHold(void)         { return 0; }

/* ---- FMV (fmv_xbox.c) ---------------------------------------------------- */

/* 0 = did not play. The caller then continues immediately instead of blocking
 * on a decoder that does not exist. */
int FMV_Play(int file_idx, int max_frames)
{
    (void)max_frames;
    SH_DBG("[FMV] stub: movie %d skipped, no PS3 decoder yet", file_idx);
    return 0;
}

/* ---- RetroAchievements (ra_xbox.c, ra_badge_xbox.c) ---------------------- */

void Pc_Ra_Update(void)         { }
void Pc_Ra_StatusToast(void)    { }
void RaBadge_RenderDirect(void) { }

/* ---- Debug overlay (dbg_overlay_xbox.c) ---------------------------------- */

/* The on-screen console needs a working renderer, which is exactly what does
 * not exist yet. The log file carries the same lines in the meantime. */
void DbgOverlay_XboxRender(void) { }

/* ---- Shutdown ------------------------------------------------------------ */

/* Unlike the 360 -- where XeLL handed control to a bare-metal ELF and the only
 * honest option was to halt in place -- the PS3 runs under GameOS and can
 * actually go back to the XMB, which is what a user pressing "quit" expects.
 * Flush first: sysProcessExit does not unwind, so an unflushed log is lost. */
void Xbox_QuitToDashboard(void)
{
    extern void SH_DebugLogFlush(void);
    SH_DBG("[SYS] quit requested - returning to XMB");
    SH_DebugLogFlush();
    sysProcessExit(0);
}
