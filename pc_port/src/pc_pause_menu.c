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
#include "pc_quicksave.h"
#include "pc_rando.h"

extern int g_PsxPresentLastFrame;
extern int g_PsxFreezeCaptureHold;
extern int g_PcFreezeReleasePending;

#define PM_HEADER_Y  64
#define PM_ROW_Y     96
#define PM_ROW_PITCH 20 /* the title's MAINMENU_ROW_PITCH */
#define PM_REASON_Y  204
#define PM_CONFIRM_Y 104
#define PM_ANSWER_DY 24
#define PM_TEXT_H    16

static const char* const s_RowLabels[PM_ROW_COUNT] = { "CONTINUE", "SAVE", "LOAD", "OPTION", "EXIT" };
static const char* const s_AnswerLabels[2]         = { "Yes", "No" };
static const int         s_AnswerCx[2]             = { 120, 200 };

static PmMenu s_menu;
static int    s_open;
static s32    s_blink;
static int    s_returnPending;
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
    s_returnPending        = 0;
    g_PsxFreezeCaptureHold = 0;
    s_open                 = 0;
    s_blink                = 0;
    s_fadeAct              = PM_ACT_NONE;
}

static void PauseMenu_Draw(void)
{
    unsigned    mask = PmMenu_EnabledMask(&s_menu);
    const char* reason;
    int         i;

    /* Yes closed the popup, but it stays on screen to fade out with the frame. */
    if (s_menu.confirm.open || s_fadeAct == PM_ACT_EXIT_YES)
    {
        Pc_PauseMenu_ConfirmDraw("Return_to_the_title_screen?", s_menu.confirm.yes, PM_CONFIRM_Y);
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

    reason = PmBlock_Reason(s_menu.block);
    if (reason != NULL)
    {
        Nm_TextDrawCentered(160, PM_REASON_Y, NM_TEXT_TALL, StringColorId_LightGrey, reason);
    }
}

static void PauseMenu_Leave(PmAction act)
{
    SD_Call(4);
    if (act == PM_ACT_EXIT_YES)
    {
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
        PauseMenu_Draw();
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
                                               PmMenu_EnabledMask(&s_menu), NULL);
    }

    act = PmMenu_Update(&s_menu, &in, hoverRow, hoverAnswer);
    switch (act)
    {
        case PM_ACT_MOVED:
            SD_Call(Sfx_MenuMove);
            break;

        case PM_ACT_EXIT_ASK:
            SD_Call(Sfx_MenuConfirm);
            break;

        case PM_ACT_EXIT_NO:
            SD_Call(Sfx_MenuCancel);
            break;

        case PM_ACT_RESUME:
            SD_Call((in.cancel || in.mouseRight) ? Sfx_MenuCancel : Sfx_MenuConfirm);
            SD_Call(4);
            /* Releasing on this tick showed one bare fog-colored frame: the world
             * draw is still gated off by the BgmStatusFlag_Pause that
             * SysState_GamePaused_Update set this tick. Hand the
             * release to MainLoop, which waits for a tick that drew. */
            g_PcFreezeReleasePending = 1;
            Pc_PauseMenu_Reset();
            SH_DBG("[PAUSE] resume");
            SysWork_StateSetNext(SysState_Gameplay);
            return;

        case PM_ACT_SAVE:
        case PM_ACT_LOAD:
        case PM_ACT_EXIT_YES:
            /* SysState_OptionsMenu fades out before its screen; the save and
             * load screens and the title would cut in, so fade here first. */
            SD_Call(Sfx_MenuConfirm);
            ScreenFade_Start(true, false, false);
            g_ScreenFadeTimestep = Q12(0.0f);
            s_fadeAct            = act;
            PauseMenu_Draw();
            return;

        case PM_ACT_OPTION:
            SD_Call(Sfx_MenuConfirm);
            PauseMenu_Leave(act);
            return;

        default:
            break;
    }

    PauseMenu_Draw();
    Pc_MouseCursor_Show();
}
