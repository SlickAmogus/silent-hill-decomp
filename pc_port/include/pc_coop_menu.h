/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_coop_menu.h - the simple co-op front end (host / join).
 *
 * An overlay opened from the main menu's Multiplayer row. Like the achievement
 * browser it does NOT change g_GameWork.gameState: it draws on top of the title
 * screen and owns the pad while it is up. Host / Join drive the Steam session
 * layer (sh_net_session.h); nothing here touches the master-server living world.
 */
#ifndef PC_COOP_MENU_H
#define PC_COOP_MENU_H

#ifdef __cplusplus
extern "C" {
#endif

/* 1 while the current game was launched as multiplayer (set at the co-op boot,
 * cleared at the title). The runtime gate for the in-game co-op features; a
 * normal single-player game leaves it 0. */
extern int g_PcCoopGame;

void Pc_CoopMenu_Open(void);        /* main-menu popup (Host / Join) */
void Pc_CoopMenu_OpenInGame(void);  /* in-game M menu (Resume / players / leave) */
void Pc_CoopMenu_Close(void);
int  Pc_CoopMenu_IsOpen(void);
int  Pc_CoopMenu_InGame(void);      /* 1 while the in-game M menu is up */

/* One frame of input, edges already derived by the caller. */
void Pc_CoopMenu_Update(int cancel, int up, int down, int confirm);

/* Text entry (Join by IP / room code). The overlay feeds the keyboard state to
 * FeedKeys every frame while Editing() is true; the field commits on Enter. */
int  Pc_CoopMenu_Editing(void);
void Pc_CoopMenu_FeedKeys(const unsigned char* ks);

/* Accessors for the renderer (sh_net_ui.c's Nu_DrawCoopMenu). The menu is drawn
 * with the online UI's clean panel so it matches the quick menu / achievements
 * popup, on the title screen and in game alike. */
int         Pc_CoopMenu_RowCount(void);
int         Pc_CoopMenu_Selected(void);
const char* Pc_CoopMenu_Title(void);
void        Pc_CoopMenu_RowText(int i, char* out, int cap);
void        Pc_CoopMenu_StatusText(char* out, int cap);

/* Mouse: the renderer computes which row is under the pointer (with the exact
 * geometry it draws the rows with, so they can never disagree) and hands that
 * row INDEX over here, -1 for none. Pc_CoopMenu_Update confirms it on the click,
 * edge-latched, once per press -- the render pass never confirms. */
void        Pc_CoopMenu_SetHover(int row);
void        Pc_CoopMenu_SetSelected(int i);
void        Pc_CoopMenu_Confirm(void);

/* The host's "Start Game" queues the boot here; title.c polls it each main-menu
 * frame and performs the actual New Game / load boot. Returns 0 none, 1 new
 * game, 2 load (save name written to outName), and clears the request. */
int         Pc_CoopMenu_TakeStartRequest(char* outName, int cap);

#ifdef __cplusplus
}
#endif

#endif /* PC_COOP_MENU_H */
