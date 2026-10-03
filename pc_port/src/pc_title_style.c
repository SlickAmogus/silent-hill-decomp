/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc_title_style.h"

#include <stdio.h>
#include <stdlib.h>

#include "game.h"
#include "main/fileinfo.h"
#include "main/fsqueue.h"
#include "bodyprog/bodyprog.h"
#include "pc_config.h"
#include "sh_log.h"

/* The three title screens, and why this is not just a file name.
 *
 *   TITLE_STYLE_US   a full 320x480 picture: background, logo and all, baked.
 *   TITLE_STYLE_PAL  no picture -- a 4bpp 320x96 logo block the game composes
 *                    a title from (black, logo, fog), via Pc_TitleLogoDrawEur.
 *   TITLE_STYLE_JP   a full picture again, black with the stylized logo.
 *
 * No disc carries all three. TIM/TITLE.TIM (the Japanese one) is byte-
 * identical on every disc, so that style is free anywhere; the other two are
 * each on some discs and not others, and the PAL disc even calls its logo
 * block TITLE_E -- the same name the US picture has elsewhere. So the port
 * ships the two images a disc can lack under gamedata/title/ and uploads one
 * of those when the chosen style is not the mounted disc's own. */

static int StyleOfRegion(void)
{
    if (g_GameRegion == Region_JPN)
        return TITLE_STYLE_JP;
    return (g_GameRegion == Region_EUR) ? TITLE_STYLE_PAL : TITLE_STYLE_US;
}

int Pc_TitleStyle(void)
{
    int want = g_PcConfig.menuStyle;

    if (want < TITLE_STYLE_US || want > TITLE_STYLE_JP)
        return StyleOfRegion(); /* auto */
    return want;
}

int Pc_TitleUsesJpArt(void)
{
    return Pc_TitleStyle() == TITLE_STYLE_JP;
}

int Pc_TitleUsesPalLogo(void)
{
    return Pc_TitleStyle() == TITLE_STYLE_PAL;
}

void Pc_TitleStyleApplyDesc(void)
{
    if (Pc_TitleUsesPalLogo())
    {
        /* The logo block is 4bpp and lands tpage-aligned at (896,0), with its
         * 16-entry CLUT at the US title-CLUT home. */
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

/* Upload a TIM sitting in memory to the home g_TitleImg names: the same two
 * LoadImage calls the queue's post-load makes, without the queue. */
static int UploadTim(void* data)
{
    TIM_IMAGE tim;
    RECT      rect;

    OpenTIM((u_long*)data);
    if (ReadTIM(&tim) == NULL)
        return 0;

    rect   = *tim.prect;
    rect.x = g_TitleImg.u + ((g_TitleImg.tPage[1] & 0xF) << 6);
    rect.y = g_TitleImg.v + ((g_TitleImg.tPage[1] << 4) & 0x100);
    LoadImage(&rect, (u_long*)tim.paddr);

    if (tim.caddr != NULL)
    {
        rect   = *tim.crect;
        rect.x = g_TitleImg.clutX;
        rect.y = g_TitleImg.clutY;
        LoadImage(&rect, (u_long*)tim.caddr);
    }
    DrawSync(0);
    return 1;
}

static int UploadTitleFile(const char* path)
{
    FILE* f = fopen(path, "rb");
    long  size;
    void* buf;
    int   ok;

    if (f == NULL)
    {
        SH_WARN("[TITLE] %s is missing - keeping the disc's own title art", path);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > (2 * 1024 * 1024))
    {
        fclose(f);
        return 0;
    }
    buf = malloc((size_t)size);
    if (buf == NULL || fread(buf, 1, (size_t)size, f) != (size_t)size)
    {
        free(buf);
        fclose(f);
        return 0;
    }
    fclose(f);
    ok = UploadTim(buf);
    free(buf);
    return ok;
}

void Pc_TitleArtLoad(void)
{
    int style = Pc_TitleStyle();

    Pc_TitleStyleApplyDesc();

    /* TITLE.TIM is on every disc, so this style never needs a shipped file. */
    if (style == TITLE_STYLE_JP)
    {
        Fs_QueueStartReadTim(FILE_TIM_TITLE_TIM, FS_BUFFER_3, &g_TitleImg);
        return;
    }

    /* The other two: a disc has whichever is its own, and only that one -- on
     * a PAL disc TITLE_E IS the logo block, not the US picture. */
    if (style == StyleOfRegion())
    {
        Fs_QueueStartReadTim(FILE_TIM_TITLE_E_TIM, FS_BUFFER_3, &g_TitleImg);
        return;
    }
    if (style == TITLE_STYLE_US && g_GameRegion == Region_JPN)
    {
        /* The Japanese disc carries the US picture under that name too. */
        Fs_QueueStartReadTim(FILE_TIM_TITLE_E_TIM, FS_BUFFER_3, &g_TitleImg);
        return;
    }

    if (!UploadTitleFile((style == TITLE_STYLE_PAL) ? "gamedata/title/pal.tim"
                                                    : "gamedata/title/us.tim"))
    {
        Fs_QueueStartReadTim(FILE_TIM_TITLE_E_TIM, FS_BUFFER_3, &g_TitleImg);
    }
}

int Pc_TitleStyleApply(void)
{
    int style;

    /* The title picture lives in one image slot, so switching is a re-read of
     * another image into it. Only at the title: anywhere else that slot holds
     * something the current screen is drawing from. */
    if (g_GameWork.gameState != GameState_MainMenu)
        return 0;

    Pc_TitleArtLoad();
    style = Pc_TitleStyle();
    SH_LOG("[TITLE] style = %s", style == TITLE_STYLE_JP  ? "japanese"
                               : style == TITLE_STYLE_PAL ? "pal" : "us");
    return 1;
}
