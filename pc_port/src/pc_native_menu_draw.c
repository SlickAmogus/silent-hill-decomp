/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game.h"

#include <string.h>
#include <SDL.h>

#include "bodyprog/bodyprog.h"
#include "bodyprog/events/map_msg.h"
#include "bodyprog/item_screens.h"
#include "bodyprog/screen/screen_data.h"
#include "bodyprog/text/text_draw.h"
#include "screens/options.h"
#include "lang_text.h"
#include "pc_mouse_cursor.h"
#include "pc_native_menu_draw.h"

#define NM_OT_ARROW 10 /* LAYER_40 in options.c */
#define NM_OT_SHADE 9  /* LAYER_36 in options.c */
#define NM_OT_TEXT  8
#define NM_OT_RULE  6  /* LAYER_24 in options.c */
#define NM_OT_BACK  15

#define NM_PAGE_HEADING_DY 32

static int nm_interlaced(void)
{
    return g_GameWork.gsScreenHeight == FRAMEBUFFER_HEIGHT_INTERLACED;
}

static s16 nm_px(int x)
{
    return (s16)(x - 160);
}

static s16 nm_py(int y)
{
    return (s16)(nm_interlaced() ? (y - 112) * 2 : (y - 112));
}

static GsOT_TAG* nm_ot(int tag)
{
    return &g_OrderingTable2[g_ActiveBufferIdx].org[tag];
}

void Nm_TextDraw(int x, int y, int size, int color, const char* s)
{
    Gfx_StringSetColor(color);

    if (nm_interlaced() && size == NM_TEXT_TALL)
    {
        g_SysWork.enableHighResGlyphs = true;
        Gfx_StringSetPosition(x, y);
    }
    else if (nm_interlaced())
    {
        Gfx_StringSetPosition(x, (2 * y) - 112);
    }
    else
    {
        Gfx_StringSetPosition(x, y);
    }
    /* Gfx_StringSetPosition reads x == NO_VALUE as "keep x", and a panning
     * line passes through -1: it drew where the last string ended. */
    if (x == NO_VALUE)
    {
        g_StringPosition.vx = x - SCREEN_POSITION_X(50.0f);
        g_StringPositionX1  = g_StringPosition.vx;
    }

    Gfx_Strings2dLayerIdxSet(NM_OT_TEXT);
    Gfx_StringDraw((char*)s, DEFAULT_MAP_MESSAGE_LENGTH);

    g_SysWork.enableHighResGlyphs = false;
    Gfx_StringsReset2dLayerIdx();
}

void Nm_TextDrawVerbatim(int x, int y, int size, int color, const char* s)
{
    g_PcLangMenuVerbatim = 1;
    Nm_TextDraw(x, y, size, color, s);
    g_PcLangMenuVerbatim = 0;
}

int Nm_TextWidth(const char* s)
{
    return Pc_LangMenuTextWidth(Pc_LangMenuText(s));
}

void Nm_TextDrawCentered(int cx, int y, int size, int color, const char* s)
{
    Nm_TextDraw(cx - (Nm_TextWidth(s) / 2), y, size, color, s);
}

void Nm_TextDrawRight(int rx, int y, int size, int color, const char* s)
{
    Nm_TextDraw(rx - Nm_TextWidth(s), y, size, color, s);
}

/* Wall-clock ms: the pan has to read at one speed whatever the frame rate. */
#define NM_MARQUEE_HOLD_MS 1200
#define NM_MARQUEE_PX_PER_S 36
/* Gfx_StringDraw stops at DEFAULT_MAP_MESSAGE_LENGTH characters. */
#define NM_MARQUEE_CHUNK 96

/* Pans on screen together share one cycle: each runs to its end and holds
 * there until the longest has arrived, then all start over at once. The
 * longest is last frame's, so a text drawn early in a frame still knows it. */
static Uint32 s_marqueeEpoch;
static Uint32 s_marqueeLast;
static Uint32 s_marqueeMove;
static Uint32 s_marqueeMovePrev;
static s32    s_marqueeTick;

static void nm_marquee_restart(void)
{
    s_marqueeEpoch = SDL_GetTicks();
}

static int nm_marquee_offset(int overflow)
{
    const Uint32 now  = SDL_GetTicks();
    const Uint32 move = ((Uint32)overflow * 1000u) / NM_MARQUEE_PX_PER_S;
    Uint32       groupMove;
    Uint32       t;

    if (g_TickCount != s_marqueeTick)
    {
        s_marqueeTick     = g_TickCount;
        s_marqueeMovePrev = s_marqueeMove;
        s_marqueeMove     = 0;
    }
    if (move > s_marqueeMove)
        s_marqueeMove = move;
    groupMove = (s_marqueeMovePrev > s_marqueeMove) ? s_marqueeMovePrev : s_marqueeMove;

    /* A screen that was closed starts its pans over when it reopens. */
    if (now - s_marqueeLast > 250u)
        s_marqueeEpoch = now;
    s_marqueeLast = now;

    if (now - s_marqueeEpoch >= (2u * NM_MARQUEE_HOLD_MS) + groupMove)
        s_marqueeEpoch = now;

    t = now - s_marqueeEpoch;
    if (t < NM_MARQUEE_HOLD_MS)
        return 0;
    t -= NM_MARQUEE_HOLD_MS;
    if (t < move)
        return (int)((t * (Uint32)overflow) / move);
    return overflow;
}

int Nm_TextDrawMarquee(int x, int y, int size, int color, const char* s, int maxW, int pan)
{
    char   chunk[NM_MARQUEE_CHUNK + 1];
    size_t left = strlen(s);
    int    w    = Pc_LangMenuTextWidth(s);
    int    cx;

    if (w > maxW && !pan)
    {
        char fit[NM_LINE_MAX];

        Nm_FitEllipsis(s, fit, sizeof(fit), maxW, Pc_LangMenuTextWidth);
        Nm_TextDrawVerbatim(x, y, size, color, fit);
        return Pc_LangMenuTextWidth(fit);
    }

    if (w <= maxW && left <= NM_MARQUEE_CHUNK)
    {
        Nm_TextDrawVerbatim(x, y, size, color, s);
        return w;
    }

    cx = x;
    if (w > maxW)
    {
        cx -= nm_marquee_offset(w - maxW);
        Gfx_PcStringSetClipX(x, x + maxW);
    }
    while (left > 0)
    {
        size_t n = left;

        if (n > NM_MARQUEE_CHUNK)
        {
            n = NM_MARQUEE_CHUNK;
            while (n > NM_MARQUEE_CHUNK / 2 && s[n - 1] != '_')
                n--;
            if (s[n - 1] != '_')
                n = NM_MARQUEE_CHUNK;
        }
        memcpy(chunk, s, n);
        chunk[n] = '\0';
        if (cx < x + maxW)
            Nm_TextDrawVerbatim(cx, y, size, color, chunk);
        cx   += Pc_LangMenuTextWidth(chunk);
        s    += n;
        left -= n;
    }
    Gfx_PcStringSetClipX(0, 0);
    return (w > maxW) ? maxW : w;
}

void Nm_ScrollArrowDraw(int cx, int y, int up, int enabled)
{
    s_Triangle2d tri;
    int          dir = up ? -1 : 1;
    int          off = 0;
    POLY_G3*     poly;

    if (enabled)
    {
        int phase = g_SysWork.counters_1C[0] & 0x3F;

        off = ((phase < 32) ? phase : (63 - phase)) * 3 / 32;
    }

    tri.vertex0.vx = nm_px(cx);
    tri.vertex0.vy = nm_py(y + (dir * (4 + off)));
    tri.vertex1.vx = nm_px(cx - 8);
    tri.vertex1.vy = nm_py(y - (dir * (4 - off)));
    tri.vertex2.vx = nm_px(cx + 8);
    tri.vertex2.vy = tri.vertex1.vy;

    if (enabled)
    {
        Options_Selection_ArrowDraw(&tri, true, false);
        return;
    }

    poly = (POLY_G3*)GsOUT_PACKET_P;
    setPolyG3(poly);
    /* Twice DarkGrey's 32: text colours modulate the near-white font at 128 = 1.0,
     * this untextured poly shows its colour as is. */
    setRGB0(poly, 64, 64, 64);
    setRGB1(poly, 64, 64, 64);
    setRGB2(poly, 64, 64, 64);
    setXY3(poly, tri.vertex0.vx, tri.vertex0.vy, tri.vertex1.vx, tri.vertex1.vy,
           tri.vertex2.vx, tri.vertex2.vy);
    addPrim(nm_ot(NM_OT_ARROW), poly);
    GsOUT_PACKET_P = (PACKET*)(poly + 1);
}

/* The underline glides to a new row the way the options menus move theirs
 * (Options_*_SelectionHighlightDraw): LINE_CURSOR_TIMER_MAX frames of a sine
 * ease. A screen calls Nm_HighlightSnap when the mouse picked the row, which
 * the options menus also leave unanimated. */
#define NM_GLIDE_FRAMES 8

static struct
{
    int fromX0, fromX1, fromY;
    int toX0, toX1, toY;
    int timer;
    int snap;
    s32 tick;
    int drawn;
} s_glide;

void Nm_HighlightSnap(void)
{
    s_glide.snap = 1;
}

static int nm_glide_lerp(int from, int to, q3_12 alpha)
{
    return from + FP_FROM((to - from) * alpha, Q12_SHIFT);
}

void Nm_HighlightDraw(int x0, int x1, int yBottom)
{
    s_Line2d line;
    q3_12    alpha;

    /* Not drawn last frame: a screen just opened or the underline was hidden,
     * so there is no previous position worth gliding from. */
    if (!s_glide.drawn || s_glide.snap ||
        (g_TickCount != s_glide.tick && g_TickCount != s_glide.tick + 1))
    {
        s_glide.fromX0 = s_glide.toX0 = x0;
        s_glide.fromX1 = s_glide.toX1 = x1;
        s_glide.fromY  = s_glide.toY  = yBottom;
        s_glide.timer  = NM_GLIDE_FRAMES;
        s_glide.snap   = 0;
        s_glide.drawn  = 1;
        nm_marquee_restart();
    }
    else if (x0 != s_glide.toX0 || x1 != s_glide.toX1 || yBottom != s_glide.toY)
    {
        alpha          = Math_Sin(s_glide.timer << 7);
        s_glide.fromX0 = nm_glide_lerp(s_glide.fromX0, s_glide.toX0, alpha);
        s_glide.fromX1 = nm_glide_lerp(s_glide.fromX1, s_glide.toX1, alpha);
        s_glide.fromY  = nm_glide_lerp(s_glide.fromY, s_glide.toY, alpha);
        s_glide.toX0   = x0;
        s_glide.toX1   = x1;
        s_glide.toY    = yBottom;
        s_glide.timer  = 0;
        nm_marquee_restart();
    }

    if (g_TickCount != s_glide.tick)
    {
        s_glide.tick = g_TickCount;
        if (s_glide.timer < NM_GLIDE_FRAMES)
            s_glide.timer++;
    }

    alpha   = Math_Sin(s_glide.timer << 7);
    x0      = nm_glide_lerp(s_glide.fromX0, s_glide.toX0, alpha);
    x1      = nm_glide_lerp(s_glide.fromX1, s_glide.toX1, alpha);
    yBottom = nm_glide_lerp(s_glide.fromY, s_glide.toY, alpha);

    line.vertex0.vx = nm_px(x0);
    line.vertex0.vy = nm_py(yBottom);
    line.vertex1.vx = nm_px(x1);
    line.vertex1.vy = line.vertex0.vy;

    if (!nm_interlaced())
    {
        Options_Selection_HighlightDraw(&line, true, false);
        return;
    }

    /* The helper's shadow is a fixed 16 prim px, half a TALL row on the
     * 448-line screen; same quad, colours and blend, at the doubled height. */
    Options_Selection_HighlightDraw(&line, false, false);
    {
        POLY_G4* poly = (POLY_G4*)GsOUT_PACKET_P;
        s16      top  = nm_py(yBottom - 16);

        setPolyG4(poly);
        setSemiTrans(poly, true);
        setRGB0(poly, 0, 0, 0);
        setRGB1(poly, Q8_COLOR(0.375f), Q8_COLOR(0.375f), Q8_COLOR(0.375f));
        setRGB2(poly, 0, 0, 0);
        setRGB3(poly, Q8_COLOR(0.375f), Q8_COLOR(0.375f), Q8_COLOR(0.375f));
        setXY4(poly, line.vertex0.vx, top, line.vertex0.vx, line.vertex0.vy,
               line.vertex1.vx, top, line.vertex1.vx, line.vertex1.vy);
        addPrim(nm_ot(NM_OT_SHADE), poly);
        GsOUT_PACKET_P = (PACKET*)(poly + 1);

        Gfx_Primitive2dTextureSet(0, 0, NM_OT_SHADE, 2);
    }
}

static void nm_quad(s_Quad2d* q, int x0, int y0, int x1, int y1)
{
    q->vertex0.vx = nm_px(x0); q->vertex0.vy = nm_py(y0);
    q->vertex1.vx = nm_px(x0); q->vertex1.vy = nm_py(y1);
    q->vertex2.vx = nm_px(x1); q->vertex2.vy = nm_py(y0);
    q->vertex3.vx = nm_px(x1); q->vertex3.vy = nm_py(y1);
}

void Nm_BulletDraw(int x, int y, int selected)
{
    s_Quad2d front;
    s_Quad2d border;

    nm_quad(&front, x, y + 1, x + 12, y + 13);
    nm_quad(&border, x - 1, y, x + 13, y + 14);
    Options_Selection_BulletPointDraw(&front, false, !selected);
    Options_Selection_BulletPointDraw(&border, true, !selected);
}

void Nm_RuleDraw(int x0, int x1, int y, int gold)
{
    if (gold)
    {
        LINE_G2* line = (LINE_G2*)GsOUT_PACKET_P;

        setLineG2(line);
        setRGB0(line, 176, 176, 176);
        setRGB1(line, 160, 128, 64);
        setXY2(line, nm_px(x0), nm_py(y), nm_px(x1), nm_py(y));
        addPrim(nm_ot(NM_OT_RULE), line);
        GsOUT_PACKET_P = (PACKET*)(line + 1);
    }
    else
    {
        LINE_F2* line = (LINE_F2*)GsOUT_PACKET_P;

        setLineF2(line);
        setRGB0(line, 128, 128, 128);
        setXY2(line, nm_px(x0), nm_py(y), nm_px(x1), nm_py(y));
        addPrim(nm_ot(NM_OT_RULE), line);
        GsOUT_PACKET_P = (PACKET*)(line + 1);
    }
}

void Nm_ValueArrowsDraw(int xLeft, int xRight, int y)
{
    s_Triangle2d tri;

    tri.vertex0.vx = nm_px(xLeft - 14);
    tri.vertex0.vy = nm_py(y + 6);
    tri.vertex1.vx = nm_px(xLeft - 6);
    tri.vertex1.vy = nm_py(y - 2);
    tri.vertex2.vx = tri.vertex1.vx;
    tri.vertex2.vy = nm_py(y + 14);
    Options_Selection_ArrowDraw(&tri, true, false);

    tri.vertex0.vx = nm_px(xRight + 14);
    tri.vertex1.vx = nm_px(xRight + 6);
    tri.vertex2.vx = tri.vertex1.vx;
    Options_Selection_ArrowDraw(&tri, true, false);
}

void Nm_BlackBackgroundDraw(void)
{
    TILE* tile = (TILE*)GsOUT_PACKET_P;

    /* Wider than 4:3 so a Hor+ widened title screen is covered too. */
    setTile(tile);
    setRGB0(tile, 0, 0, 0);
    setXY0(tile, -480, -240);
    setWH(tile, 960, 480);
    addPrim(nm_ot(NM_OT_BACK), tile);
    GsOUT_PACKET_P = (PACKET*)(tile + 1);
}

void Nm_SpriteDraw(int x, int y, int w, int h, u16 clut, u8 rgb)
{
    POLY_FT4* poly = (POLY_FT4*)GsOUT_PACKET_P;
    s16       x0   = nm_px(x);
    s16       x1   = nm_px(x + w);
    s16       y0   = nm_py(y);
    s16       y1   = nm_py(y + h);

    setPolyFT4(poly);
    setRGB0(poly, rgb, rgb, rgb);
    setXY4(poly, x0, y0, x1, y0, x0, y1, x1, y1);
    setUV4(poly, 0, 0, 255, 0, 0, 255, 255, 255);
    poly->tpage = 0;
    poly->clut  = clut;
    addPrim(nm_ot(NM_OT_TEXT), poly);
    GsOUT_PACKET_P = (PACKET*)(poly + 1);
}

void NmInput_FromGame(NmInput* out)
{
    const s_ControllerConfig* cfg = &g_GameWorkPtr->config.controllerConfig;
    int mx = 0;
    int my = 0;
    /* While the cursor is live the mouse reports its own clicks and wheel below,
     * so its bound pad bits would make one click act twice (see saveload.c).
     * With the cursor off those fields stay 0 and the binds are the only path. */
    int mouseOk = Pc_MouseCursor_UiPos(&mx, &my);
    u32 mouseBits = mouseOk ? Pc_MouseCursor_BoundPadBits() : 0;
    u32 pulsed    = g_Controller0->pulsedBtnFlags & ~mouseBits;
    u32 clicked   = g_Controller0->clickedBtnFlags & ~mouseBits;

    memset(out, 0, sizeof(*out));
    out->up      = (pulsed & ControllerFlag_LStickUp) != 0;
    out->down    = (pulsed & ControllerFlag_LStickDown) != 0;
    out->left    = (pulsed & ControllerFlag_LStickLeft) != 0;
    out->right   = (pulsed & ControllerFlag_LStickRight) != 0;
    out->confirm = (clicked & (cfg->enter | cfg->action)) != 0;
    out->cancel  = (clicked & cfg->cancel) != 0;

    if (mouseOk)
    {
        out->wheel   = Pc_MouseCursor_WheelNotches();
        out->mouseOk = 1;
        out->mouseX  = mx;
        /* UiPos reports the 448-line authoring space (-112..336) on the title. */
        out->mouseY  = nm_interlaced() ? (my + 112) / 2 : my;
    }
    out->mouseMoved = Pc_MouseCursor_Moved();
    out->mouseLeft  = Pc_MouseCursor_LeftClicked();
    out->mouseRight = Pc_MouseCursor_RightClicked();
}

void NmPage_Draw(const NmPage* p, int x, int y, int pitch, int valueX)
{
    const NmScroll* s = &p->scroll;
    int             i;

    if (p->heading != NULL)
    {
        Nm_TextDrawCentered(160, y - NM_PAGE_HEADING_DY, NM_TEXT_TALL, StringColorId_White, p->heading);
    }

    for (i = s->top; i < p->rowCount && i < s->top + s->visible; i++)
    {
        const NmRow* row   = &p->rows[i];
        int          ry    = y + ((i - s->top) * pitch);
        int          sel   = (i == s->sel);
        const char*  value = (row->value != NULL) ? row->value(p->ctx) : NULL;

        if (row->kind == NM_ROW_INFO)
        {
            Nm_TextDraw(x, ry, NM_TEXT_TALL, StringColorId_LightGrey, row->label);
            if (value != NULL)
            {
                Nm_TextDraw(valueX, ry, NM_TEXT_TALL, StringColorId_LightGrey, value);
            }
            continue;
        }

        Nm_BulletDraw(x - NM_BULLET_TEXT_DX, ry, sel);
        Nm_TextDraw(x, ry, NM_TEXT_TALL, StringColorId_White, row->label);

        if (value != NULL)
        {
            Nm_TextDraw(valueX, ry, NM_TEXT_TALL, StringColorId_White, value);
            if (sel && row->kind == NM_ROW_VALUE)
            {
                Nm_ValueArrowsDraw(valueX, valueX + Nm_TextWidth(value), ry);
            }
        }

        if (sel)
        {
            Nm_HighlightDraw(x - NM_BULLET_TEXT_DX - 1, x + Nm_TextWidth(row->label) + 4, ry + 14);
        }
    }

    if (s->count > s->visible)
    {
        Nm_ScrollArrowDraw(160, y - 8, 1, NmScroll_CanUp(s));
        Nm_ScrollArrowDraw(160, y + (s->visible * pitch) + 6, 0, NmScroll_CanDown(s));
    }
}

void NmConfirm_Draw(const NmConfirm* c, const char* question, int cy)
{
    static const char* const LABELS[2] = { "Yes", "No" };
    int                      ry = cy + 24;
    int                      i;

    if (!c->open)
    {
        return;
    }

    Nm_TextDrawCentered(160, cy, NM_TEXT_TALL, StringColorId_White, question);

    for (i = 0; i < 2; i++)
    {
        int sel = (i == 0) ? c->yes : !c->yes;
        int w   = Nm_TextWidth(LABELS[i]);
        int x   = ((i == 0) ? 120 : 200) - (w / 2);

        Nm_TextDraw(x, ry, NM_TEXT_TALL, sel ? StringColorId_Gold : StringColorId_White, LABELS[i]);
        if (sel)
        {
            Nm_HighlightDraw(x - 4, x + w + 4, ry + 14);
        }
    }
}
