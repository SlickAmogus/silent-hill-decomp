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

/* XA (xa_xbox.c) and FMV (fmv_xbox.c) are real on this build now: the
 * shared players compile for the 360 with a ms clock and an XAudio2 flush
 * from the HAL (see their SH_XBOX360_PORT blocks). */

/* ---- RetroAchievements (ra_xbox.c, ra_badge_xbox.c) ---------------------- */

void Pc_Ra_Update(void)         { }
void Pc_Ra_StatusToast(void)    { }
void RaBadge_RenderDirect(void) { }

/* ---- Debug overlay (dbg_overlay_xbox.c) ---------------------------------- */

void DbgOverlay_XboxRender(void) { }
