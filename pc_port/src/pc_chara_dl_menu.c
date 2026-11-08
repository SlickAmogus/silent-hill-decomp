/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional Characters screens; see pc_chara_dl_menu.h. Drawn with the native
 * menu primitives in Character Select's frame: the list on the left, the
 * preview and the selected character's details on the right. */
#include "game.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "bodyprog/bodyprog.h"
#include "bodyprog/sound/sfx_id_enum.h"
#include "bodyprog/sound/sound_system.h"
#include "bodyprog/text/text_draw.h"
#include "pc_chara_dl.h"
#include "pc_chara_dl_menu.h"
#include "pc_chara_text.h"
#include "lang_quick.h"
#include "pc_chara_preview.h"
#include "pc_mouse_cursor.h"
#include "pc_native_menu_draw.h"
#include "pc_playas.h"
#include "bodyprog/chara/chara.h"
#include "sh_log.h"

/* Same vertical grid as Character Select. */
#define M_HEADING_Y  12
#define M_RULE_Y     30
#define M_LIST_Y     40
#define M_PITCH      16
#define M_VISIBLE    9
#define M_ARROW_UP_Y 34
#define M_ARROW_DN_Y 188
#define M_PREV_Y0    34
#define M_PREV_Y1    196
#define M_FOOTER_Y   204
#define M_OT_LINE    6
#define M_BOX        9   /* checkbox side */

static int s_fl, s_fr;
#define M_W        (s_fr - s_fl)
#define M_LIST_X0  (s_fl + 8)
#define M_DIV_X    (s_fl + (M_W * 55) / 100)
#define M_LIST_X1  (M_DIV_X - 6)
#define M_PREV_X0  (M_DIV_X + 6)
#define M_PREV_X1  (s_fr - 8)
#define M_BACK_X   (s_fl + 16)
/* The right panel: preview above, button and details below. */
#define M_BTN_Y    150
#define M_INFO_Y0  166
#define M_INFO_Y1  182

enum { P_HUB, P_DOWN, P_LOCAL, P_PICK };
enum { R_HEADER, R_ITEM, R_STOCK, R_PLAYAS };
#define M_CAST_GROUP (-2)  /* the picker's built-in Play As cast, Harry only */

#define M_MAX_ROWS 2048
#define M_MAX_CATS 256

typedef struct
{
    int type;
    int cat;   /* category / group slot */
    int idx;   /* catalog index (P_DOWN) or local index (P_LOCAL/P_PICK) */
} MRow;

typedef struct
{
    char name[64];
    int  expanded;
    int  group;  /* P_LOCAL: the Cd group */
} MCat;

static int      s_open;
static int      s_page;
static int      s_closing;
static NmScroll s_scroll;
static int      s_onBack;
static int      s_onPreviewBtn;
static int      s_hubSel;
static int      s_changed;

static MRow s_rows[M_MAX_ROWS];
static int  s_rowCount;
static MCat s_cats[M_MAX_CATS];
static int  s_catCount;

/* Dialogs. */
enum { D_NONE, D_REPLACE, D_JOJO, D_RESTART, D_INFO };
static int       s_dialog;
static NmConfirm s_confirm;
static int       s_pendingIdx;   /* D_REPLACE: catalog index or -(cat+1) */
static char      s_dialogText[160];

/* Picker. */
static char s_pickStock[16];
static int  s_pickStockId;
static char s_pickCur[64];
static int  s_pickDone;
static char s_pickFile[64];
static int  s_pickStockChosen;

static int s_restartShown;
static int s_wasBusy;
static int s_jojoAsked;   /* the update offer is made once per visit */

/* ------------------------------------------------------------------------ */
/* drawing helpers                                                           */

static int m_interlaced(void) { return g_GameWork.gsScreenHeight == FRAMEBUFFER_HEIGHT_INTERLACED; }
static s16 m_px(int x) { return (s16)(x - 160); }
static s16 m_py(int y) { return (s16)(m_interlaced() ? (y - 112) * 2 : (y - 112)); }

static void m_line(int x0, int y0, int x1, int y1, u8 r, u8 g, u8 b)
{
    LINE_F2* line = (LINE_F2*)GsOUT_PACKET_P;

    setLineF2(line);
    setRGB0(line, r, g, b);
    setXY2(line, m_px(x0), m_py(y0), m_px(x1), m_py(y1));
    addPrim(&g_OrderingTable2[g_ActiveBufferIdx].org[M_OT_LINE], line);
    GsOUT_PACKET_P = (PACKET*)(line + 1);
}

static void m_frame(int x0, int y0, int x1, int y1)
{
    u8 o = Q8_COLOR(0.375f), in = Q8_COLOR(0.125f);

    m_line(x0, y0, x1, y0, o, o, 0xFF);
    m_line(x0, y1, x1, y1, o, o, 0xFF);
    m_line(x0, y0, x0, y1, o, o, 0xFF);
    m_line(x1, y0, x1, y1, o, o, 0xFF);
    m_line(x0 + 2, y0 + 2, x1 - 2, y0 + 2, in, in, 0xFF);
    m_line(x0 + 2, y1 - 2, x1 - 2, y1 - 2, in, in, 0xFF);
    m_line(x0 + 2, y0 + 2, x0 + 2, y1 - 2, in, in, 0xFF);
    m_line(x1 - 2, y0 + 2, x1 - 2, y1 - 2, in, in, 0xFF);
}

/* state: 0 empty, 1 ticked, 2 partly (a category with some of its rows). */
static void m_checkbox(int x, int y, int state, int sel)
{
    u8 c = sel ? 0xE0 : 0xA0;
    int y0 = y + 3, x1 = x + M_BOX, y1 = y0 + M_BOX;

    m_line(x, y0, x1, y0, c, c, c);
    m_line(x, y1, x1, y1, c, c, c);
    m_line(x, y0, x, y1, c, c, c);
    m_line(x1, y0, x1, y1, c, c, c);
    if (state == 1)
    {
        m_line(x + 2, y0 + 5, x + 4, y1 - 2, 0x60, 0xFF, 0x60);
        m_line(x + 4, y1 - 2, x1 - 2, y0 + 2, 0x60, 0xFF, 0x60);
    }
    else if (state == 2)
        m_line(x + 2, y0 + M_BOX / 2, x1 - 2, y0 + M_BOX / 2, 0xFF, 0xC0, 0x40);
}

static void m_plusminus(int x, int y, int expanded)
{
    int cy = y + 7;

    m_line(x, cy, x + 6, cy, 0xC0, 0xC0, 0xC0);
    if (!expanded)
        m_line(x + 3, cy - 3, x + 3, cy + 3, 0xC0, 0xC0, 0xC0);
}

/* The picker's stock row by name: the Play As cast entry for that body. */
static const char* m_stock_label(void)
{
    int i;

    for (i = 0; i < Pc_PlayAs_Count(); i++)
        if (Pc_PlayAs_File(i) == NULL && Pc_PlayAs_CharaId(i) == s_pickStockId)
            return Ct_Name(Pc_PlayAs_Label(i));
    return Ct_Tr("Original model");
}

static void m_text(int x, int y, int color, const char* s, int maxW, int sel)
{
    Ct_DrawText(x, y, 1, color, s, maxW, sel);
}

/* Detail and status lines: TrueType, so long text keeps its case and fits. */
static void m_small(int x, int y, int color, const char* s, int maxW)
{
    Ct_Small(x, y, color, s, maxW);
}

/* ------------------------------------------------------------------------ */
/* model of each page                                                        */

static int m_cat_find_or_add(const char* name, int group)
{
    int i;

    for (i = 0; i < s_catCount; i++)
        if (strcmp(s_cats[i].name, name) == 0 && s_cats[i].group == group)
            return i;
    if (s_catCount >= M_MAX_CATS)
        return -1;
    snprintf(s_cats[s_catCount].name, sizeof(s_cats[0].name), "%s", name);
    s_cats[s_catCount].group    = group;
    s_cats[s_catCount].expanded = 0;
    return s_catCount++;
}

/* "Other" last, the rest alphabetical. */
static int m_cat_cmp(const void* a, const void* b)
{
    const MCat* x = (const MCat*)a;
    const MCat* y = (const MCat*)b;
    int         xo = SDL_strcasecmp(x->name, "Other") == 0;
    int         yo = SDL_strcasecmp(y->name, "Other") == 0;

    if (xo != yo)
        return xo - yo;
    return SDL_strcasecmp(x->name, y->name);
}

static const char* m_item_name(int idx)
{
    if (s_page == P_DOWN)
        return Cd_CatalogGet(idx)->name;
    return Cd_LocalGet(idx)->name;
}

static int m_item_cmp_name(const int* a, const int* b)
{
    return SDL_strcasecmp(m_item_name(*a), m_item_name(*b));
}

/* The category a picker row sorts under: its category, else its author (the
 * ini's, else its pack's), else the pack it came in, else Other. */
static void m_pick_cat(const CdLocal* l, char* out, int outSz)
{
    const CdGroup* g = Cd_GroupGet(l->group);

    if (l->category[0])
        snprintf(out, (size_t)outSz, "%s", l->category);
    else if (l->author[0])
        snprintf(out, (size_t)outSz, "%s", l->author);
    else if (g != NULL && g->source == CD_SRC_PACK && g->name[0])
        snprintf(out, (size_t)outSz, "%s", g->name);
    else
        snprintf(out, (size_t)outSz, "Other");
}

static void m_build(void)
{
    int  expanded[M_MAX_CATS][2];
    char expName[M_MAX_CATS][64];
    int  nExp = 0, i, c, n;

    /* Keep what was open across a rebuild. */
    for (i = 0; i < s_catCount && nExp < M_MAX_CATS; i++)
    {
        snprintf(expName[nExp], sizeof(expName[0]), "%s", s_cats[i].name);
        expanded[nExp][0] = s_cats[i].expanded;
        expanded[nExp][1] = s_cats[i].group;
        nExp++;
    }
    s_catCount = 0;
    s_rowCount = 0;

    if (s_page == P_DOWN)
    {
        for (i = 0; i < Cd_CatalogCount(); i++)
            m_cat_find_or_add(Cd_CatalogGet(i)->category, -1);
    }
    else if (s_page == P_LOCAL)
    {
        for (i = 0; i < Cd_GroupCount(); i++)
            m_cat_find_or_add(Cd_GroupGet(i)->name, i);
    }
    else if (s_page == P_PICK)
    {
        for (i = 0; i < Cd_LocalCount(); i++)
        {
            char cat[64];

            if (!Cd_LocalGet(i)->enabled)
                continue;
            m_pick_cat(Cd_LocalGet(i), cat, sizeof(cat));
            m_cat_find_or_add(cat, -1);
        }
    }
    qsort(s_cats, (size_t)s_catCount, sizeof(s_cats[0]), m_cat_cmp);
    /* Harry can also be played as the stock cast (Play As), listed first. */
    if (s_page == P_PICK && strcmp(s_pickStock, "HERO") == 0 && s_catCount < M_MAX_CATS)
    {
        memmove(&s_cats[1], &s_cats[0], (size_t)s_catCount * sizeof(s_cats[0]));
        snprintf(s_cats[0].name, sizeof(s_cats[0].name), "Silent Hill 1 cast");
        s_cats[0].group    = M_CAST_GROUP;
        s_cats[0].expanded = 0;
        s_catCount++;
    }
    for (c = 0; c < s_catCount; c++)
        for (i = 0; i < nExp; i++)
            if (strcmp(expName[i], s_cats[c].name) == 0 && (s_page != P_LOCAL || expanded[i][1] == s_cats[c].group))
                s_cats[c].expanded = expanded[i][0];

    if (s_page == P_PICK && s_rowCount < M_MAX_ROWS)
    {
        s_rows[s_rowCount].type = R_STOCK;
        s_rows[s_rowCount].cat  = -1;
        s_rows[s_rowCount].idx  = -1;
        s_rowCount++;
    }
    for (c = 0; c < s_catCount && s_rowCount < M_MAX_ROWS; c++)
    {
        static int items[1024];

        n = 0;
        if (s_cats[c].group == M_CAST_GROUP)
        {
            s_rows[s_rowCount].type = R_HEADER;
            s_rows[s_rowCount].cat  = c;
            s_rows[s_rowCount].idx  = -1;
            s_rowCount++;
            if (s_cats[c].expanded)
                for (i = 0; i < Pc_PlayAs_Count() && s_rowCount < M_MAX_ROWS; i++)
                {
                    if (Pc_PlayAs_File(i) != NULL || Pc_PlayAs_CharaId(i) == Chara_Harry)
                        continue;
                    s_rows[s_rowCount].type = R_PLAYAS;
                    s_rows[s_rowCount].cat  = c;
                    s_rows[s_rowCount].idx  = i;
                    s_rowCount++;
                }
            continue;
        }
        if (s_page == P_DOWN)
        {
            for (i = 0; i < Cd_CatalogCount() && n < (int)(sizeof(items) / sizeof(items[0])); i++)
                if (strcmp(Cd_CatalogGet(i)->category, s_cats[c].name) == 0)
                    items[n++] = i;
        }
        else
        {
            for (i = 0; i < Cd_LocalCount() && n < (int)(sizeof(items) / sizeof(items[0])); i++)
            {
                const CdLocal* l = Cd_LocalGet(i);
                char           cat[64];

                if (s_page == P_LOCAL && l->group != s_cats[c].group)
                    continue;
                if (s_page == P_PICK)
                {
                    if (!l->enabled)
                        continue;
                    m_pick_cat(l, cat, sizeof(cat));
                    if (strcmp(cat, s_cats[c].name) != 0)
                        continue;
                }
                items[n++] = i;
            }
        }
        qsort(items, (size_t)n, sizeof(items[0]), (int (*)(const void*, const void*))m_item_cmp_name);
        s_rows[s_rowCount].type = R_HEADER;
        s_rows[s_rowCount].cat  = c;
        s_rows[s_rowCount].idx  = -1;
        s_rowCount++;
        if (!s_cats[c].expanded)
            continue;
        for (i = 0; i < n && s_rowCount < M_MAX_ROWS; i++)
        {
            s_rows[s_rowCount].type = R_ITEM;
            s_rows[s_rowCount].cat  = c;
            s_rows[s_rowCount].idx  = items[i];
            s_rowCount++;
        }
    }
    NmScroll_SetCount(&s_scroll, s_rowCount > 0 ? s_rowCount : 1);
}

/* Download page check states. */
static int m_down_item_state(int idx)
{
    return Cd_IsDownloaded(Cd_CatalogGet(idx)) ? 1 : 0;
}

static int m_cat_state(int c)
{
    int i, on = 0, all = 0;

    if (s_page == P_LOCAL)
    {
        const CdGroup* g = Cd_GroupGet(s_cats[c].group);

        if (g->source != CD_SRC_DOWNLOAD)
            return g->enabled ? 1 : 0;
        return 1;
    }
    for (i = 0; i < Cd_CatalogCount(); i++)
        if (strcmp(Cd_CatalogGet(i)->category, s_cats[c].name) == 0)
        {
            all++;
            on += m_down_item_state(i);
        }
    return on == 0 ? 0 : (on == all ? 1 : 2);
}

/* ------------------------------------------------------------------------ */
/* actions                                                                   */

static void m_install(int idx, int confirmed)
{
    const CdEntry* e = Cd_CatalogGet(idx);

    (void)e;
    (void)confirmed;
    Cd_QueueInstall(idx);
    s_changed = 1;
}

static void m_toggle_item(int idx)
{
    SD_Call(Sfx_MenuConfirm);
    if (Cd_IsDownloaded(Cd_CatalogGet(idx)) && !Cd_NeedsUpdate(Cd_CatalogGet(idx)))
    {
        Cd_QueueRemove(idx);
        s_changed = 1;
    }
    else
        m_install(idx, 0);
}

static void m_toggle_category(int c, int confirmed)
{
    int i, all = 1;

    (void)confirmed;
    for (i = 0; i < Cd_CatalogCount(); i++)
        if (strcmp(Cd_CatalogGet(i)->category, s_cats[c].name) == 0 && !Cd_IsDownloaded(Cd_CatalogGet(i)))
            all = 0;
    SD_Call(Sfx_MenuConfirm);
    for (i = 0; i < Cd_CatalogCount(); i++)
        if (strcmp(Cd_CatalogGet(i)->category, s_cats[c].name) == 0)
        {
            if (all)
                Cd_QueueRemove(i);
            else if (!Cd_IsDownloaded(Cd_CatalogGet(i)) || Cd_NeedsUpdate(Cd_CatalogGet(i)))
                Cd_QueueInstall(i);
        }
    s_changed = 1;
}

static void m_local_toggle(int row)
{
    const MRow* r = &s_rows[row];

    SD_Call(Sfx_MenuConfirm);
    if (r->type == R_HEADER)
    {
        const CdGroup* g = Cd_GroupGet(s_cats[r->cat].group);

        if (g->source == CD_SRC_DOWNLOAD)
            return;
        Cd_SetGroupEnabled(s_cats[r->cat].group, !g->enabled);
        s_changed = 1;
        return;
    }
    {
        const CdLocal* l = Cd_LocalGet(r->idx);
        int            i;

        /* Downloads come off one at a time; packs switch as a whole. */
        if (l->source == CD_SRC_DOWNLOAD)
        {
            for (i = 0; i < Cd_CatalogCount(); i++)
                if (SDL_strcasecmp(Cd_CatalogGet(i)->hero, l->hero) == 0)
                    Cd_QueueRemove(i);
        }
        else
            Cd_SetGroupEnabled(l->group, !Cd_GroupGet(l->group)->enabled);
        s_changed = 1;
    }
}

static void m_pick(int row)
{
    const MRow* r = &s_rows[row];

    SD_Call(Sfx_MenuConfirm);
    s_pickDone = 1;
    if (r->type == R_STOCK)
    {
        s_pickStockChosen = 1;
        s_pickFile[0] = '\0';
    }
    else if (r->type == R_PLAYAS)
    {
        /* '*' marks a Play As cast name rather than a file. */
        s_pickStockChosen = 0;
        snprintf(s_pickFile, sizeof(s_pickFile), "*%s", Pc_PlayAs_Name(r->idx));
    }
    else
    {
        s_pickStockChosen = 0;
        snprintf(s_pickFile, sizeof(s_pickFile), "%s", Cd_LocalGet(r->idx)->hero);
    }
    s_closing = 1;
}

static void m_activate_row(int row)
{
    const MRow* r = &s_rows[row];

    if (s_page == P_PICK && r->type != R_HEADER)
    {
        m_pick(row);
        return;
    }
    if (r->type == R_HEADER)
    {
        if (s_page == P_DOWN)
            m_toggle_category(r->cat, 0);
        else if (s_page == P_LOCAL)
            m_local_toggle(row);
        else
        {
            SD_Call(Sfx_MenuMove);
            s_cats[r->cat].expanded = !s_cats[r->cat].expanded;
            m_build();
        }
        return;
    }
    if (s_page == P_DOWN)
        m_toggle_item(r->idx);
    else
        m_local_toggle(row);
}

static void m_set_page(int page)
{
    s_page          = page;
    s_onBack        = 0;
    s_onPreviewBtn  = 0;
    s_catCount      = 0;
    NmScroll_Init(&s_scroll, 1, M_VISIBLE);
    if (page == P_DOWN)
        Cd_CatalogFetch();
    if (page != P_HUB)
        m_build();
}

/* ------------------------------------------------------------------------ */
/* open / close                                                              */

void Cdm_Open(int page, const char* stock, const char* current, int stockCharaId)
{
    s_pickStockId = stockCharaId;
    s_open    = 1;
    s_closing = 0;
    s_dialog  = D_NONE;
    s_pickDone = 0;
    s_hubSel  = 0;
    Cd_LocalScan();
    if (page == CDM_PICKER)
    {
        int i;

        snprintf(s_pickStock, sizeof(s_pickStock), "%s", stock ? stock : "HERO");
        snprintf(s_pickCur, sizeof(s_pickCur), "%s", current ? current : "");
        m_set_page(P_PICK);
        /* Open on the current model, its category expanded. */
        for (i = 0; i < s_rowCount; i++)
            if (s_rows[i].type == R_ITEM && SDL_strcasecmp(Cd_LocalGet(s_rows[i].idx)->hero, s_pickCur) == 0)
                break;
        if (s_pickCur[0])
        {
            int c;
            for (c = 0; c < s_catCount; c++)
            {
                int k;
                for (k = 0; k < Cd_LocalCount(); k++)
                {
                    char cat[64];
                    if (SDL_strcasecmp(Cd_LocalGet(k)->hero, s_pickCur) != 0)
                        continue;
                    m_pick_cat(Cd_LocalGet(k), cat, sizeof(cat));
                    if (strcmp(cat, s_cats[c].name) == 0)
                        s_cats[c].expanded = 1;
                }
            }
            m_build();
            for (i = 0; i < s_rowCount; i++)
                if (s_rows[i].type == R_ITEM && SDL_strcasecmp(Cd_LocalGet(s_rows[i].idx)->hero, s_pickCur) == 0)
                {
                    NmScroll_Select(&s_scroll, i);
                    break;
                }
        }
    }
    else
    {
        s_jojoAsked = 0;
        m_set_page(P_HUB);
        Cd_CatalogFetch();
    }
}

int Cdm_IsOpen(void) { return s_open; }

int Cdm_PickerResult(const char** file)
{
    if (!s_pickDone)
        return 0;
    s_pickDone = 0;
    *file = s_pickStockChosen ? NULL : s_pickFile;
    return 1;
}

int Cdm_TakeChanged(void)
{
    int c = s_changed;

    s_changed = 0;
    return c;
}

/* ------------------------------------------------------------------------ */
/* input                                                                     */

static int m_back_right(void) { return M_BACK_X + NM_BULLET_TEXT_DX + Nm_TextWidth("Back"); }

/* Row under the mouse, or -2 Back, -3 the preview button, -1 nothing.
 * *box is set when the pointer is on a row's checkbox, *pm on its +/-. */
static int m_hit(const NmInput* in, int* box, int* pm)
{
    int i;

    *box = *pm = 0;
    if (!in->mouseOk)
        return -1;
    if (in->mouseX >= M_BACK_X - 1 && in->mouseX < m_back_right() + 4 && in->mouseY >= M_FOOTER_Y &&
        in->mouseY < M_FOOTER_Y + 16)
        return -2;
    if (s_page == P_DOWN && in->mouseX >= M_PREV_X0 && in->mouseX < M_PREV_X1 && in->mouseY >= M_BTN_Y - 2 &&
        in->mouseY < M_BTN_Y + 14)
        return -3;
    if (s_page == P_HUB)
    {
        for (i = 0; i < 2; i++)
        {
            int y = M_LIST_Y + 16 + i * 24;
            if (in->mouseY >= y - 1 && in->mouseY < y + 15 && in->mouseX >= M_LIST_X0 && in->mouseX < M_LIST_X1)
                return i;
        }
        return -1;
    }
    for (i = 0; i < M_VISIBLE && s_scroll.top + i < s_rowCount; i++)
    {
        int         y = M_LIST_Y + i * M_PITCH - 1;
        int         row = s_scroll.top + i;
        const MRow* r = &s_rows[row];
        int         x0 = M_LIST_X0 + ((r->type == R_ITEM || r->type == R_PLAYAS) ? 16 : 0);

        if (in->mouseY < y || in->mouseY >= y + M_PITCH || in->mouseX < s_fl || in->mouseX >= M_DIV_X)
            continue;
        if (s_page != P_PICK && in->mouseX >= x0 - 2 && in->mouseX < x0 + M_BOX + 3)
            *box = 1;
        if (r->type == R_HEADER && in->mouseX >= x0 + (s_page == P_PICK ? 0 : M_BOX + 4) - 2 &&
            in->mouseX < x0 + (s_page == P_PICK ? 0 : M_BOX + 4) + 9)
            *pm = 1;
        return row;
    }
    return -1;
}

static int m_dialog_update(const NmInput* in)
{
    NmConfirmResult res;

    if (s_dialog == D_RESTART || s_dialog == D_INFO)
    {
        if (in->confirm || in->cancel || in->mouseLeft)
        {
            SD_Call(Sfx_MenuConfirm);
            s_dialog = D_NONE;
        }
        return 1;
    }
    res = NmConfirm_Update(&s_confirm, in);
    if (res == NM_CONFIRM_PENDING)
        return 1;
    SD_Call(res == NM_CONFIRM_YES ? Sfx_MenuConfirm : Sfx_MenuCancel);
    if (res == NM_CONFIRM_YES)
    {
        if (s_dialog == D_REPLACE)
        {
            if (s_pendingIdx >= 0)
                m_install(s_pendingIdx, 1);
            else
                m_toggle_category(-s_pendingIdx - 1, 1);
        }
        else if (s_dialog == D_JOJO)
        {
            Cd_UpdateOldJojo();
            s_changed = 1;
        }
    }
    s_dialog = D_NONE;
    return 1;
}

int Cdm_Update(const NmInput* in)
{
    int hit, box, pm, oldSel, oldBack;

    if (!s_open)
        return 0;
    Cd_Tick();
    /* A job finishing (or the list arriving) changes what the rows show. */
    if (s_wasBusy && !Cd_Busy() && s_page != P_HUB && s_page != P_PICK)
        m_build();
    s_wasBusy = Cd_Busy();
    if (Cd_RestartAdvised() && !Cd_Busy() && !s_restartShown && s_dialog == D_NONE)
    {
        s_restartShown = 1;
        snprintf(s_dialogText, sizeof(s_dialogText), "%s", Ct_Tr("Restart the game to use the updated characters."));
        s_dialog = D_RESTART;
    }
    if (s_page == P_HUB && s_dialog == D_NONE && !s_jojoAsked && Cd_OldJojoPending())
    {
        s_jojoAsked = 1;
        snprintf(s_dialogText, sizeof(s_dialogText), "%s",
                 Ct_Tr("Newer versions of jojo670's characters are available. Update them?"));
        s_dialog = D_JOJO;
        NmConfirm_Open(&s_confirm);
    }
    if (s_dialog != D_NONE)
        return m_dialog_update(in);

    if (s_closing)
    {
        s_open = 0;
        return 0;
    }

    if (in->cancel || in->mouseRight)
    {
        SD_Call(Sfx_MenuCancel);
        if (s_page == P_DOWN || s_page == P_LOCAL)
        {
            m_set_page(P_HUB);
            return 1;
        }
        s_open = 0;
        return 0;
    }

    hit = m_hit(in, &box, &pm);

    if (s_page == P_HUB)
    {
        int old = s_hubSel;

        if (in->mouseMoved && hit >= 0)
            s_hubSel = hit;
        else if (in->mouseMoved && hit == -2)
            s_hubSel = 2;
        if (in->up)
            s_hubSel = (s_hubSel + 2) % 3;
        if (in->down)
            s_hubSel = (s_hubSel + 1) % 3;
        if (s_hubSel != old)
            SD_Call(Sfx_MenuMove);
        if (in->confirm || (in->mouseLeft && hit != -1))
        {
            SD_Call(Sfx_MenuConfirm);
            if (s_hubSel == 2)
            {
                s_open = 0;
                return 0;
            }
            m_set_page(s_hubSel == 0 ? P_DOWN : P_LOCAL);
        }
        return 1;
    }

    oldSel  = s_scroll.sel;
    oldBack = s_onBack;

    if (in->wheel != 0)
    {
        NmScroll_Wheel(&s_scroll, in->wheel);
        s_onBack = 0;
    }
    if (in->mouseLeft && hit != -1)
    {
        Nm_HighlightSnap();
        if (hit == -2)
        {
            SD_Call(Sfx_MenuCancel);
            if (s_page == P_PICK)
            {
                s_open = 0;
                return 0;
            }
            m_set_page(P_HUB);
            return 1;
        }
        if (hit == -3)
        {
            int r = s_scroll.sel;
            if (r >= 0 && r < s_rowCount && s_rows[r].type == R_ITEM)
            {
                SD_Call(Sfx_MenuConfirm);
                Cd_QueuePreview(s_rows[r].idx);
            }
            return 1;
        }
        NmScroll_Select(&s_scroll, hit);
        s_onBack = 0;
        s_onPreviewBtn = 0;
        if (pm && s_rows[hit].type == R_HEADER)
        {
            SD_Call(Sfx_MenuMove);
            s_cats[s_rows[hit].cat].expanded = !s_cats[s_rows[hit].cat].expanded;
            m_build();
            return 1;
        }
        if (box || s_page == P_PICK || (s_rows[hit].type == R_HEADER && s_page == P_PICK))
            m_activate_row(hit);
        else if (s_rows[hit].type == R_HEADER)
        {
            SD_Call(Sfx_MenuMove);
            s_cats[s_rows[hit].cat].expanded = !s_cats[s_rows[hit].cat].expanded;
            m_build();
        }
        return 1;
    }
    if (in->mouseMoved && hit != -1)
    {
        if (hit == -2)
            s_onBack = 1;
        else if (hit == -3)
            s_onPreviewBtn = 1;
        else
        {
            NmScroll_Select(&s_scroll, hit);
            s_onBack = 0;
            s_onPreviewBtn = 0;
        }
    }

    if (in->up)
    {
        if (s_onBack)
            s_onBack = 0;
        else
            NmScroll_Move(&s_scroll, -1, 0);
        s_onPreviewBtn = 0;
    }
    else if (in->down && !s_onBack)
    {
        if (s_scroll.sel >= s_rowCount - 1)
            s_onBack = 1;
        else
            NmScroll_Move(&s_scroll, +1, 0);
        s_onPreviewBtn = 0;
    }
    else if (in->left && !s_onBack)
    {
        const MRow* r = &s_rows[s_scroll.sel];

        if (s_onPreviewBtn)
            s_onPreviewBtn = 0;
        else if (r->type == R_HEADER && s_cats[r->cat].expanded)
        {
            s_cats[r->cat].expanded = 0;
            m_build();
            SD_Call(Sfx_MenuMove);
        }
        else if (r->type == R_ITEM)
        {
            int i;
            /* Back up to the category it sits in. */
            for (i = s_scroll.sel; i >= 0; i--)
                if (s_rows[i].type == R_HEADER)
                {
                    NmScroll_Select(&s_scroll, i);
                    break;
                }
        }
    }
    else if (in->right && !s_onBack)
    {
        const MRow* r = &s_rows[s_scroll.sel];

        if (r->type == R_HEADER && !s_cats[r->cat].expanded)
        {
            s_cats[r->cat].expanded = 1;
            m_build();
            SD_Call(Sfx_MenuMove);
        }
        else if (r->type == R_ITEM && s_page == P_DOWN && !Cd_PreviewFile(r->idx))
        {
            s_onPreviewBtn = 1;
            SD_Call(Sfx_MenuMove);
        }
    }

    if ((s_scroll.sel != oldSel || s_onBack != oldBack) && !in->mouseMoved)
        SD_Call(Sfx_MenuMove);

    if (in->confirm)
    {
        if (s_onBack)
        {
            SD_Call(Sfx_MenuCancel);
            if (s_page == P_PICK)
            {
                s_open = 0;
                return 0;
            }
            m_set_page(P_HUB);
            return 1;
        }
        if (s_onPreviewBtn)
        {
            SD_Call(Sfx_MenuConfirm);
            Cd_QueuePreview(s_rows[s_scroll.sel].idx);
            s_onPreviewBtn = 0;
            return 1;
        }
        if (s_rowCount > 0)
            m_activate_row(s_scroll.sel);
    }
    return 1;
}

/* ------------------------------------------------------------------------ */
/* drawing                                                                   */

static void m_back_draw(int selected)
{
    Nm_BulletDraw(M_BACK_X, M_FOOTER_Y, selected);
    Nm_TextDraw(M_BACK_X + NM_BULLET_TEXT_DX, M_FOOTER_Y, NM_TEXT_TALL, StringColorId_White, "Back");
    if (selected)
        Nm_HighlightDraw(M_BACK_X - 1, m_back_right() + 4, M_FOOTER_Y + 14);
}

static void m_rows_draw(void)
{
    int i;

    for (i = 0; i < M_VISIBLE && s_scroll.top + i < s_rowCount; i++)
    {
        int         row   = s_scroll.top + i;
        const MRow* r     = &s_rows[row];
        int         y     = M_LIST_Y + i * M_PITCH;
        int         sel   = !s_onBack && !s_onPreviewBtn && row == s_scroll.sel;
        int         color = sel ? StringColorId_Gold : StringColorId_White;
        int         x     = M_LIST_X0 + ((r->type == R_ITEM || r->type == R_PLAYAS) ? 16 : 0);
        int         tx;
        char        label[96];

        if (r->type == R_PLAYAS)
        {
            int cur = s_pickCur[0] == '*' && strcmp(s_pickCur + 1, Pc_PlayAs_Name(r->idx)) == 0;
            snprintf(label, sizeof(label), "%s%s", Ct_Name(Pc_PlayAs_Label(r->idx)), cur ? "  *" : "");
            m_text(x, y, color, label, M_LIST_X1 - x, sel);
            continue;
        }
        if (r->type == R_STOCK)
        {
            int cur = s_pickCur[0] == '\0';
            snprintf(label, sizeof(label), "%s%s", m_stock_label(), cur ? "  *" : "");
            m_text(x, y, color, label, M_LIST_X1 - x, sel);
            continue;
        }
        if (r->type == R_HEADER)
        {
            if (s_page != P_PICK)
            {
                if (!(s_page == P_LOCAL && Cd_GroupGet(s_cats[r->cat].group)->source == CD_SRC_DOWNLOAD))
                    m_checkbox(x, y, m_cat_state(r->cat), sel);
                m_plusminus(x + M_BOX + 4, y, s_cats[r->cat].expanded);
                tx = x + M_BOX + 14;
            }
            else
            {
                m_plusminus(x, y, s_cats[r->cat].expanded);
                tx = x + 10;
            }
            m_text(tx, y, color, Ct_Tr(s_cats[r->cat].name), M_LIST_X1 - tx, sel);
            continue;
        }
        if (s_page == P_DOWN)
        {
            const CdEntry* e = Cd_CatalogGet(r->idx);
            int            st = Cd_IsDownloaded(e) ? 1 : 0;

            m_checkbox(x, y, st, sel);
            const char* tag = Cd_NeedsUpdate(e) ? Ct_Tr("(update)") : Cd_CustomCopyWins(e) ? Ct_Tr("(custom copy in use)") : "";

            snprintf(label, sizeof(label), "%s%s%s", e->name, tag[0] ? " " : "", tag);
        }
        else if (s_page == P_LOCAL)
        {
            const CdLocal* l = Cd_LocalGet(r->idx);

            m_checkbox(x, y, l->enabled ? 1 : 0, sel);
            snprintf(label, sizeof(label), "%s", l->name);
        }
        else
        {
            const CdLocal* l = Cd_LocalGet(r->idx);
            int            cur = SDL_strcasecmp(l->hero, s_pickCur) == 0;

            snprintf(label, sizeof(label), "%s%s", l->name, cur ? "  *" : "");
            m_text(x, y, color, label, M_LIST_X1 - x, sel);
            continue;
        }
        tx = x + M_BOX + 6;
        m_text(tx, y, color, label, M_LIST_X1 - tx, sel);
    }
    if (s_rowCount > M_VISIBLE)
    {
        Nm_ScrollArrowDraw((M_LIST_X0 + M_LIST_X1) / 2, M_ARROW_UP_Y, 1, NmScroll_CanUp(&s_scroll));
        Nm_ScrollArrowDraw((M_LIST_X0 + M_LIST_X1) / 2, M_ARROW_DN_Y, 0, NmScroll_CanDown(&s_scroll));
    }
}

/* The selected row's details under the preview. */
static void m_info_draw(void)
{
    const MRow* r;
    char        name[96] = "", game[96] = "", ver[32] = "", author[96] = "";
    int         x0 = M_PREV_X0 + 8, x1 = M_PREV_X1 - 8, mid = (x0 + x1) / 2 + 12;

    if (s_rowCount == 0 || s_scroll.sel >= s_rowCount)
        return;
    r = &s_rows[s_scroll.sel];
    if (r->type == R_HEADER)
    {
        snprintf(name, sizeof(name), "%s", Ct_Tr(s_cats[r->cat].name));
        if (s_page == P_LOCAL && Cd_GroupGet(s_cats[r->cat].group)->author[0])
            snprintf(author, sizeof(author), "%s", Cd_GroupGet(s_cats[r->cat].group)->author);
    }
    else if (r->type == R_STOCK)
    {
        snprintf(name, sizeof(name), "%s", m_stock_label());
        snprintf(game, sizeof(game), "Silent Hill");
        snprintf(author, sizeof(author), "Konami");
    }
    else if (r->type == R_PLAYAS)
    {
        snprintf(name, sizeof(name), "%s", Ct_Name(Pc_PlayAs_Label(r->idx)));
        snprintf(game, sizeof(game), "Silent Hill");
        snprintf(author, sizeof(author), "Konami");
    }
    else if (s_page == P_DOWN)
    {
        const CdEntry* e = Cd_CatalogGet(r->idx);
        snprintf(name, sizeof(name), "%s", e->name);
        snprintf(game, sizeof(game), "%s", e->category);
        snprintf(ver, sizeof(ver), "v%s", e->version[0] ? e->version : "1");
        snprintf(author, sizeof(author), "%s", e->author);
    }
    else
    {
        const CdLocal* l = Cd_LocalGet(r->idx);
        snprintf(name, sizeof(name), "%s", l->name);
        snprintf(game, sizeof(game), "%s", l->category);
        if (l->version[0])
            snprintf(ver, sizeof(ver), "v%s", l->version);
        snprintf(author, sizeof(author), "%s", l->author);
    }
    (void)mid;
    {
        char top[160];

        /* Name and version share the first line; the version is short. */
        snprintf(top, sizeof(top), "%s%s%s", name, ver[0] ? "   " : "", ver);
        m_small(x0, M_INFO_Y0, StringColorId_White, top, x1 - x0);
        m_small(x0, M_INFO_Y0 + 9, StringColorId_LightGrey, game, x1 - x0);
        m_small(x0, M_INFO_Y0 + 18, StringColorId_LightGrey, author, x1 - x0);
    }
}

static void m_preview_panel(void)
{
    const char* file = NULL;
    int         cx = (M_PREV_X0 + M_PREV_X1) / 2;

    m_frame(M_PREV_X0, M_PREV_Y0, M_PREV_X1, M_PREV_Y1);
    if (Cdm_PreviewTarget(&file) != NULL && !Pc_CharaPreview_Showing() && Cd_Busy() == 0)
        Nm_TextDrawCentered(cx, (M_PREV_Y0 + M_BTN_Y) / 2 - 8, NM_TEXT_TALL, StringColorId_DarkGrey, "?");
    if (s_page == P_DOWN && s_rowCount > 0 && s_rows[s_scroll.sel].type == R_ITEM &&
        !Cd_PreviewFile(s_rows[s_scroll.sel].idx))
    {
        const char* lbl = "Download Preview";
        int         w   = Ct_Width(lbl, 0);

        Ct_Draw(cx - w / 2, M_BTN_Y, 0, s_onPreviewBtn ? StringColorId_Gold : StringColorId_White, lbl,
                M_PREV_X1 - M_PREV_X0, 0);
        m_line(cx - w / 2 - 4, M_BTN_Y - 2, cx + w / 2 + 4, M_BTN_Y - 2, 0xC0, 0x80, 0x50);
        m_line(cx - w / 2 - 4, M_BTN_Y + 12, cx + w / 2 + 4, M_BTN_Y + 12, 0xC0, 0x80, 0x50);
        m_line(cx - w / 2 - 4, M_BTN_Y - 2, cx - w / 2 - 4, M_BTN_Y + 12, 0xC0, 0x80, 0x50);
        m_line(cx + w / 2 + 4, M_BTN_Y - 2, cx + w / 2 + 4, M_BTN_Y + 12, 0xC0, 0x80, 0x50);
    }
    m_info_draw();
}

static void m_dialog_draw(void)
{
    int cx = (s_fl + s_fr) / 2;

    if (s_dialog == D_NONE)
        return;
    m_small(s_fl + 16, 104, StringColorId_White, s_dialogText, M_W - 32);
    if (s_dialog == D_RESTART || s_dialog == D_INFO)
    {
        Ct_DrawCentered(cx, 124, 1, StringColorId_Gold, "OK");
        return;
    }
    {
        static const char* const yn[2] = { "Yes", "No" };
        int                      i;

        for (i = 0; i < 2; i++)
        {
            int sel = (i == 0) ? s_confirm.yes : !s_confirm.yes;
            int w   = Ct_Width(yn[i], 1);
            int x   = ((i == 0) ? 120 : 200) - w / 2;

            Ct_Draw(x, 124, 1, sel ? StringColorId_Gold : StringColorId_White, yn[i], 0, 0);
            if (sel)
                Nm_HighlightDraw(x - 4, x + w + 4, 138);
        }
    }
}

void Cdm_Draw(int fieldL, int fieldR)
{
    const char* heading = "Additional Characters";
    float       prog;
    const char* status;

    if (!s_open)
        return;
    s_fl = fieldL;
    s_fr = fieldR;
    if (s_page == P_DOWN)
        heading = "Downloadable Characters";
    else if (s_page == P_LOCAL)
        heading = "Installed Characters";
    else if (s_page == P_PICK)
        heading = "Choose a Model";
    Ct_DrawCentered(160, M_HEADING_Y, 1, StringColorId_White, heading);
    Nm_RuleDraw(M_LIST_X0, M_PREV_X1, M_RULE_Y, 0);

    if (s_dialog != D_NONE && s_dialog != D_RESTART && s_dialog != D_INFO)
    {
        m_dialog_draw();
        Pc_MouseCursor_Show();
        return;
    }

    if (s_page == P_HUB)
    {
        static const char* const items[3] = { "Downloadable", "Locally installed", "Back" };
        int                      i;

        for (i = 0; i < 2; i++)
        {
            int y = M_LIST_Y + 16 + i * 24;
            int w;

            char up[96];

            Nm_BulletDraw(M_LIST_X0, y, s_hubSel == i);
            Pc_LangUtf8Upper(Ct_Tr(items[i]), up, (int)sizeof(up));
            w = Ct_TextWidth(up, 1);
            Ct_DrawText(M_LIST_X0 + NM_BULLET_TEXT_DX, y, 1, s_hubSel == i ? StringColorId_Gold : StringColorId_White,
                        up, 0, 0);
            if (s_hubSel == i)
                Nm_HighlightDraw(M_LIST_X0 - 1, M_LIST_X0 + NM_BULLET_TEXT_DX + w + 4, y + 14);
        }
        m_back_draw(s_hubSel == 2);
    }
    else
    {
        m_line(M_DIV_X, M_RULE_Y, M_DIV_X, M_ARROW_DN_Y + 6, Q8_COLOR(0.125f), Q8_COLOR(0.125f), 0xFF);
        if (s_page == P_DOWN && Cd_CatalogState() == CD_CAT_LOADING)
            m_text(M_LIST_X0, M_LIST_Y, StringColorId_LightGrey, Ct_Tr("Loading the list..."), M_LIST_X1 - M_LIST_X0, 0);
        else if (s_page == P_DOWN && Cd_CatalogState() == CD_CAT_FAILED)
            m_text(M_LIST_X0, M_LIST_Y, StringColorId_LightGrey, Ct_Tr("Could not reach the list."), M_LIST_X1 - M_LIST_X0, 0);
        else if (s_rowCount == 0)
            m_text(M_LIST_X0, M_LIST_Y, StringColorId_LightGrey, Ct_Tr("Nothing here yet."), M_LIST_X1 - M_LIST_X0, 0);
        else
            m_rows_draw();
        m_preview_panel();
        m_back_draw(s_onBack);
    }

    status = Cd_Status(&prog);
    if (status && status[0] && (Cd_Busy() || s_page != P_HUB))
    {
        char line[160];
        int  x = m_back_right() + 12;

        if (prog >= 0.0f && Cd_Busy())
            snprintf(line, sizeof(line), "%s %d%%", status, (int)(prog * 100.0f));
        else
            snprintf(line, sizeof(line), "%s", status);
        m_small(x, M_FOOTER_Y + 2, StringColorId_LightGrey, line, M_PREV_X1 - x);
    }
    m_dialog_draw();
    Pc_MouseCursor_Show();
}

/* ------------------------------------------------------------------------ */
/* preview                                                                   */

const char* Cdm_PreviewTarget(const char** file)
{
    const MRow* r;

    *file = NULL;
    if (!s_open || s_page == P_HUB || s_rowCount == 0 || s_scroll.sel >= s_rowCount || s_dialog != D_NONE)
        return NULL;
    r = &s_rows[s_scroll.sel];
    if (r->type == R_ITEM)
    {
        if (s_page == P_DOWN)
            *file = Cd_PreviewFile(r->idx);
        else
        {
            const CdLocal* l = Cd_LocalGet(r->idx);
            static char    off[160];

            if (l->enabled)
                *file = l->hero;
            else
            {
                /* Parked files still preview from where they sit. */
                const CdGroup* g = Cd_GroupGet(l->group);
                snprintf(off, sizeof(off), "../CHARA_off/%s/%s", g->key, l->hero);
                *file = off;
            }
        }
    }
    return s_page == P_PICK ? s_pickStock : "HERO";
}

int Cdm_PreviewStockId(void)
{
    const MRow* r;

    if (!s_open || s_page != P_PICK || s_rowCount == 0 || s_scroll.sel >= s_rowCount || s_dialog != D_NONE)
        return -1;
    r = &s_rows[s_scroll.sel];
    if (r->type == R_STOCK)
        return s_pickStockId;
    if (r->type == R_PLAYAS)
        return Pc_PlayAs_CharaId(r->idx);
    return -1;
}

void Cdm_PreviewRect(int* x0, int* y0, int* x1, int* y1)
{
    *x0 = M_PREV_X0 + 3;
    *y0 = M_PREV_Y0 + 3;
    *x1 = M_PREV_X1 - 3;
    *y1 = (s_page == P_DOWN || s_page == P_LOCAL || s_page == P_PICK) ? M_BTN_Y - 4 : M_PREV_Y1 - 3;
}
