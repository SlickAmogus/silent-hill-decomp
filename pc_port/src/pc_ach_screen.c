/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL.h>

#include "bodyprog/sound/sfx_id_enum.h"
#include "bodyprog/sound/sound_system.h"
#include "bodyprog/text/text_draw.h"
#include "hires_override.h"
#include "lang_text.h"
#include "pc_ach_model.h"
#include "pc_ach_screen.h"
#include "pc_native_menu_draw.h"
#include "pc_retroachievements.h"
#include "sh_log.h"

#define ACH_VISIBLE        5
#define ACH_HEADING_Y      12
#define ACH_ARROW_UP_Y     40
#define ACH_LIST_Y         48
#define ACH_PITCH          29
#define ACH_ARROW_DOWN_Y   196
#define ACH_BADGE_X        8
#define ACH_BADGE_SIZE     26
#define ACH_TEXT_X         40
#define ACH_TEXT_W         272
#define ACH_DESC_DY        16
#define ACH_UNDERLINE_DY   25
#define ACH_FOOTER_Y       204
#define ACH_BACK_X         16
#define ACH_REFRESH_FRAMES 60

#define ACH_SV_BADGE_Y     40
#define ACH_SV_BADGE_SIZE  32
#define ACH_SV_TITLE_Y     80
#define ACH_SV_RULE_X0     30
#define ACH_SV_RULE_X1     290
#define ACH_SV_RULE1_Y     97
#define ACH_SV_DATE_Y      100
#define ACH_SV_POINTS_Y    118
#define ACH_SV_RULE2_Y     128
#define ACH_SV_DESC_Y      131
#define ACH_SV_DESC_PITCH  9
#define ACH_SV_DESC_LINES  8
#define ACH_SV_DESC_W      260
/* Wrap depth for a description; past ACH_SV_DESC_LINES the rest pans on the last line. */
#define ACH_SV_DESC_WRAP   32

/* Nm_SpriteDraw spans UV 0..255, so the slot's UV denominator is 256 whatever
 * size the PNG decodes to. */
#define ACH_BADGE_NATIVE   256
#define ACH_RGB_LIT        128
#define ACH_RGB_LOCKED     80

#define ACH_HIT_NONE (-1)
#define ACH_HIT_BACK (-2)

typedef struct
{
    char           badge[16];
    unsigned char* png;
    size_t         len;
} AchPng;

static int           s_open;
static int           s_single;
static int           s_onBack;
static int           s_refresh;
static int           s_escPrev;
static AchList       s_list;
static PcRaAch       s_snap[ACH_MAX];
static NmScroll      s_scroll;
static AchBadgeCache s_badges;
/* Pc_Ra_RequestBadge fetches a name once per session, so a badge evicted from
 * the 8 pool slots could never be downloaded again; its bytes are kept here. */
static AchPng        s_png[ACH_MAX];
static int           s_pngCount;

static const AchPng* ach_png_find(const char* badge)
{
    int i;

    for (i = 0; i < s_pngCount; i++)
    {
        if (strcmp(s_png[i].badge, badge) == 0)
        {
            return &s_png[i];
        }
    }
    return NULL;
}

void Pc_AchScreen_ProvideBadge(const char* badge, const unsigned char* png, size_t len)
{
    AchPng* e;

    if (badge == NULL || !badge[0] || png == NULL || len == 0 ||
        s_pngCount >= ACH_MAX || ach_png_find(badge) != NULL)
    {
        return;
    }

    e      = &s_png[s_pngCount];
    e->png = (unsigned char*)malloc(len);
    if (e->png == NULL)
    {
        return;
    }
    memcpy(e->png, png, len);
    e->len = len;
    memset(e->badge, 0, sizeof(e->badge));
    strncpy(e->badge, badge, sizeof(e->badge) - 1);
    s_pngCount++;
}

static u16 ach_clut(int slot, int row)
{
    int id = ACH_BADGE_SLOT_FIRST + slot;

    return (u16)(((HIRES_POOL_CLUT_ROW_BASE + ((id / 64) * HIRES_POOL_MAX_ROWS) + row) << 6) | (id % 64));
}

static void ach_badge_ensure(const char* badge)
{
    const AchPng*  p;
    unsigned char* rgba = NULL;
    int            w    = 0;
    int            h    = 0;
    int            ok   = 0;
    int            slot;
    int            id;

    if (!badge[0] || AchBadgeCache_Find(&s_badges, badge) >= 0)
    {
        return;
    }

    p = ach_png_find(badge);
    if (p == NULL)
    {
        Pc_Ra_RequestBadge(badge);
        return;
    }

    slot = AchBadgeCache_Claim(&s_badges, badge);
    id   = ACH_BADGE_SLOT_FIRST + slot;
    if (HiresOverride_DecodeToRGBA(p->png, (unsigned int)p->len, &rgba, &w, &h) == 0 &&
        rgba != NULL && w > 0 && h > 0)
    {
        ok = HiresOverride_PoolSlotRegisterRGBA(id, 0, rgba, w, h, ACH_BADGE_NATIVE, ACH_BADGE_NATIVE) == 0;
        if (ok)
        {
            int i;

            for (i = 0; i < w * h; i++)
            {
                unsigned char* px   = &rgba[i * 4];
                unsigned char  luma = (unsigned char)(((px[0] * 77) + (px[1] * 150) + (px[2] * 29)) >> 8);

                px[0] = luma;
                px[1] = luma;
                px[2] = luma;
            }
            ok = HiresOverride_PoolSlotRegisterRGBA(id, 1, rgba, w, h, ACH_BADGE_NATIVE, ACH_BADGE_NATIVE) == 0;
        }
    }
    free(rgba);
    s_badges.ready[slot] = ok;
    if (!ok)
    {
        SH_DBG("[ACHSCREEN] badge %s unusable (decode or upload failed)", badge);
    }
}

static void ach_badge_draw(const char* badge, int x, int y, int size, int locked)
{
    int slot = AchBadgeCache_Find(&s_badges, badge);

    if (slot < 0 || !s_badges.ready[slot])
    {
        return;
    }
    Nm_SpriteDraw(x, y, size, size, ach_clut(slot, locked ? 1 : 0), locked ? ACH_RGB_LOCKED : ACH_RGB_LIT);
}

static void ach_rebuild(void)
{
    char badge[16];
    char title[128];
    int  had = s_list.count > 0;
    int  idx = -1;
    int  n;

    if (had)
    {
        memcpy(badge, s_list.items[s_scroll.sel].badge, sizeof(badge));
        memcpy(title, s_list.items[s_scroll.sel].title, sizeof(title));
    }

    n = Pc_Ra_SnapshotAchievements(s_snap, ACH_MAX);
    AchList_Build(&s_list, s_snap, n);
    NmScroll_SetCount(&s_scroll, s_list.count);
    if (had)
    {
        idx = AchList_IndexOf(&s_list, badge, title);
    }
    if (idx >= 0)
    {
        NmScroll_Select(&s_scroll, idx);
    }
    if (s_list.count == 0)
    {
        s_onBack = 1;
        s_single = 0;
    }
}

void Pc_AchScreen_Open(void)
{
    s_list.count = 0;
    NmScroll_Init(&s_scroll, 0, ACH_VISIBLE);
    ach_rebuild();
    s_onBack  = s_list.count == 0;
    s_single  = 0;
    s_refresh = 0;
    s_escPrev = 1;
    s_open    = 1;
    SH_DBG("[ACHSCREEN] opened with %d achievements", s_list.count);
}

int Pc_AchScreen_IsOpen(void)
{
    return s_open;
}

void Pc_AchScreen_Close(void)
{
    s_open = 0;
}

static int ach_back_right(void)
{
    return ACH_BACK_X + NM_BULLET_TEXT_DX + Nm_TextWidth("Back");
}

static int ach_hit(const NmInput* in)
{
    int i;

    if (!in->mouseOk)
    {
        return ACH_HIT_NONE;
    }

    for (i = 0; i < ACH_VISIBLE && s_scroll.top + i < s_list.count; i++)
    {
        int y = ACH_LIST_Y + (i * ACH_PITCH) - 2;

        if (in->mouseX >= ACH_BADGE_X && in->mouseX < 320 - ACH_BADGE_X &&
            in->mouseY >= y && in->mouseY < y + ACH_PITCH)
        {
            return s_scroll.top + i;
        }
    }

    if (in->mouseX >= ACH_BACK_X - 1 && in->mouseX < ach_back_right() + 4 &&
        in->mouseY >= ACH_FOOTER_Y && in->mouseY < ACH_FOOTER_Y + 16)
    {
        return ACH_HIT_BACK;
    }
    return ACH_HIT_NONE;
}

static void ach_select(int hit)
{
    if (hit == ACH_HIT_BACK)
    {
        s_onBack = 1;
    }
    else if (hit >= 0)
    {
        s_onBack = 0;
        NmScroll_Select(&s_scroll, hit);
    }
}

/* 0 when the activation closes the screen. */
static int ach_activate(void)
{
    if (s_onBack)
    {
        SD_Call(Sfx_MenuCancel);
        return 0;
    }
    SD_Call(Sfx_MenuConfirm);
    s_single = 1;
    return 1;
}

/* Raw Escape backs out like the Controls screen: Escape is the default Exit
 * Game bind, which is suppressed while this screen is open. */
static int ach_escape_pressed(void)
{
    const Uint8* ks   = SDL_GetKeyboardState(NULL);
    int          held = ks != NULL && ks[SDL_SCANCODE_ESCAPE];
    int          edge = held && !s_escPrev;

    s_escPrev = held;
    return edge;
}

static int ach_update_list(const NmInput* in, int back)
{
    int oldSel  = s_scroll.sel;
    int oldBack = s_onBack;
    int count   = s_list.count;
    int hit;

    if (back || in->cancel || in->mouseRight)
    {
        SD_Call(Sfx_MenuCancel);
        return 0;
    }

    if (in->wheel != 0 && count > 0)
    {
        int oldTop = s_scroll.top;

        NmScroll_Wheel(&s_scroll, in->wheel);
        if (s_scroll.top != oldTop)
        {
            s_onBack = 0;
        }
    }

    hit = ach_hit(in);
    if (in->mouseLeft && hit != ACH_HIT_NONE)
    {
        Nm_HighlightSnap();
        ach_select(hit);
        return ach_activate();
    }
    if (in->mouseMoved && hit != ACH_HIT_NONE)
    {
        Nm_HighlightSnap();
        ach_select(hit);
    }

    if (in->up && s_onBack && count > 0)
    {
        s_onBack = 0;
    }
    else if (in->up)
    {
        NmScroll_Move(&s_scroll, -1, 0);
    }
    else if (in->down && !s_onBack)
    {
        if (s_scroll.sel >= count - 1)
        {
            s_onBack = 1;
        }
        else
        {
            NmScroll_Move(&s_scroll, +1, 0);
        }
    }

    if (s_scroll.sel != oldSel || s_onBack != oldBack)
    {
        SD_Call(Sfx_MenuMove);
    }

    if (in->confirm)
    {
        return ach_activate();
    }
    return 1;
}

static void ach_update_single(const NmInput* in, int back)
{
    if (back || in->cancel || in->confirm || in->mouseRight || in->mouseLeft)
    {
        SD_Call(Sfx_MenuCancel);
        s_single = 0;
    }
}

static void ach_request_visible(void)
{
    int i;

    if (s_single)
    {
        ach_badge_ensure(s_list.items[s_scroll.sel].badge);
        return;
    }
    for (i = s_scroll.top; i < s_list.count && i < s_scroll.top + ACH_VISIBLE; i++)
    {
        ach_badge_ensure(s_list.items[i].badge);
    }
}

int Pc_AchScreen_Update(const NmInput* in)
{
    int back;

    if (!s_open)
    {
        return 0;
    }
    back = ach_escape_pressed();

    if (++s_refresh >= ACH_REFRESH_FRAMES)
    {
        s_refresh = 0;
        ach_rebuild();
    }

    if (s_single)
    {
        ach_update_single(in, back);
    }
    else if (!ach_update_list(in, back))
    {
        /* Stays open so the host can keep drawing it through its fade out. */
        return 0;
    }

    ach_request_visible();
    return 1;
}

static void ach_back_draw(int selected)
{
    int x = ACH_BACK_X + NM_BULLET_TEXT_DX;

    Nm_BulletDraw(ACH_BACK_X, ACH_FOOTER_Y, selected);
    Nm_TextDraw(x, ACH_FOOTER_Y, NM_TEXT_TALL, StringColorId_White, "Back");
    if (selected)
    {
        Nm_HighlightDraw(ACH_BACK_X - 1, ach_back_right() + 4, ACH_FOOTER_Y + 14);
    }
}

static void ach_counter_draw(void)
{
    char count[32];
    char points[96];
    int  wc;
    int  wp;
    int  x;

    snprintf(count, sizeof(count), "%d/%d_", s_list.unlocked, s_list.count);
    snprintf(points, sizeof(points), "(%u/%u_%s)", s_list.pts, s_list.ptsTotal, Pc_LangMenuText("points"));
    wc = Pc_LangMenuTextWidth(count);
    wp = Pc_LangMenuTextWidth(points);
    x  = 320 - ACH_BACK_X - (wc + wp);
    if (x < ach_back_right() + 12)
    {
        x = ach_back_right() + 12;
    }
    Nm_TextDraw(x, ACH_FOOTER_Y, NM_TEXT_TALL, StringColorId_White, count);
    Nm_TextDraw(x + wc, ACH_FOOTER_Y, NM_TEXT_TALL, StringColorId_Gold, points);
}

static void ach_list_draw(void)
{
    char clean[512];
    int  i;

    for (i = 0; i < ACH_VISIBLE && s_scroll.top + i < s_list.count; i++)
    {
        int            idx   = s_scroll.top + i;
        const PcRaAch* a     = &s_list.items[idx];
        int            y     = ACH_LIST_Y + (i * ACH_PITCH);
        int            sel   = !s_onBack && idx == s_scroll.sel;
        int            color = a->unlocked ? StringColorId_White : StringColorId_DarkGrey;
        int            w;

        ach_badge_draw(a->badge, ACH_BADGE_X, y - 1, ACH_BADGE_SIZE, !a->unlocked);

        Nm_SanitizeGlyphs(a->title, clean, sizeof(clean), 1);
        w = Nm_TextDrawMarquee(ACH_TEXT_X, y, NM_TEXT_TALL, sel ? StringColorId_Gold : color, clean, ACH_TEXT_W, sel);
        if (sel)
        {
            Nm_HighlightDraw(ACH_BADGE_X - 1, ACH_TEXT_X + w + 4, y + ACH_UNDERLINE_DY);
        }

        Nm_SanitizeGlyphs(a->desc, clean, sizeof(clean), 0);
        Nm_TextDrawMarquee(ACH_TEXT_X, y + ACH_DESC_DY, NM_TEXT_SMALL, color, clean, ACH_TEXT_W, sel);
    }

    if (s_list.count > ACH_VISIBLE)
    {
        Nm_ScrollArrowDraw(160, ACH_ARROW_UP_Y, 1, NmScroll_CanUp(&s_scroll));
        Nm_ScrollArrowDraw(160, ACH_ARROW_DOWN_Y, 0, NmScroll_CanDown(&s_scroll));
    }

    ach_back_draw(s_onBack);
    ach_counter_draw();
}

static void ach_single_draw(void)
{
    const PcRaAch* a = &s_list.items[s_scroll.sel];
    char           clean[512];
    char           rest[512];
    char           lines[ACH_SV_DESC_WRAP][NM_LINE_MAX];
    char           text[64];
    const char*    word;
    int            w;
    int            x;
    int            n;
    int            i;

    ach_badge_draw(a->badge, 160 - (ACH_SV_BADGE_SIZE / 2), ACH_SV_BADGE_Y, ACH_SV_BADGE_SIZE, !a->unlocked);

    Nm_SanitizeGlyphs(a->title, clean, sizeof(clean), 1);
    w = Pc_LangMenuTextWidth(clean);
    if (w > ACH_SV_RULE_X1 - ACH_SV_RULE_X0)
        w = ACH_SV_RULE_X1 - ACH_SV_RULE_X0;
    Nm_TextDrawMarquee(160 - (w / 2), ACH_SV_TITLE_Y, NM_TEXT_TALL, StringColorId_Gold, clean, w, 1);
    Nm_RuleDraw(ACH_SV_RULE_X0, ACH_SV_RULE_X1, ACH_SV_RULE1_Y, 1);

    if (a->unlocked)
    {
        time_t     t  = (time_t)a->unlockTime;
        struct tm* tm = (a->unlockTime > 0) ? localtime(&t) : NULL;

        if (tm != NULL)
        {
            char date[32];

            Nm_FormatDate(tm->tm_mday, tm->tm_mon + 1, tm->tm_year + 1900, tm->tm_hour, tm->tm_min,
                          date, sizeof(date));
            snprintf(text, sizeof(text), ":_%s", date);
            w = Nm_TextWidth("UNLOCKED");
            x = 160 - ((w + Pc_LangMenuTextWidth(text)) / 2);
            Nm_TextDraw(x, ACH_SV_DATE_Y, NM_TEXT_TALL, StringColorId_White, "UNLOCKED");
            Nm_TextDraw(x + w, ACH_SV_DATE_Y, NM_TEXT_TALL, StringColorId_White, text);
        }
        else
        {
            Nm_TextDrawCentered(160, ACH_SV_DATE_Y, NM_TEXT_TALL, StringColorId_White, "UNLOCKED");
        }
    }
    else
    {
        Nm_TextDrawCentered(160, ACH_SV_DATE_Y, NM_TEXT_TALL, StringColorId_White, "LOCKED");
    }

    snprintf(text, sizeof(text), "%u_", a->points);
    word = (a->points == 1) ? "Point" : "Points";
    w    = Pc_LangMenuTextWidth(text);
    x    = 160 - ((w + Nm_TextWidth(word)) / 2);
    Nm_TextDraw(x, ACH_SV_POINTS_Y, NM_TEXT_SMALL, StringColorId_Gold, text);
    Nm_TextDraw(x + w, ACH_SV_POINTS_Y, NM_TEXT_SMALL, StringColorId_Gold, word);
    Nm_RuleDraw(ACH_SV_RULE_X0, ACH_SV_RULE_X1, ACH_SV_RULE2_Y, 1);

    Nm_SanitizeGlyphs(a->desc, clean, sizeof(clean), 0);
    n = Nm_WrapLines(clean, lines, ACH_SV_DESC_WRAP, ACH_SV_DESC_W, Pc_LangMenuTextWidth);
    for (i = 0; i < n && i < ACH_SV_DESC_LINES - 1; i++)
    {
        Nm_TextDrawVerbatim(ACH_SV_RULE_X0, ACH_SV_DESC_Y + (i * ACH_SV_DESC_PITCH), NM_TEXT_SMALL,
                            StringColorId_White, lines[i]);
    }
    if (i < n)
    {
        snprintf(rest, sizeof(rest), "%s", lines[i]);
        while (++i < n)
        {
            size_t len = strlen(rest);
            snprintf(rest + len, sizeof(rest) - len, "_%s", lines[i]);
        }
        Nm_TextDrawMarquee(ACH_SV_RULE_X0, ACH_SV_DESC_Y + ((ACH_SV_DESC_LINES - 1) * ACH_SV_DESC_PITCH),
                           NM_TEXT_SMALL, StringColorId_White, rest, ACH_SV_DESC_W, 1);
    }

    ach_back_draw(1);
}

void Pc_AchScreen_Draw(void)
{
    if (!s_open)
    {
        return;
    }

    Nm_BlackBackgroundDraw();
    Nm_TextDrawCentered(160, ACH_HEADING_Y, NM_TEXT_TALL, StringColorId_White, "ACHIEVEMENTS");

    if (s_single && s_list.count > 0)
    {
        ach_single_draw();
    }
    else
    {
        ach_list_draw();
    }
}
