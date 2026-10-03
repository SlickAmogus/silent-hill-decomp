/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc_title_style.h"

#include "game.h"
#include "main/fileinfo.h"
#include "main/fsqueue.h"
#include "bodyprog/bodyprog.h"
#include "pc_config.h"
#include "sh_log.h"

int Pc_TitleUsesJpArt(void)
{
    if (g_PcConfig.menuStyle == MENU_STYLE_JAPANESE)
        return 1;
    if (g_PcConfig.menuStyle == MENU_STYLE_WESTERN)
        return 0;
    return g_GameRegion == Region_JPN; /* auto: whatever the disc is */
}

/* The title picture's VRAM descriptor. PAL's is a different shape (a 4bpp
 * logo block it composes from) than the full 8bpp picture the US and Japanese
 * art are, so the desc follows the STYLE, not the disc. */
void Pc_TitleStyleApplyDesc(void)
{
    if (g_GameRegion == Region_EUR && !Pc_TitleUsesJpArt())
    {
        g_TitleImg.tPage[0] = 0;
        g_TitleImg.tPage[1] = 14;
        g_TitleImg.u        = 0;
        g_TitleImg.v        = 0;
        g_TitleImg.clutX    = 224;
        g_TitleImg.clutY    = 15;
        return;
    }

    /* screen_data.c's compiled values, which both full pictures want. */
    g_TitleImg.tPage[0] = 1;
    g_TitleImg.tPage[1] = 13;
    g_TitleImg.u        = 32;
    g_TitleImg.v        = 0;
    g_TitleImg.clutX    = 224;
    g_TitleImg.clutY    = 15;
}

int Pc_TitleStyleApply(void)
{
    /* The title picture lives in one image slot, so switching is a re-read of
     * the other file into it. Only at the title: anywhere else that slot holds
     * something the current screen is drawing from. */
    if (g_GameWork.gameState != GameState_MainMenu)
        return 0;

    Pc_TitleStyleApplyDesc();
    GameFs_TitleGfxLoad();
    SH_LOG("[TITLE] style = %s", Pc_TitleUsesJpArt() ? "japanese" : "western");
    return 1;
}
