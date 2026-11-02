/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_controls_screen.c - native Controls (rebind) screen. See
 * pc_controls_screen.h. pc_bind_panel.c (GL, Quick Options) is the
 * behavioural reference for activate, clear, capture and reset.
 */
#include "game.h"

#include <stdio.h>
#include <string.h>

#include "bodyprog/bodyprog.h"
#include "bodyprog/screen/screen_data.h"
#include "bodyprog/sound/sfx_id_enum.h"
#include "bodyprog/sound/sound_system.h"
#include "bodyprog/text/text_draw.h"
#include "dbg_overlay.h"
#include "lang_text.h"
#include "pc_bind_core.h"
#include "pc_bind_input.h"
#include "pc_config.h"
#include "pc_controls_screen.h"
#include "pc_input_glyphs.h"
#include "pc_native_menu.h"
#include "pc_native_menu_draw.h"
#include "sh_log.h"

extern int g_DebugThirdPersonCam;

/* Options draws only y 0..224 of the 240-line text space. */
#define CS_ICONS_Y      4
#define CS_RULE1_Y      23
#define CS_HEAD_Y       25
#define CS_RULE2_Y      42
#define CS_ARROW_UP_Y   49
#define CS_LIST_Y       56
#define CS_PITCH        16
#define CS_VISIBLE      8
#define CS_ARROW_DOWN_Y 189
#define CS_RULE3_Y      198
#define CS_FOOTER_Y     202
#define CS_CONFIRM_Y    96

#define CS_RULE_X0      4
#define CS_RULE_X1      316
#define CS_TEXT_X       28
#define CS_VALUE_RX     300
#define CS_LABEL_RX     129
#define CS_HEAD_FUNC_X  70
#define CS_HL_X0        2
#define CS_HL_X1        316
#define CS_ARROW_X      160
#define CS_ROW_H        16
#define CS_ICON_DY      0

/* After the text (NM_OT_TEXT 8), so each glyph quad's tpage cannot touch a font glyph. */
#define CS_OT_ICON      7

#define CS_RGB_WHITE    128
#define CS_RGB_DIM      64
#define CS_RGB_RED      0x800000

/* Widths from the FONT16 kerning table: the widest English label
 * ("Change_Camera", 129) starts at x 0; "Space" (48) fits both keyboard
 * cells; pad cells take "LS_U" (41) and "DP_U" (43), PAD2 ends at x 318.
 * Advances carry the glyph's own right gap, so cells abut. */
static const int s_colX[BC_COLS] = { 134, 182, 230, 275 };
static const int s_colW[BC_COLS] = { 48, 48, 43, 43 };

enum { CS_FOCUS_ICONS, CS_FOCUS_LIST, CS_FOCUS_BACK, CS_FOCUS_RESET };
enum { CS_HIT_NONE, CS_HIT_ICONS, CS_HIT_ROW, CS_HIT_BACK, CS_HIT_RESET, CS_HIT_YES, CS_HIT_NO };

static int       s_open;
static int       s_focus;
static int       s_col, s_wantCol;
static int       s_listen;
static NmScroll  s_list;
static NmConfirm s_confirm;
static BindNav   s_nav;
static int       s_set;

static void cs_beep(int sfxId)
{
    Sd_PlaySfx((u16)sfxId, 0, 64);
}

static int cs_cell_exists(int row, int col)
{
    const BcRow* r = BindCore_Row(row);

    return r != NULL && col >= 0 && col < BC_COLS && r->key[col] != NULL;
}

static int cs_stick_cell(int row, int col)
{
    const BcRow* r = BindCore_Row(row);

    return r != NULL && (r->flags & BCF_STICK) && col >= BC_PAD1 && r->key[col] == NULL;
}

/* The nearest cell of this row that exists, preferring the left side. */
static int cs_resolve_col(int row, int want)
{
    int d;

    if (cs_cell_exists(row, want))
        return want;
    for (d = 1; d < BC_COLS; d++)
    {
        if (cs_cell_exists(row, want - d))
            return want - d;
        if (cs_cell_exists(row, want + d))
            return want + d;
    }
    return BC_KB1;
}

static void cs_focus_list(int row)
{
    s_focus = CS_FOCUS_LIST;
    NmScroll_Select(&s_list, row);
    s_col = cs_resolve_col(s_list.sel, s_wantCol);
}

static void cs_move_row(int dir)
{
    const int last = BindCore_RowCount() - 1;

    switch (s_focus)
    {
        case CS_FOCUS_ICONS:
            if (dir > 0)
                cs_focus_list(0);
            else
                s_focus = CS_FOCUS_BACK;
            break;

        case CS_FOCUS_LIST:
            if (s_list.sel + dir < 0)
                s_focus = CS_FOCUS_ICONS;
            else if (s_list.sel + dir > last)
                s_focus = CS_FOCUS_BACK;
            else
                cs_focus_list(s_list.sel + dir);
            break;

        default:
            if (dir > 0)
                s_focus = CS_FOCUS_ICONS;
            else
                cs_focus_list(last);
            break;
    }
}

static void cs_move_col(int dir)
{
    int c = s_col;
    int i;

    for (i = 0; i < BC_COLS - 1; i++)
    {
        c = (c + dir + BC_COLS) % BC_COLS;
        if (cs_cell_exists(s_list.sel, c))
            break;
    }
    if (cs_cell_exists(s_list.sel, c) && c != s_col)
    {
        s_col     = c;
        s_wantCol = c;
        cs_beep(Sfx_MenuMove);
    }
}

static void cs_activate(void)
{
    char toast[96];
    int  row = s_list.sel;

    if (!BindCore_CellUsable(row, s_col) || (s_col >= BC_PAD1 && !BindInput_PadConnected()))
    {
        cs_beep(Sfx_MenuError);
        return;
    }
    cs_beep(Sfx_MenuConfirm);
    BindCore_FormatRebindToast(row, s_col, toast, sizeof(toast));
    DbgOverlay_ToastLine(toast);
    s_listen = 1;
    BindCapture_Begin(row, s_col);
}

static void cs_clear(void)
{
    if (!BindCore_CellUsable(s_list.sel, s_col))
    {
        cs_beep(Sfx_MenuError);
        return;
    }
    BindCore_Set(s_list.sel, s_col, "NONE");
    cs_beep(Sfx_MenuCancel);
}

static void cs_open_reset(void)
{
    cs_beep(Sfx_MenuConfirm);
    NmConfirm_Open(&s_confirm);
}

static int cs_footer_reset_x(void)
{
    return CS_VALUE_RX - Nm_TextWidth("Reset");
}

static int cs_value_left(void)
{
    return CS_VALUE_RX - Nm_TextWidth(InputGlyphs_SetLabel(g_PcConfig.gamepadIcons));
}

static int cs_in_row(int my, int y)
{
    return my >= y && my < y + CS_ROW_H;
}

static int cs_hit(const NmInput* in, int* outRow, int* outCol)
{
    int i, c;

    *outRow = -1;
    *outCol = -1;
    if (s_confirm.open)
    {
        if (!cs_in_row(in->mouseY, CS_CONFIRM_Y + 24) || in->mouseX < 80 || in->mouseX >= 240)
            return CS_HIT_NONE;
        return (in->mouseX < 160) ? CS_HIT_YES : CS_HIT_NO;
    }
    if (cs_in_row(in->mouseY, CS_ICONS_Y))
        return CS_HIT_ICONS;
    if (cs_in_row(in->mouseY, CS_FOOTER_Y))
    {
        if (in->mouseX >= CS_TEXT_X - NM_BULLET_TEXT_DX - 1 && in->mouseX < CS_TEXT_X + Nm_TextWidth("Back") + 4)
            return CS_HIT_BACK;
        if (in->mouseX >= cs_footer_reset_x() - NM_BULLET_TEXT_DX - 1 && in->mouseX < CS_VALUE_RX + 4)
            return CS_HIT_RESET;
        return CS_HIT_NONE;
    }
    if (in->mouseY < CS_LIST_Y)
        return CS_HIT_NONE;
    i = (in->mouseY - CS_LIST_Y) / CS_PITCH;
    if (i >= s_list.visible || s_list.top + i >= s_list.count)
        return CS_HIT_NONE;
    *outRow = s_list.top + i;
    for (c = 0; c < BC_COLS; c++)
    {
        int x0 = s_colX[c] - 2;
        int x1 = (c + 1 < BC_COLS) ? s_colX[c + 1] - 2 : s_colX[c] + s_colW[c] + 2;

        if (in->mouseX >= x0 && in->mouseX < x1)
            *outCol = c;
    }
    return CS_HIT_ROW;
}

static int cs_close(void)
{
    cs_beep(Sfx_MenuCancel);
    SH_DBG("[BINDS] controls screen closed");
    return 0;
}

/* 0 when a click closed the screen. */
static int cs_mouse(const NmInput* in)
{
    int row, col, hit;

    if (!in->mouseOk || !(in->mouseMoved || in->mouseLeft || in->mouseRight))
        return 1;
    hit = cs_hit(in, &row, &col);
    if (hit != CS_HIT_NONE)
        Nm_HighlightSnap();

    if (in->mouseMoved)
    {
        int oldFocus = s_focus, oldSel = s_list.sel, oldCol = s_col;

        if (hit == CS_HIT_ICONS)
            s_focus = CS_FOCUS_ICONS;
        else if (hit == CS_HIT_BACK)
            s_focus = CS_FOCUS_BACK;
        else if (hit == CS_HIT_RESET)
            s_focus = CS_FOCUS_RESET;
        else if (hit == CS_HIT_ROW)
        {
            if (cs_cell_exists(row, col))
                s_wantCol = col;
            cs_focus_list(row);
        }
        if (s_focus != oldFocus || (s_focus == CS_FOCUS_LIST && (s_list.sel != oldSel || s_col != oldCol)))
            cs_beep(Sfx_MenuMove);
    }

    if (in->mouseLeft)
    {
        if (hit == CS_HIT_ICONS)
        {
            int left = cs_value_left();

            s_focus = CS_FOCUS_ICONS;
            InputGlyphs_Step((in->mouseX < left && in->mouseX >= left - 16) ? -1 : 1);
            cs_beep(Sfx_MenuMove);
        }
        else if (hit == CS_HIT_BACK)
        {
            return cs_close();
        }
        else if (hit == CS_HIT_RESET)
        {
            s_focus = CS_FOCUS_RESET;
            cs_open_reset();
        }
        else if (hit == CS_HIT_ROW && cs_cell_exists(row, col))
        {
            s_wantCol = col;
            cs_focus_list(row);
            cs_activate();
        }
        else if (hit == CS_HIT_ROW && cs_stick_cell(row, col))
        {
            cs_beep(Sfx_MenuError);
        }
    }
    else if (in->mouseRight && hit == CS_HIT_ROW)
    {
        if (cs_cell_exists(row, col))
        {
            s_wantCol = col;
            cs_focus_list(row);
            cs_clear();
        }
        else if (cs_stick_cell(row, col))
        {
            cs_beep(Sfx_MenuError);
        }
    }
    return 1;
}

static void cs_confirm_update(const int pressed[BN_COUNT], const NmInput* mouse)
{
    NmInput         in;
    NmConfirmResult res;
    int             row, col;

    memset(&in, 0, sizeof(in));
    in.left    = pressed[BN_LEFT];
    in.right   = pressed[BN_RIGHT];
    in.confirm = pressed[BN_OK];
    in.cancel  = pressed[BN_BACK];

    if (mouse->mouseOk && (mouse->mouseMoved || mouse->mouseLeft))
    {
        int hit = cs_hit(mouse, &row, &col);

        if (hit == CS_HIT_YES || hit == CS_HIT_NO)
        {
            int yes = (hit == CS_HIT_YES);

            if (s_confirm.yes != yes)
                cs_beep(Sfx_MenuMove);
            Nm_HighlightSnap();
            s_confirm.yes = yes;
            in.confirm |= mouse->mouseLeft;
        }
    }
    if (in.left || in.right)
        cs_beep(Sfx_MenuMove);

    res = NmConfirm_Update(&s_confirm, &in);
    if (res == NM_CONFIRM_YES)
    {
        cs_beep(Sfx_MenuConfirm);
        BindCore_ResetScheme();
    }
    else if (res == NM_CONFIRM_NO)
    {
        cs_beep(Sfx_MenuCancel);
    }
}

void Pc_ControlsScreen_Open(void)
{
    BindCore_SetScheme(g_DebugThirdPersonCam ? 1 : 0);
    NmScroll_Init(&s_list, BindCore_RowCount(), CS_VISIBLE);
    s_focus   = CS_FOCUS_LIST;
    s_col     = BC_KB1;
    s_wantCol = BC_KB1;
    s_listen  = 0;
    memset(&s_confirm, 0, sizeof(s_confirm));
    BindNav_Reset(&s_nav);
    InputGlyphs_Load();
    s_open = 1;
    SH_DBG("[BINDS] controls screen open: %s scheme", BindCore_Scheme() ? "altcam" : "classic");
}

void Pc_ControlsScreen_Close(void)
{
    s_open = 0;
}

int Pc_ControlsScreen_IsOpen(void)
{
    /* Options can be left without a Back (the Select+Start warm reset), which
     * leaves s_open set. */
    return s_open && g_GameWork.gameState == GameState_OptionScreen;
}

int Pc_ControlsScreen_Update(void)
{
    int     pressed[BN_COUNT];
    NmInput in;

    if (!s_open)
        return 0;

    if (s_listen)
    {
        const char* got = BindCapture_Update();

        if (got != NULL)
        {
            s_listen = 0;
            if (got[0] == '\0')
            {
                cs_beep(Sfx_MenuCancel);
            }
            else
            {
                BindCore_Set(s_list.sel, s_col, got);
                cs_beep(Sfx_MenuConfirm);
            }
            /* The captured press must not also navigate. */
            BindNav_Reset(&s_nav);
        }
        return 1;
    }

    BindNav_Poll(&s_nav, pressed);
    NmInput_FromGame(&in);

    if (s_confirm.open)
    {
        cs_confirm_update(pressed, &in);
        return 1;
    }

    if (pressed[BN_BACK])
        return cs_close();

    if (in.wheel != 0)
    {
        NmScroll_Wheel(&s_list, in.wheel);
        if (s_focus == CS_FOCUS_LIST)
            s_col = cs_resolve_col(s_list.sel, s_wantCol);
    }
    if (!cs_mouse(&in))
        return 0;
    if (s_listen || s_confirm.open)
        return 1;

    if (pressed[BN_UP] || pressed[BN_DOWN])
    {
        cs_move_row(pressed[BN_UP] ? -1 : 1);
        cs_beep(Sfx_MenuMove);
    }
    if (pressed[BN_LEFT] || pressed[BN_RIGHT])
    {
        int dir = pressed[BN_LEFT] ? -1 : 1;

        if (s_focus == CS_FOCUS_ICONS)
        {
            InputGlyphs_Step(dir);
            cs_beep(Sfx_MenuMove);
        }
        else if (s_focus == CS_FOCUS_LIST)
        {
            cs_move_col(dir);
        }
        else
        {
            s_focus = (s_focus == CS_FOCUS_BACK) ? CS_FOCUS_RESET : CS_FOCUS_BACK;
            cs_beep(Sfx_MenuMove);
        }
    }

    if (pressed[BN_OK])
    {
        if (s_focus == CS_FOCUS_ICONS)
        {
            InputGlyphs_Step(1);
            cs_beep(Sfx_MenuMove);
        }
        else if (s_focus == CS_FOCUS_LIST)
        {
            cs_activate();
        }
        else if (s_focus == CS_FOCUS_BACK)
        {
            return cs_close();
        }
        else
        {
            cs_open_reset();
        }
    }
    else if (pressed[BN_CLEAR])
    {
        if (s_focus == CS_FOCUS_LIST)
            cs_clear();
        else
            cs_beep(Sfx_MenuError);
    }
    return 1;
}

/* ---- draw ---------------------------------------------------------------- */

/* Localized here rather than by Gfx_StringDraw so a translation too wide for
 * its space is measured, and pans, as drawn. Gold is the selected cell, the
 * only one that pans. */
static void cs_fit_draw(int x, int y, int color, const char* key, int maxW)
{
    Nm_TextDrawMarquee(x, y, NM_TEXT_TALL, color, Pc_LangMenuText(key), maxW, color == StringColorId_Gold);
}

static void cs_glyph_draw(int glyph, int dark, int x, int y, int rgb)
{
    GsOT_TAG* ot   = &g_OrderingTable2[g_ActiveBufferIdx].org[CS_OT_ICON];
    POLY_FT4* poly = (POLY_FT4*)GsOUT_PACKET_P;
    int       tall = (g_GameWork.gsScreenHeight == FRAMEBUFFER_HEIGHT_INTERLACED);
    int       y0   = tall ? (y - 112) * 2 : (y - 112);
    int       y1   = tall ? (y + INPUT_GLYPH_PX - 112) * 2 : (y + INPUT_GLYPH_PX - 112);
    int       u, v, texW, screenW, texH;

    InputGlyphs_UV(glyph, dark, &u, &v, &texW, &screenW);
    texH = texW * INPUT_GLYPH_PX / screenW;

    setPolyFT4(poly);
    setRGB0(poly, (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
    setXY4(poly, x - 160, y0, x - 160 + screenW, y0, x - 160, y1, x - 160 + screenW, y1);
    setUV4(poly, u, v, u + texW, v, u, v + texH, u + texW, v + texH);
    poly->tpage = getTPage(0, 0, INPUT_GLYPHS_VRAM_X, 0);
    poly->clut  = getClut(INPUT_GLYPHS_CLUT_X, INPUT_GLYPHS_CLUT_Y);
    addPrim(ot, poly);
    GsOUT_PACKET_P = (PACKET*)(poly + 1);
}

static void cs_glyph_cell_draw(int glyph, int x, int y, int color)
{
    int rgb;

    switch (color)
    {
        case StringColorId_Red:      rgb = CS_RGB_RED;              break;
        case StringColorId_DarkGrey: rgb = CS_RGB_DIM * 0x010101;   break;
        default:                     rgb = CS_RGB_WHITE * 0x010101; break;
    }
    cs_glyph_draw(glyph, color != StringColorId_Gold, x, y + CS_ICON_DY, rgb);
}

static void cs_pad_draw(int glyph, const char* text, int col, int y, int color)
{
    char name[NM_LINE_MAX];

    if (InputGlyphs_Ready())
    {
        cs_glyph_cell_draw(glyph, s_colX[col], y, color);
        return;
    }
    Nm_SanitizeGlyphs(text, name, sizeof(name), 0);
    cs_fit_draw(s_colX[col], y, color, name, s_colW[col]);
}

/* A Move row's direction, in e_GlyphDir order, or -1. */
static int cs_stick_dir(int row)
{
    static const char* const keys[] = { "key_up", "key_right", "key_down", "key_left" };
    const char* key = BindCore_Row(row)->key[BC_KB1];
    int         i;

    for (i = 0; i < (int)(sizeof(keys) / sizeof(keys[0])); i++)
    {
        if (strcmp(key, keys[i]) == 0)
            return i;
    }
    return -1;
}

/* The left-stick and d-pad glyphs for a Move row's direction. */
static int cs_stick_icon(int row, int col)
{
    return InputGlyphs_ForStick(s_set, cs_stick_dir(row), col != BC_PAD1);
}

static void cs_stick_draw(int row, int col, int y)
{
    char text[8];
    int  dir = cs_stick_dir(row);

    if (dir < 0)
        return;
    snprintf(text, sizeof(text), "%s_%c", (col == BC_PAD1) ? "LS" : "DP", "URDL"[dir]);
    cs_pad_draw(cs_stick_icon(row, col), text, col, y, StringColorId_DarkGrey);
}

static void cs_key_draw(const char* v, int col, int y, int color)
{
    char        label[NM_LINE_MAX], name[NM_LINE_MAX], clean[NM_LINE_MAX];
    int         glyph;
    const char* suffix = InputGlyphs_KeySuffix(v);

    BindInput_KeyLabel(v, label, sizeof(label));
    glyph = InputGlyphs_Ready() ? InputGlyphs_ForKey(label) : -1;
    if (glyph >= 0)
    {
        cs_glyph_cell_draw(glyph, s_colX[col], y, color);
        if (suffix != NULL)
            Nm_TextDraw(s_colX[col] + INPUT_GLYPH_PX + 1, y, NM_TEXT_TALL, color, suffix);
        return;
    }
    BindCore_DisplayKey(label, name, sizeof(name));
    /* The menu font has no glyph for some layout labels (QWERTZ's dead-key
     * cedilla); the US name beats a blank cell. */
    if (Nm_SanitizeGlyphs(name, clean, sizeof(clean), 0) == 0)
    {
        BindCore_DisplayKey(v, name, sizeof(name));
        Nm_SanitizeGlyphs(name, clean, sizeof(clean), 0);
    }
    cs_fit_draw(s_colX[col], y, color, clean, s_colW[col]);
}

static void cs_cell_draw(int row, int col, int y, int selected)
{
    const char* v;
    int         color;

    if (!cs_cell_exists(row, col))
    {
        if (cs_stick_cell(row, col))
            cs_stick_draw(row, col, y);
        return;
    }
    if (selected && s_listen)
    {
        Nm_TextDraw(s_colX[col], y, NM_TEXT_TALL, StringColorId_Gold, "...");
        return;
    }

    if (selected)
        color = StringColorId_Gold;
    else if (!BindCore_RowEnabled(row))
        color = StringColorId_DarkGrey;
    else if (BindCore_IsDuplicate(row, col))
        color = StringColorId_Red;
    else
        color = StringColorId_White;

    v = BindCore_Value(row, col);
    if (BindCore_Unbound(v))
    {
        Nm_TextDraw(s_colX[col], y, NM_TEXT_TALL, color, "-");
        return;
    }
    if (col >= BC_PAD1)
        cs_pad_draw(InputGlyphs_ForPad(s_set, v), v, col, y, color);
    else
        cs_key_draw(v, col, y, color);
}

static void cs_row_draw(int row, int y)
{
    const int   selRow = (s_focus == CS_FOCUS_LIST && row == s_list.sel);
    const int   color  = BindCore_RowEnabled(row) ? StringColorId_White : StringColorId_DarkGrey;
    char        label[NM_LINE_MAX], fit[NM_LINE_MAX];
    const char* shown;
    int         w;
    int         c;

    Nm_SanitizeGlyphs(BindCore_Row(row)->label, label, sizeof(label), 0);
    /* Bare "Map" is the PC Options start-map row's key, which Japanese words
     * as "start map". The leading space makes this row a key of its own and
     * draws nothing in a right-aligned label. */
    if (strcmp(label, "Map") == 0)
        snprintf(label, sizeof(label), "_Map");
    shown = Pc_LangMenuText(label);
    if (!selRow && Nm_FitEllipsis(shown, fit, sizeof(fit), CS_LABEL_RX, Pc_LangMenuTextWidth))
        shown = fit;
    w = Pc_LangMenuTextWidth(shown);
    if (w > CS_LABEL_RX)
        w = CS_LABEL_RX;
    Nm_TextDrawMarquee(CS_LABEL_RX - w, y, NM_TEXT_TALL, color, shown, w, 1);
    Nm_TextDraw(CS_LABEL_RX, y, NM_TEXT_TALL, color, ":");

    for (c = 0; c < BC_COLS; c++)
        cs_cell_draw(row, c, y, selRow && c == s_col);

    if (selRow)
        Nm_HighlightDraw(CS_HL_X0, CS_HL_X1, y + 14);
}

static void cs_bullet_draw(int x, int y, int selected, const char* label)
{
    Nm_BulletDraw(x - NM_BULLET_TEXT_DX, y, selected);
    Nm_TextDraw(x, y, NM_TEXT_TALL, StringColorId_White, label);
    if (selected)
        Nm_HighlightDraw(x - NM_BULLET_TEXT_DX - 1, x + Nm_TextWidth(label) + 4, y + 14);
}

void Pc_ControlsScreen_Draw(void)
{
    int kbSel, padSel, i;

    if (!s_open)
        return;

    Nm_BlackBackgroundDraw();

    if (s_confirm.open)
    {
        NmConfirm_Draw(&s_confirm, "Reset_controls?", CS_CONFIRM_Y);
        return;
    }

    s_set = InputGlyphs_ActiveSet();

    cs_bullet_draw(CS_TEXT_X, CS_ICONS_Y, s_focus == CS_FOCUS_ICONS, "Gamepad_Icons");
    Nm_TextDrawRight(CS_VALUE_RX, CS_ICONS_Y, NM_TEXT_TALL, StringColorId_White,
                     InputGlyphs_SetLabel(g_PcConfig.gamepadIcons));
    Nm_ValueArrowsDraw(cs_value_left(), CS_VALUE_RX, CS_ICONS_Y);

    kbSel  = (s_focus == CS_FOCUS_LIST && s_col <= BC_KB2);
    padSel = (s_focus == CS_FOCUS_LIST && s_col >= BC_PAD1);
    Nm_RuleDraw(CS_RULE_X0, CS_RULE_X1, CS_RULE1_Y, 0);
    Nm_TextDrawCentered(CS_HEAD_FUNC_X, CS_HEAD_Y, NM_TEXT_TALL, StringColorId_LightGrey, "Function");
    Nm_TextDrawCentered((s_colX[BC_KB1] + s_colX[BC_KB2] + s_colW[BC_KB2]) / 2, CS_HEAD_Y, NM_TEXT_TALL,
                        kbSel ? StringColorId_Gold : StringColorId_LightGrey, "Keyboard");
    Nm_TextDrawCentered((s_colX[BC_PAD1] + s_colX[BC_PAD2] + s_colW[BC_PAD2]) / 2, CS_HEAD_Y, NM_TEXT_TALL,
                        padSel ? StringColorId_Gold : StringColorId_LightGrey, "Controller");
    Nm_RuleDraw(CS_RULE_X0, CS_RULE_X1, CS_RULE2_Y, 0);

    for (i = s_list.top; i < s_list.count && i < s_list.top + s_list.visible; i++)
        cs_row_draw(i, CS_LIST_Y + ((i - s_list.top) * CS_PITCH));

    if (s_list.count > s_list.visible)
    {
        Nm_ScrollArrowDraw(CS_ARROW_X, CS_ARROW_UP_Y, 1, NmScroll_CanUp(&s_list));
        Nm_ScrollArrowDraw(CS_ARROW_X, CS_ARROW_DOWN_Y, 0, NmScroll_CanDown(&s_list));
    }

    Nm_RuleDraw(CS_RULE_X0, CS_RULE_X1, CS_RULE3_Y, 0);
    cs_bullet_draw(CS_TEXT_X, CS_FOOTER_Y, s_focus == CS_FOCUS_BACK, "Back");
    cs_bullet_draw(cs_footer_reset_x(), CS_FOOTER_Y, s_focus == CS_FOCUS_RESET, "Reset");
}
