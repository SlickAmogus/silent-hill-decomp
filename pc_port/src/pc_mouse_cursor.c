/* PC port: mouse control for cursor-driven puzzles + a clickable main menu.
 * See pc_mouse_cursor.h for the design. */
#include "game.h"
#include "inline_no_dmpsx.h"

#include "bodyprog/bodyprog.h"
#include "bodyprog/sys/joy.h"
#include "pc_config.h"
#include "pc_mouse_cursor.h"

#include "bodyprog/screen/screen_data.h"

#include <SDL.h>
#include <PsyX/PsyX_public.h>
#include <libgs.h>

/* Menu rows are authored via Gfx_StringSetPosition(x, y), a center-origin space
 * whose reference is (OFFSET_X, OFFSET_Y). The overlay OT draw origin is the
 * framebuffer centre (gsScreenWidth/2, gsScreenHeight/2). The framebuffer HEIGHT
 * is 448 on the interlaced title/difficulty menus and 224 in-game, so we read it
 * live (g_GameWork.gsScreenHeight) — a fixed 240/120 was the bug that trapped the
 * cursor in the middle band. Width/OFFSET_X are 320/160 in both modes. */
#define MC_OFFSET_X 160 /* SCREEN_POSITION_X(50) — text authoring X reference */
#define MC_OFFSET_Y 112 /* SCREEN_POSITION_Y(47) — text authoring Y reference */

/* Puzzle cursor servo gain. Each free-cursor puzzle integrates the analog stick
 * as a velocity and clamps its cursor to a fixed range, so a delta-velocity
 * injection only delivered a fraction of that range per full mouse sweep — the
 * cursor stopped ~2/3 of the way down. Instead we deflect the stick proportional
 * to the error between the puzzle's own cursor and the framebuffer point the
 * mouse is over, converging on exactly where the mouse points regardless of the
 * framebuffer height or the stick scale. Gain sets how hard it steers; the
 * mouse-sensitivity slider scales it. */
#define MC_PUZZLE_SERVO_GAIN 8.0f

/* Per-frame pointer state, in logical 320x240 coords. */
static float s_gx, s_gy, s_prevGx, s_prevGy;
static int   s_inView;
static int   s_leftDown, s_prevLeft, s_leftEdge;
static int   s_rightDown, s_prevRight, s_rightEdge;
static int   s_puzzleFrames; /* >0 while a free-cursor puzzle is on screen */
static int   s_havePrev;
static int   s_moved;        /* mouse moved this frame */
static int   s_prevMx = -1, s_prevMy = -1;

/* Puzzle cursor servo state: the game's own cursor position (framebuffer
 * centre-origin px, captured from Gfx_CursorDraw) and whether a recent mouse
 * move has engaged absolute tracking — released once the cursor reaches the
 * pointer, so a real controller keeps the stick while the mouse sits idle. */
static float s_gameCurX, s_gameCurY;
static int   s_haveGameCur;
static int   s_servoActive;
static int   s_wheelStep;    /* +1/-1 on a new wheel notch this frame, else 0 */
static int   s_prevWheelUp, s_prevWheelDown;

static int Mc_Enabled(void)
{
    if (!g_PcConfig.mouseCursor)
        return 0;
    /* Only when the OS cursor is free. In TPS/OTS/FPS the pointer is captured
     * for camera look, so its absolute position is meaningless. */
    if (SDL_GetRelativeMouseMode() == SDL_TRUE)
        return 0;
    /* The quake console owns the mouse while open (its own pointer, click-drag
     * selection) — no hover/click may leak into the menu underneath. */
    {
        extern int Pc_ConsoleIsOpen(void);
        if (Pc_ConsoleIsOpen())
            return 0;
    }
    return 1;
}

void Pc_MouseCursor_NoteCursorDrawn(int curX, int curY)
{
    s_puzzleFrames = 3;
    s_gameCurX     = (float)curX;
    s_gameCurY     = (float)curY;
    s_haveGameCur  = 1;
}

int Pc_MouseCursor_Moved(void)
{
    return s_moved;
}

int Pc_MouseCursor_UiPos(int* outX, int* outY)
{
    if (!Mc_Enabled() || !s_inView)
        return 0;
    if (outX != NULL)
        *outX = (int)s_gx;
    if (outY != NULL)
        *outY = (int)s_gy;
    return 1;
}

int Pc_MouseCursor_LeftClicked(void)
{
    return Mc_Enabled() && s_inView && s_leftEdge;
}

int Pc_MouseCursor_LeftHeld(void)
{
    return Mc_Enabled() && s_inView && s_leftDown;
}

int Pc_MouseCursor_RightClicked(void)
{
    return Mc_Enabled() && s_inView && s_rightEdge;
}

int Pc_MouseCursor_WheelStep(void)
{
    return Mc_Enabled() ? s_wheelStep : 0;
}

int Pc_MouseCursor_PuzzleActive(void)
{
    return s_puzzleFrames > 0;
}

void Pc_MouseCursor_FrameUpdate(void)
{
    int    mx, my;
    Uint32 btn;
    float  fx, fy;

    s_leftEdge = s_rightEdge = 0;

    if (!Mc_Enabled())
    {
        s_prevLeft = s_prevRight = 0;
        s_havePrev  = 0;
        s_wheelStep = 0;
        if (s_puzzleFrames > 0)
            s_puzzleFrames--;
        return;
    }

    /* Wheel: PsyX latches each notch as a 2-frame countdown; a fresh notch is a
     * rise back to 2 (prev<2). Two notches in one frame collapse to one step. */
    {
        extern int g_PsyX_WheelUpFrames, g_PsyX_WheelDownFrames;

        s_wheelStep = 0;
        if (g_PsyX_WheelUpFrames == 2 && s_prevWheelUp < 2)
            s_wheelStep = 1;
        else if (g_PsyX_WheelDownFrames == 2 && s_prevWheelDown < 2)
            s_wheelStep = -1;
        s_prevWheelUp   = g_PsyX_WheelUpFrames;
        s_prevWheelDown = g_PsyX_WheelDownFrames;
    }

    btn         = SDL_GetMouseState(&mx, &my);
    s_moved     = (mx != s_prevMx || my != s_prevMy);
    s_prevMx    = mx;
    s_prevMy    = my;
    s_leftDown  = (btn & SDL_BUTTON(SDL_BUTTON_LEFT))  != 0;
    s_rightDown = (btn & SDL_BUTTON(SDL_BUTTON_RIGHT)) != 0;
    s_leftEdge  = (s_leftDown  && !s_prevLeft);
    s_rightEdge = (s_rightDown && !s_prevRight);
    s_prevLeft  = s_leftDown;
    s_prevRight = s_rightDown;

    s_prevGx = s_gx;
    s_prevGy = s_gy;
    s_inView = PsyX_MapWindowToViewport(mx, my, &fx, &fy);
    /* Mouse -> menu text-authoring space. Height differs by framebuffer mode:
     * 448 (interlaced menus) / 224 (progressive in-game), read live. */
    s_gx = fx * (float)g_GameWork.gsScreenWidth;
    s_gy = fy * (float)g_GameWork.gsScreenHeight
           - (float)(g_GameWork.gsScreenHeight / 2)
           + (float)MC_OFFSET_Y;
    if (!s_havePrev)
    {
        s_prevGx  = s_gx;
        s_prevGy  = s_gy;
        s_havePrev = 1;
    }

    /* ---- Free-cursor puzzle servo (piano / plate / door / map pan) ---- */
    if (s_puzzleFrames > 0)
    {
        float sens = g_PcConfig.mouseSensitivity;
        int   sx = 0, sy = 0;

        if (sens <= 0.0f)
            sens = 1.0f;

        /* A mouse move (re)engages absolute tracking; it releases once the cursor
         * has caught up to the pointer, so an idle mouse won't fight a real pad. */
        if (s_moved)
            s_servoActive = 1;

        if (s_servoActive && s_haveGameCur)
        {
            /* The mouse target and the puzzle's own cursor are both framebuffer
             * centre-origin pixels (the MC_OFFSET reference cancels), so steer the
             * stick by their difference until the cursor reaches the pointer. */
            float ex = (s_gx - (float)MC_OFFSET_X) - s_gameCurX;
            float ey = (s_gy - (float)MC_OFFSET_Y) - s_gameCurY;

            if (ex > -3.0f && ex < 3.0f && ey > -3.0f && ey < 3.0f)
            {
                s_servoActive = 0; /* arrived — hand the stick back to a real pad */
            }
            else
            {
                sx = (int)(ex * MC_PUZZLE_SERVO_GAIN * sens);
                sy = (int)(ey * MC_PUZZLE_SERVO_GAIN * sens);
                if (sx < -127) sx = -127; else if (sx > 127) sx = 127;
                if (sy < -127) sy = -127; else if (sy > 127) sy = 127;
            }
        }

        if (g_Controller0 != NULL)
        {
            /* Harry is control-frozen during these puzzles, so this can't move the
             * player. Only overwrite while steering, so an idle mouse leaves a
             * plugged-in analog stick alone. */
            if (sx != 0) g_Controller0->sticks_24.sticks_0.leftX = (s8)sx;
            if (sy != 0) g_Controller0->sticks_24.sticks_0.leftY = (s8)sy;

            if (s_leftEdge)
                g_Controller0->clickedBtnFlags |= g_GameWorkPtr->config.controllerConfig.enter;
            if (s_rightEdge)
                g_Controller0->clickedBtnFlags |= g_GameWorkPtr->config.controllerConfig.cancel;
        }
        s_puzzleFrames--;
    }
}

int Pc_MouseCursor_MenuRowHover(int baseY, int stepY, int count, unsigned int visibleFlags, int* outClicked)
{
    int i, hovered = -1;

    if (outClicked != NULL)
        *outClicked = 0;

    if (!Mc_Enabled() || !s_inView || stepY <= 0)
        return -1;

    for (i = 0; i < count; i++)
    {
        int top, bot;

        if ((visibleFlags & (1u << i)) == 0)
            continue;

        top = baseY + (i * stepY) - 3; /* small lead-in above the glyph top */
        bot = top + stepY;
        if (s_gy >= (float)top && s_gy < (float)bot)
        {
            hovered = i;
            break;
        }
    }

    if (hovered >= 0 && outClicked != NULL)
        *outClicked = s_leftEdge;

    return hovered;
}

void Pc_MouseCursor_Draw(void)
{
    POLY_FT4* poly;
    s32       cx, cy;

    if (!Mc_Enabled() || !s_inView)
        return;

    cx = (s32)s_gx - MC_OFFSET_X;
    cy = (s32)s_gy - MC_OFFSET_Y;

    /* The game's own arrow pointer — the 32x32 sprite every cursor puzzle passes
     * to Gfx_CursorDraw: BG_ETC.TIM atlas UV(0,64), CLUT (192,0), 4bpp tpage at
     * VRAM (768,0). BG_ETC is uploaded during the boot logos (b_konami.c, or the
     * skip_intros replication in game_main.c) and no region's title art overlaps
     * it (US/JPN title lives at (848,0), PAL at (896,0); PAL's resliced BG_ETC
     * keeps the top half, so the same UVs hold). Drawn at 16x16 like the puzzles
     * do; the arrow tip sits at the sprite's top-left, so no hotspot offset. */
    poly = (POLY_FT4*)GsOUT_PACKET_P;
    setPolyFT4(poly);
    setXY0Fast(poly, cx,      cy);
    setXY1Fast(poly, cx + 16, cy);
    setXY2Fast(poly, cx,      cy + 16);
    setXY3Fast(poly, cx + 16, cy + 16);
    setUVWH(poly, 0, 64, 32, 32);
    poly->clut  = getClut(192, 0);
    poly->tpage = getTPage(0, 0, 768, 0);
    setRGB0(poly, 128, 128, 128);
    setSemiTrans(poly, false);

    /* The menu text draws into g_OrderingTable2 (g_OtTags0), layer 6; add the
     * cursor at layer 4 so it sits on top — same overlay the crosshair uses. */
    AddPrim(&g_OtTags0[g_ActiveBufferIdx][4], poly);
    GsOUT_PACKET_P = (PACKET*)(poly + 1);
}
