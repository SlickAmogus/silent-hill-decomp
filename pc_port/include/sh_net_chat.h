/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_chat.h - in-game text chat for the living world.
 *
 * TWO CHANNELS, matching the two things "who can hear me" can mean while the
 * world is ghosts-only:
 *   GLOBAL  everyone connected to the master server.
 *   GAME    everyone standing in the same map (a "game", for now). When real
 *           co-op lands, GAME becomes the session with no change to this file.
 *
 * The window is a sibling of the debug console: same bottom-of-screen slab and
 * the same typed-line model, but anchored bottom-LEFT, smaller, and it never
 * freezes the game. Lines fade out after a while of quiet, like any game chat,
 * and reappear the instant a message arrives or you start typing.
 *
 * GAME-THREAD ONLY. It pulls received lines from the network worker through
 * ShNet_PopChat and hands typed lines back through ShNet_SendChat; the worker
 * owns the socket. The keyboard-to-text mapping lives here (not in the input
 * layer) so the whole feature is one file.
 */
#ifndef SH_NET_CHAT_H
#define SH_NET_CHAT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Once per frame from the game tick: drain received lines and age the log. */
void ShNetChat_Tick(void);

/* Input, driven from the keyboard layer while the box is open. `ks` is the SDL
 * keyboard state array; the module keeps its own previous-frame copy so the
 * caller passes only the live state. Handles typing, backspace, Enter (send),
 * Esc (cancel) and the channel-cycle keys. */
void ShNetChat_FeedKeys(const unsigned char* ks);

/* Y / U from the input layer. Open() brings the box up to type; CycleScope()
 * flips GLOBAL<->GAME (and opens the box if it was closed). */
void ShNetChat_Open(void);
void ShNetChat_CycleScope(void);
void ShNetChat_Close(void);
int  ShNetChat_IsOpen(void);      /* 1 while composing (input must be swallowed) */

/* Console `chat show|hide|on|off`: hide suppresses the whole overlay. */
void ShNetChat_SetHidden(int hidden);
int  ShNetChat_Hidden(void);

/* A locally-generated line (a system notice), shown like any chat line. */
void ShNetChat_AddSystem(const char* text);

/* ------------------------------------------------------------------ */
/* Draw-facing (read by sh_net_ui.c, which owns the GL atlas)          */
/* ------------------------------------------------------------------ */

/* 1 when the overlay should draw at all this frame (visible lines, or open). */
int  ShNetChat_DisplayActive(void);

/* Visible log lines, newest last. Alpha is 0..1 for the fade. */
int         ShNetChat_LineCount(void);
const char* ShNetChat_LineText(int i);
int         ShNetChat_LineScope(int i);   /* SHNET_CHAT_* */
float       ShNetChat_LineAlpha(int i);

/* The compose prompt, e.g. "[All]" / "[Game]", and the text being typed. */
int         ShNetChat_Composing(void);
int         ShNetChat_Scope(void);
const char* ShNetChat_ComposeText(void);

#ifdef __cplusplus
}
#endif

#endif /* SH_NET_CHAT_H */
