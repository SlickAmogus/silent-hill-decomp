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

/* ---- test hook: autoplay "WARP" -------------------------------------------- */
/* Places Harry at (x, z) metres facing rotQ12, for scripted Xenia test runs
 * (pad_xinput.cpp autoplay). Never reached without an autoplay.txt line. */
#include "game.h"
#include "bodyprog/bodyprog.h"
#include "sh_log.h"
void Debug_PlayerWarp(float x, float z, int rotQ12)
{
    s_SubCharacter* h = &g_SysWork.playerWork.player;
    h->position.vx = (q19_12)(x * 4096.0f);
    h->position.vz = (q19_12)(z * 4096.0f);
    h->rotation.vy = (q3_12)rotQ12;
    SH_DBG("[WARP] Harry -> (%d, %d) rotY=%d", (int)h->position.vx, (int)h->position.vz, rotQ12);
}

/* autoplay "EVENT n" -> events_main.c Event_Update test hook. */
int g_DebugForceEventIdx = -1;
