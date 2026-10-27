/*
 * split_xbox.h - two players on one Xbox: split screen.
 *
 * One game, two players. Player 1 is the game's own player in g_SysWork;
 * Player 2 is a second player context kept beside it and swapped into the same
 * g_SysWork slots whenever the stock player, camera or event code has to run for
 * Player 2, so both play by exactly the same rules. The world is drawn twice a
 * frame, once from each player's camera, into two parts of the screen; menus,
 * messages and cutscenes show one full screen.
 *
 *   split_xbox.c        the session, Player 2's context and its per-frame pass
 *   split_render_xbox.c the second view: its ordering table, layouts, present
 *   split_npc_xbox.c    monsters choosing and attacking either player
 *   split_audio_xbox.c  world sound from the nearer player
 *   split_menu_xbox.c   the title screen's Multiplayer menu
 *
 * Every hook in shared code is a no-op unless a split session is running, so a
 * single-player game behaves exactly as it did before split screen existed.
 */
#ifndef SPLIT_XBOX_H
#define SPLIT_XBOX_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- settings ---- */
enum
{
    SPLIT_LAYOUT_TOPBOTTOM        = 0, /* full width, each view the middle band */
    SPLIT_LAYOUT_SIDEBYSIDE       = 1, /* full height, each view the middle band */
    SPLIT_LAYOUT_TOPBOTTOM_WHOLE  = 2, /* each view the whole picture, smaller */
    SPLIT_LAYOUT_SIDEBYSIDE_WHOLE = 3,
    SPLIT_LAYOUT_COUNT
};

enum
{
    SPLIT_START_NEW      = 1,
    SPLIT_START_CONTINUE = 2,
    SPLIT_START_LOAD     = 3
};

/* ---- the Multiplayer menu (split_menu_xbox.c) ---- */
void SplitMenu_Open(void);
void SplitMenu_OpenAt(int preferContinue);
void SplitMenu_Close(void);
int  SplitMenu_IsOpen(void);
void SplitMenu_SetAvailable(int canContinue, int canLoad);
void SplitMenu_Update(int cancel, int up, int down, int left, int right, int confirm);
void SplitMenu_Render(void);
int  Split_P2CharIndex(void);       /* Player 2's character, a play-as index */
void Split_P2CharCycle(int dir);
int  Split_P2PortPick(void);        /* the pad port Player 2 would get, 0 = none */

/* ---- the session (split_xbox.c) ---- */
extern int g_XbSplitActive;   /* a split-screen game is under way (both players exist) */
/* Two views on screen this frame: gameplay, outside menus, messages and scenes.
 * Everything that only matters for two views keys off this. */
extern int g_XbSplitTwoViews;

/* Title screen (title.c). */
int  Split_TitleBusy(void);                          /* menu open, or a split game starting */
void Split_TitleFrame(int canContinue, int canLoad); /* every main-menu frame */
int  Split_TitleTakeBootEntry(void);                 /* stock entry to run for a split start, or -1 */
void Split_MenuStart(int kind, int p2Port);          /* the menu chose New Game / Continue / Load */
void Split_MenuClosed(void);                         /* Back out of the menu */

/* Game flow. */
void Split_OnBootStep0(int processFlags);  /* GameBoot_GameStartup case 0 (game_load.c) */
int  Split_ExteriorChunkSlots(int count, int interior); /* world_draw.c: the slot count for this load */
void Split_OnInGameEnter(void);            /* GameState_InGame_Update case 1 */
void Split_OnGameOverExit(void);           /* just before a game over's Game_WarmBoot */
void Split_OnWarmBoot(void);               /* Game_WarmBoot, before SysWork_Clear */

/* Per frame. */
void Split_FrameInputBegin(void);          /* game_main.c: before Joy_ReadP1 */
void Split_FrameInputEnd(void);            /* game_main.c: after Joy_ControllerDataUpdate */
void Split_FrameBegin(void);               /* game_main.c: after GsClearOt */
void Split_AfterDispatch(void);            /* game_sys_states.c: after the sysState dispatch */
void Split_SlotCameraDone(void);           /* ...after the slot player's camera */
void Split_SidePass(void);                 /* ...after the slot player's update and draw */
void Split_View2World(void);               /* ...after Gfx_InGameDraw (split_render_xbox.c) */
int  Split_PacedVSync(int* vblanks, int* prevVblanks); /* game_main.c pacing: 1 = done */

/* Deaths (player_control.c). */
int  Split_DeathHold(void);                /* 1 = hold instead of the game over */
int  Split_DeathHolding(void);

#ifdef __cplusplus
}
#endif

#endif /* SPLIT_XBOX_H */
