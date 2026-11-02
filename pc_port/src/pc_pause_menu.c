/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/screen/screen_draw.h"
#include "bodyprog/screen/screen_fade.h"
#include "bodyprog/sound/sound_system.h"
#include "bodyprog/text/text_draw.h"
#include "sh_log.h"
#include "pc_mouse_cursor.h"
#include "pc_native_menu_draw.h"
#include "pc_pause_menu.h"
#include "pc_pause_menu_core.h"
#include "pc_quick_options.h"
#include "pc_quicksave.h"
#include "pc_rando.h"

extern int g_PsxPresentLastFrame;
extern int g_PsxFreezeCaptureHold;
extern int g_PcFreezeReleasePending;
extern void PsyX_SPUAL_SetKeyOnMasterExempt(int on);

#define PM_HEADER_Y  64
#define PM_ROW_Y     96
#define PM_ROW_PITCH 20 /* the title's MAINMENU_ROW_PITCH */
#define PM_CONFIRM_Y 104
#define PM_ANSWER_DY 24
#define PM_TEXT_H    16

static const char* const s_RowLabels[PM_ROW_COUNT] = { "CONTINUE", "QUICK_MENU", "SAVE", "LOAD", "OPTION", "EXIT" };
static const char* const s_AnswerLabels[2]         = { "YES", "NO" };
static const int         s_AnswerCx[2]             = { 120, 200 };

static PmMenu s_menu;
static int    s_open;
static s32    s_blink;
static int    s_returnPending;
/* QUICK MENU resumes first and opens the panel once gameplay is live again:
 * F10 only ever opens it over the running game, never over the pause. Not
 * cleared by Pc_PauseMenu_Reset, which game_sys_states.c calls on every
 * unpaused tick and so dropped the request before it could open. */
static int    s_quickMenuPending;
/* SAVE, LOAD or EXIT -> Yes chosen; carried out once the fade-out is black. */
static PmAction s_fadeAct;

/* Centred the way MainMenu_MainTextDraw centres the title rows. */
static int MarkedLabelX(int cx, const char* label)
{
    return cx - ((Nm_TextWidth(label) + Nm_TextWidth("[")) / 2);
}

static void MarkedLabelDraw(int cx, int y, int color, const char* label, int sel)
{
    const char* mark = sel ? "[" : "_";
    int         x    = MarkedLabelX(cx, label);

    Nm_TextDraw(x, y, NM_TEXT_TALL, color, mark);
    x += Nm_TextWidth(mark);
    Nm_TextDraw(x, y, NM_TEXT_TALL, color, label);
    if (sel)
    {
        Nm_TextDraw(x + Nm_TextWidth(label), y, NM_TEXT_TALL, color, "]");
    }
}

void Pc_PauseMenu_ConfirmDraw(const char* question, int yes, int y)
{
    int i;

    Nm_TextDrawCentered(160, y, NM_TEXT_TALL, StringColorId_White, question);
    for (i = 0; i < 2; i++)
    {
        MarkedLabelDraw(s_AnswerCx[i], y + PM_ANSWER_DY, StringColorId_White, s_AnswerLabels[i],
                        (i == 0) ? (yes != 0) : (yes == 0));
    }
}

int Pc_PauseMenu_ConfirmHover(const NmInput* in, int y)
{
    int i;

    if (!in->mouseOk || in->mouseY < y + PM_ANSWER_DY || in->mouseY >= y + PM_ANSWER_DY + PM_TEXT_H)
    {
        return -1;
    }

    for (i = 0; i < 2; i++)
    {
        const char* label = s_AnswerLabels[i];
        int         x     = MarkedLabelX(s_AnswerCx[i], label);
        int         w     = Nm_TextWidth("[") + Nm_TextWidth(label) + Nm_TextWidth("]");

        if (in->mouseX >= x && in->mouseX < x + w)
        {
            return i;
        }
    }
    return -1;
}

int Pc_PauseMenu_ReturnPending(void)
{
    return s_returnPending;
}

int Pc_PauseMenu_TakeReturn(void)
{
    int pending = s_returnPending;

    s_returnPending = 0;
    return pending;
}

void Pc_PauseMenu_Reset(void)
{
    /* Away on a screen the pause opened, the pause's SD_Call(3) mute is still
     * held (PauseMenu_Leave); every way off that path that is not back to the
     * pause must lift it, as the pause's own exits do. */
    if (s_returnPending)
    {
        SD_Call(4);
    }
    PsyX_SPUAL_SetKeyOnMasterExempt(0);
    s_returnPending        = 0;
    g_PsxFreezeCaptureHold = 0;
    s_open                 = 0;
    s_blink                = 0;
    s_fadeAct              = PM_ACT_NONE;
}

void Pc_PauseMenu_AwayUpdate(void)
{
    s32 state    = g_GameWork.gameState;
    int onScreen = state == GameState_SaveScreen || state == GameState_LoadSavegameScreen ||
                   state == GameState_OptionScreen;

    if (s_returnPending && !onScreen && state != GameState_InGame)
    {
        Pc_PauseMenu_Reset();
    }

    /* Those screens' own cues key on from this thread, as the pause menu's do;
     * nothing else does while they are up (the world is not updated, and BGM
     * notes key on from the sequencer thread), so arming for the whole screen
     * leaves the game's audio under the mute. */
    PsyX_SPUAL_SetKeyOnMasterExempt(s_returnPending && onScreen);

    if (s_quickMenuPending && state != GameState_InGame)
    {
        s_quickMenuPending = 0;
    }
    if (s_quickMenuPending && g_SysWork.sysState == SysState_Gameplay && !g_PcFreezeReleasePending)
    {
        s_quickMenuPending = 0;
        if (!Pc_QuickOptions_IsOpen())
        {
            Pc_QuickOptions_Toggle();
        }
    }
}

static void PauseMenu_Draw(void)
{
    unsigned mask = PmMenu_EnabledMask(&s_menu);
    int      i;

    if (s_menu.confirm.open)
    {
        Pc_PauseMenu_ConfirmDraw("RETURN_TO_THE_TITLE_SCREEN?", s_menu.confirm.yes, PM_CONFIRM_Y);
        return;
    }

    if (!((s_blink >> 11) & 1))
    {
        Nm_TextDrawCentered(160, PM_HEADER_Y, NM_TEXT_TALL, StringColorId_White, "\x07PAUSED");
    }

    for (i = 0; i < PM_ROW_COUNT; i++)
    {
        MarkedLabelDraw(160, PM_ROW_Y + (i * PM_ROW_PITCH),
                        (mask & (1u << i)) ? StringColorId_White : StringColorId_DarkGrey,
                        s_RowLabels[i], i == s_menu.sel);
    }
}

/* The pause keeps the game's SD_Call(3) master fade, which takes everything to
 * silence; the menu's own cues key on outside the master so they still sound.
 * The voice stays exempt until it is next keyed, so a cue that outlives the
 * menu (CONTINUE's, under the SD_Call(4) fade back up) is not cut or faded. */
static void PauseMenu_Sfx(u16 sfxId)
{
    PsyX_SPUAL_SetKeyOnMasterExempt(1);
    SD_Call(sfxId);
    PsyX_SPUAL_SetKeyOnMasterExempt(0);
}

/* SAVE, LOAD and OPTION keep the pause's mute up on their screens and back in
 * the pause; CONTINUE's SD_Call(4) lifts it, and Pc_PauseMenu_Reset does on
 * any other way out. */
static void PauseMenu_Leave(PmAction act)
{
    if (act == PM_ACT_EXIT_YES)
    {
        SD_Call(4);
        Pc_PauseMenu_Reset();
        g_SysWork.sysFlags |= SysFlag_DoWarmReset;
        SH_DBG("[PAUSE] exit to title");
        return;
    }
    s_returnPending        = 1;
    g_PsxFreezeCaptureHold = 1;
    s_open                 = 0;
    SH_DBG("[PAUSE] -> %s", s_RowLabels[s_menu.sel]);
    if (act == PM_ACT_OPTION)
    {
        SysWork_StateSetNext(SysState_OptionsMenu);
        return;
    }
    g_PsxPresentLastFrame = 0;
    if (act == PM_ACT_SAVE)
    {
        Pc_QuickSave_OpenSaveScreen();
    }
    else
    {
        Pc_QuickSave_OpenLoadScreen();
    }
}

void Pc_PauseMenu_Update(void)
{
    const s_ControllerConfig* cfg = &g_GameWorkPtr->config.controllerConfig;
    NmInput                   in;
    u32                       padClicked;
    int                       hoverRow    = -1;
    int                       hoverAnswer = -1;
    PmAction                  act;

    if (!s_open)
    {
        PmBlock block = PM_BLOCK_NONE;

        if (Pc_Rando_Active())
        {
            block = PM_BLOCK_RANDO;
        }
        else if (Pc_QuickSave_BossActive())
        {
            block = PM_BLOCK_BOSS;
        }
        PmMenu_Open(&s_menu, block);
        s_open = 1;
        SH_DBG("[PAUSE] open (block %d)", (int)block);
    }

    s_blink += g_DeltaTimeRaw;

    if (s_fadeAct != PM_ACT_NONE)
    {
        if (ScreenFade_IsFinished())
        {
            act       = s_fadeAct;
            s_fadeAct = PM_ACT_NONE;
            PauseMenu_Leave(act);
            return;
        }
        return;
    }

    NmInput_FromGame(&in);
    /* A Pause bind that is not also a confirm button closes the menu like
     * Cancel; when it shares Start with `enter` it confirms the row instead. */
    padClicked = g_Controller0->clickedBtnFlags;
    if (in.mouseOk)
    {
        padClicked &= ~Pc_MouseCursor_BoundPadBits();
    }
    if (padClicked & cfg->pause & ~cfg->enter)
    {
        in.cancel = 1;
    }

    if (s_menu.confirm.open)
    {
        hoverAnswer = Pc_PauseMenu_ConfirmHover(&in, PM_CONFIRM_Y);
    }
    else
    {
        hoverRow = Pc_MouseCursor_MenuRowHover(PM_ROW_Y, PM_ROW_PITCH, PM_ROW_COUNT,
                                               (1u << PM_ROW_COUNT) - 1u, NULL);
    }

    act = PmMenu_Update(&s_menu, &in, hoverRow, hoverAnswer);
    switch (act)
    {
        case PM_ACT_MOVED:
            PauseMenu_Sfx(Sfx_MenuMove);
            break;

        case PM_ACT_EXIT_ASK:
            PauseMenu_Sfx(Sfx_MenuConfirm);
            break;

        case PM_ACT_EXIT_NO:
            PauseMenu_Sfx(Sfx_MenuCancel);
            break;

        case PM_ACT_BLOCKED:
            PauseMenu_Sfx(Sfx_MenuCancel);
            SH_DBG_ECHO("Can't %s %s", (s_menu.sel == PM_ROW_LOAD) ? "load" : "save",
                        (s_menu.block == PM_BLOCK_BOSS) ? "during a boss fight" : "in a randomizer run");
            break;

        case PM_ACT_RESUME:
        case PM_ACT_QUICK_MENU:
            PauseMenu_Sfx((in.cancel || in.mouseRight) ? Sfx_MenuCancel : Sfx_MenuConfirm);
            SD_Call(4);
            /* Releasing on this tick showed one bare fog-colored frame: the world
             * draw is still gated off by the BgmStatusFlag_Pause that
             * SysState_GamePaused_Update set this tick. Hand the
             * release to MainLoop, which waits for a tick that drew. */
            g_PcFreezeReleasePending = 1;
            Pc_PauseMenu_Reset();
            s_quickMenuPending = (act == PM_ACT_QUICK_MENU);
            SH_DBG("[PAUSE] resume%s", s_quickMenuPending ? " -> quick menu" : "");
            SysWork_StateSetNext(SysState_Gameplay);
            return;

        case PM_ACT_SAVE:
        case PM_ACT_LOAD:
        case PM_ACT_EXIT_YES:
            /* SysState_OptionsMenu fades out before its screen; the save and
             * load screens and the title would cut in, so fade here first. */
            /* As the game's own exits to these screens (the save point's
             * "Save?" prompt, OPTION): the prompt goes on the confirm and the
             * screen's files load under the fade, which Screen_FadeUpdate
             * paces by the queue length. Left on screen, the white text dimmed
             * evenly under the subtract long after the dark world had gone
             * black, which read as an alpha overlay. The opener loads again
             * after its VRAM clear, as item_screens_2.c's SAVE does. */
            PauseMenu_Sfx(Sfx_MenuConfirm);
            if (act != PM_ACT_EXIT_YES)
            {
                GameFs_SaveLoadBinLoad();
            }
            ScreenFade_Start(true, false, false);
            g_ScreenFadeTimestep = Q12(0.0f);
            s_fadeAct            = act;
            return;

        case PM_ACT_OPTION:
            PauseMenu_Sfx(Sfx_MenuConfirm);
            PauseMenu_Leave(act);
            return;

        default:
            break;
    }

    PauseMenu_Draw();
    Pc_MouseCursor_Show();
}
