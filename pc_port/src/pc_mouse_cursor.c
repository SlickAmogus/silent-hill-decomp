/* SPDX-License-Identifier: GPL-3.0-or-later */
/* PC port: mouse control for cursor-driven puzzles + a clickable main menu.
 * See pc_mouse_cursor.h for the design. */
#include "game.h"
#include "inline_no_dmpsx.h"

#include "bodyprog/bodyprog.h"
#include "bodyprog/sys/joy.h"
#include "pc_config.h"
#include "pc_mouse_cursor.h"
#include "sh_log.h"

#include "bodyprog/screen/screen_data.h"

#include <SDL.h>
#include <PsyX/PsyX_public.h>
#include <PsyX/PsyX_render.h> /* GR_ReadVRAM */
#include <PsyX/PsyX_backend.h>
#include <math.h>
#include "sh_log.h"
#include "pc_confirm_dialog.h"
#include "pc_bind_panel.h"
#include "pc_quick_options.h"
#include <libgs.h>

extern GLuint GR_ScreenFBO(void);

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

/* Free-cursor puzzles on a touchscreen. A tap used to BE a click: SDL reports
 * every finger as a left mouse button too, and the servo below injected the
 * confirm bind from that edge, so a note sounded wherever the cursor happened
 * to be standing on the very frame the finger landed. Tapping a key that
 * should have been silent played one anyway, and the piano was unsolvable
 * without a pad (reported 2026-10-07).
 *
 * Touch now separates aiming from acting: a finger drags the cursor and never
 * clicks by itself. A click comes from a double tap at the cursor, or from the
 * Click button the overlay puts in the corner opposite Back. The double tap is
 * held out of the servo so it cannot nudge the cursor off the spot it is
 * confirming. A mouse is untouched and still clicks directly. */
#define MC_PUZZLE_DTAP_MS  400u  /* second tap has to land within this */
#define MC_PUZZLE_TAP_NEAR 40.0f /* ...and this close to the cursor, in UI px */

static Uint32 s_puzzleTapMs;   /* when the last tap near the cursor landed */
static int    s_puzzleGrab;    /* a finger has hold of the cursor and may drag it */
static int    s_puzzleClickReq;/* the overlay's Click button */

void Pc_MouseCursor_PuzzleClickRequest(void)
{
    s_puzzleClickReq = 1;
}

/* Per-frame pointer state, in logical 320x240 coords. */
static float s_gx, s_gy, s_prevGx, s_prevGy;
static float s_vx, s_vy;   /* normalized 0..1 inside the presented picture */
static int   s_inView;
static int   s_leftDown, s_prevLeft, s_leftEdge;
static int   s_rightDown, s_prevRight, s_rightEdge;
static int   s_puzzleFrames; /* >0 while a free-cursor puzzle is on screen */
static int   s_havePrev;
static int   s_moved;        /* mouse moved this frame */
static int   s_everMoved;    /* ...and whether anything ever has (mobile) */
static int   s_prevMx = -1, s_prevMy = -1;

/* Puzzle cursor servo state: the game's own cursor position (framebuffer
 * centre-origin px, captured from Gfx_CursorDraw) and whether a recent mouse
 * move has engaged absolute tracking — released once the cursor reaches the
 * pointer, so a real controller keeps the stick while the mouse sits idle. */
static float s_gameCurX, s_gameCurY;
static int   s_haveGameCur;
static int   s_servoActive;
static int   s_wheelStep;    /* +1/-1 on a new wheel notch this frame, else 0 */
static int   s_wheelNotches; /* every notch this frame, signed */
static int   s_prevWheelTotal;
static int   s_prevWheelUp, s_prevWheelDown;
static int   s_showReq;     /* a screen asked for the pointer since the last draw */

static GLuint s_glProg, s_glVao, s_glVbo, s_glTex;
static int    s_glState; /* 0 untried, 1 ready, -1 failed */

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

/* The Essential QoL Mods window (pc_qol_mods.c) sits over the title and Options
 * menus, which hit-test the pointer themselves and see each tap a frame before
 * the window does, so a tap on the window also landed on the row beneath it.
 * While it is open only the window may read the pointer, and after it closes the
 * pointer stays withheld until the finger or button that closed it lifts. */
static int s_captured, s_ownerRead;

static int Mc_Captured(void)
{
    return s_captured && !s_ownerRead;
}

void Pc_MouseCursor_OwnerRead(int on)
{
    s_ownerRead = on;
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
    return s_moved && !Mc_Captured();
}

int Pc_MouseCursor_UiPos(int* outX, int* outY)
{
    if (!Mc_Enabled() || !s_inView || Mc_Captured())
        return 0;
    if (outX != NULL)
        *outX = (int)s_gx;
    if (outY != NULL)
        *outY = (int)s_gy;
    return 1;
}

/* Normalized position inside the presented picture. Anything drawing in the
 * post-capture GL hook wants THIS, not UiPos: the authoring space UiPos reports
 * in is the live framebuffer (448 tall for the interlaced menus, 224 in game,
 * and different again on PAL), so reconstructing viewport pixels from it needs
 * knowledge this side does not have. */
int Pc_MouseCursor_ViewportPos(float* outX, float* outY)
{
    if (!Mc_Enabled() || !s_inView || Mc_Captured())
        return 0;
    if (outX != NULL) *outX = s_vx;
    if (outY != NULL) *outY = s_vy;
    return 1;
}

int Pc_MouseCursor_LeftClicked(void)
{
    return Mc_Enabled() && s_inView && !Mc_Captured() && s_leftEdge;
}

int Pc_MouseCursor_LeftHeld(void)
{
    return Mc_Enabled() && s_inView && !Mc_Captured() && s_leftDown;
}

int Pc_MouseCursor_RightClicked(void)
{
    return Mc_Enabled() && s_inView && !Mc_Captured() && s_rightEdge;
}

int Pc_MouseCursor_WheelStep(void)
{
    return Mc_Enabled() && !Mc_Captured() ? s_wheelStep : 0;
}

int Pc_MouseCursor_WheelNotches(void)
{
    return Mc_Enabled() && !Mc_Captured() ? s_wheelNotches : 0;
}

unsigned int Pc_MouseCursor_BoundPadBits(void)
{
    extern unsigned short g_cfg_mouseButtonMask[8];
    extern int            g_PsyX_WheelUpFrames, g_PsyX_WheelDownFrames;
    const Uint32          mb   = SDL_GetMouseState(NULL, NULL);
    unsigned int          bits = 0;
    int                   b;

    for (b = 1; b <= 5; b++)
    {
        if (mb & SDL_BUTTON(b))
            bits |= g_cfg_mouseButtonMask[b];
    }
    if (g_PsyX_WheelUpFrames > 0)   bits |= g_cfg_mouseButtonMask[6];
    if (g_PsyX_WheelDownFrames > 0) bits |= g_cfg_mouseButtonMask[7];
    if (bits & ControllerFlag_DpadUp)    bits |= ControllerFlag_LStickUp;
    if (bits & ControllerFlag_DpadDown)  bits |= ControllerFlag_LStickDown;
    if (bits & ControllerFlag_DpadLeft)  bits |= ControllerFlag_LStickLeft;
    if (bits & ControllerFlag_DpadRight) bits |= ControllerFlag_LStickRight;
    return bits;
}

int Pc_MouseCursor_PuzzleActive(void)
{
    return s_puzzleFrames > 0;
}

/* ---- Touch pointer (see pc_mouse_cursor.h) ---------------------------- */
static int    s_tDown, s_tPrevDown, s_tPressEdge, s_tReleaseEdge, s_tInView;
static float  s_tgx, s_tgy;
static float  s_tvx, s_tvy;
static Uint32 s_tLastMs;

#define MC_TOUCH_RECENT_MS 3000

static void Mc_TouchClear(void)
{
    s_tDown = s_tPrevDown = s_tPressEdge = s_tReleaseEdge = 0;
}

static void Mc_TouchUpdate(void)
{
    int   nDev  = SDL_GetNumTouchDevices();
    int   found = 0, d;
    float nx = 0.0f, ny = 0.0f;

    /* Every device, as pc_touch.c does: the touchscreen is not necessarily
     * device 0. The first finger found is the pointer. */
    for (d = 0; d < nDev && !found; d++)
    {
        SDL_TouchID dev = SDL_GetTouchDevice(d);

        if (SDL_GetNumTouchFingers(dev) > 0)
        {
            SDL_Finger* f = SDL_GetTouchFinger(dev, 0);

            if (f != NULL)
            {
                nx    = f->x;
                ny    = f->y;
                found = 1;
            }
        }
    }

    s_tPrevDown    = s_tDown;
    s_tDown        = found;
    s_tPressEdge   = (s_tDown && !s_tPrevDown);
    s_tReleaseEdge = (!s_tDown && s_tPrevDown);

    if (found)
    {
        /* The mapping pc_touch.c uses for its buttons, which land where they
         * are drawn: window-normalised finger -> drawable pixels -> the
         * presented picture -> the same authoring space as s_gx/s_gy. */
        int   winW = 0, winH = 0;
        float vx = 0.0f, vy = 0.0f;

        s_tLastMs = SDL_GetTicks();
        PsyX_GetScreenSize(&winW, &winH);
        s_tInView = PsyX_MapWindowToViewport((int)(nx * (float)winW),
                                             (int)(ny * (float)winH), &vx, &vy);
        if (s_tInView)
        {
            s_tvx = vx;
            s_tvy = vy;
            s_tgx = vx * (float)g_GameWork.gsScreenWidth;
            s_tgy = vy * (float)g_GameWork.gsScreenHeight
                    - (float)(g_GameWork.gsScreenHeight / 2)
                    + (float)MC_OFFSET_Y;
        }

        /* One line per touch, not per frame: where the finger landed at every
         * stage of the mapping, beside what the synthesised mouse reports, so
         * a pointer that still lands in the wrong place is a single log away
         * from its cause. */
        if (s_tPressEdge)
        {
            SH_DBG("[TOUCHPTR] down n=(%.3f,%.3f) px=(%d,%d) of %dx%d view=(%.3f,%.3f) "
                   "in=%d ui=(%d,%d) fb=%dx%d mouse=(%d,%d)",
                   nx, ny, (int)(nx * (float)winW), (int)(ny * (float)winH), winW, winH,
                   vx, vy, s_tInView, (int)s_tgx, (int)s_tgy,
                   (int)g_GameWork.gsScreenWidth, (int)g_GameWork.gsScreenHeight,
                   (int)s_gx, (int)s_gy);
        }
    }
}

int Pc_MouseCursor_TouchDriving(void)
{
    if (!Mc_Enabled() || s_tLastMs == 0)
        return 0;

    return s_tDown || (SDL_GetTicks() - s_tLastMs) < MC_TOUCH_RECENT_MS;
}

int Pc_MouseCursor_TouchDown(int* outX, int* outY)
{
    if (!Mc_Enabled() || Mc_Captured() || !s_tDown || !s_tInView)
        return 0;
    if (outX != NULL) *outX = (int)s_tgx;
    if (outY != NULL) *outY = (int)s_tgy;
    return 1;
}

int Pc_MouseCursor_TouchViewportPos(float* outX, float* outY)
{
    if (!Mc_Enabled() || Mc_Captured() || !(s_tDown || s_tReleaseEdge) || !s_tInView)
        return 0;
    if (outX != NULL) *outX = s_tvx;
    if (outY != NULL) *outY = s_tvy;
    return 1;
}

int Pc_MouseCursor_TouchPressed(void)
{
    return Mc_Enabled() && !Mc_Captured() && s_tPressEdge && s_tInView;
}

int Pc_MouseCursor_TouchReleased(int* outX, int* outY)
{
    if (!Mc_Enabled() || Mc_Captured() || !s_tReleaseEdge || !s_tInView)
        return 0;
    if (outX != NULL) *outX = (int)s_tgx;
    if (outY != NULL) *outY = (int)s_tgy;
    return 1;
}

void Pc_MouseCursor_FrameUpdate(void)
{
    int    mx, my, leftUp;
    Uint32 btn;
    float  fx, fy;

    s_leftEdge = s_rightEdge = 0;

    if (!Mc_Enabled())
    {
        s_prevLeft = s_prevRight = 0;
        s_havePrev  = 0;
        s_wheelStep = 0;
        Mc_TouchClear();
        {
            /* Notches spun while the cursor was off must not land as one
             * burst when it comes back. */
            extern int g_PsyX_WheelTotal;
            s_wheelNotches   = 0;
            s_prevWheelTotal = g_PsyX_WheelTotal;
        }
        if (s_puzzleFrames > 0)
            s_puzzleFrames--;
        return;
    }

    /* Wheel: PsyX latches each notch as a 2-frame countdown; a fresh notch is a
     * rise back to 2 (prev<2). Two notches in one frame collapse to one step. */
    {
        extern int g_PsyX_WheelUpFrames, g_PsyX_WheelDownFrames;
        extern int g_PsyX_WheelTotal;

        s_wheelNotches   = g_PsyX_WheelTotal - s_prevWheelTotal;
        s_prevWheelTotal = g_PsyX_WheelTotal;
        s_wheelStep = 0;
        if (g_PsyX_WheelUpFrames == 2 && s_prevWheelUp < 2)
            s_wheelStep = 1;
        else if (g_PsyX_WheelDownFrames == 2 && s_prevWheelDown < 2)
            s_wheelStep = -1;
        s_prevWheelUp   = g_PsyX_WheelUpFrames;
        s_prevWheelDown = g_PsyX_WheelDownFrames;
    }

    btn         = SDL_GetMouseState(&mx, &my);
    /* SDL hands these back in window points, including the ones it
     * synthesises from touch. PsyX_MapWindowToViewport reasons in drawable
     * pixels, so on a high-DPI iPhone an unscaled pointer lands at a third
     * of its true position -- touch the centre of the menu, the cursor
     * appears in the top-left corner. No-op everywhere else. */
    PsyX_ScaleWindowToDrawable(&mx, &my);
    s_moved     = (mx != s_prevMx || my != s_prevMy);
    s_everMoved = s_everMoved || s_moved;
    s_prevMx    = mx;
    s_prevMy    = my;
    s_leftDown  = (btn & SDL_BUTTON(SDL_BUTTON_LEFT))  != 0;
    s_rightDown = (btn & SDL_BUTTON(SDL_BUTTON_RIGHT)) != 0;
    s_leftEdge  = (s_leftDown  && !s_prevLeft);
    s_rightEdge = (s_rightDown && !s_prevRight);
    leftUp      = (!s_leftDown && s_prevLeft);
    s_prevLeft  = s_leftDown;
    s_prevRight = s_rightDown;

    s_prevGx = s_gx;
    s_prevGy = s_gy;
    s_inView = PsyX_MapWindowToViewport(mx, my, &fx, &fy);
    s_vx = fx;
    s_vy = fy;
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

    Mc_TouchUpdate();

    {
        extern int Pc_QolMods_IsOpen(void);
        if (Pc_QolMods_IsOpen())
            s_captured = 1;
        else if (!s_leftDown && !leftUp && !s_tDown && !s_tReleaseEdge)
            s_captured = 0;
    }

    /* ---- Free-cursor puzzle servo (piano / plate / door / map pan) ---- */
    if (s_puzzleFrames > 0)
    {
        float     sens  = g_PcConfig.mouseSensitivity;
        int       sx = 0, sy = 0;
        const int touch = Pc_MouseCursor_TouchDriving();
        int       click = 0, cancel = 0;

        if (sens <= 0.0f)
            sens = 1.0f;

        if (touch)
        {
            int tx, ty;

            if (Pc_MouseCursor_TouchPressed() && Pc_MouseCursor_TouchDown(&tx, &ty))
            {
                const float  dx    = (float)tx - (float)MC_OFFSET_X - s_gameCurX;
                const float  dy    = (float)ty - (float)MC_OFFSET_Y - s_gameCurY;
                const Uint32 now   = SDL_GetTicks();
                const int    atCur = ((dx * dx) + (dy * dy)) <=
                                     (MC_PUZZLE_TAP_NEAR * MC_PUZZLE_TAP_NEAR);

                if (atCur && s_puzzleTapMs != 0 && (now - s_puzzleTapMs) <= MC_PUZZLE_DTAP_MS)
                {
                    /* Second tap on the cursor: confirm, and take no hold, so
                     * the cursor cannot be nudged off the spot it is about to
                     * act on. */
                    click         = 1;
                    s_puzzleTapMs = 0;
                    s_puzzleGrab  = 0;
                }
                else
                {
                    /* A press only takes hold of the cursor when it lands
                     * within reach of it. Anywhere else does nothing at all. */
                    s_puzzleTapMs = atCur ? now : 0;
                    s_puzzleGrab  = atCur;
                }
            }
            if (!s_tDown)
                s_puzzleGrab = 0;
            if (s_puzzleClickReq)
            {
                click            = 1;
                s_puzzleClickReq = 0;
            }
        }
        else
        {
            click  = s_leftEdge;
            cancel = s_rightEdge;
        }

        /* A mouse move (re)engages absolute tracking; it releases once the cursor
         * has caught up to the pointer, so an idle mouse won't fight a real pad. */
        if (s_moved)
            s_servoActive = 1;

        /* Touch never chases the pointer. SDL puts the synthetic mouse wherever
         * the finger lands, so the line above would have the cursor run at any
         * touch at all: pressing the Click button in the corner sent it skidding
         * into the screen edge, fighting a target it could not reach. It moves
         * only while a finger has hold of it. */
        if (touch)
            s_servoActive = s_puzzleGrab;

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

            if (click)
                g_Controller0->clickedBtnFlags |= g_GameWorkPtr->config.controllerConfig.enter;
            if (cancel)
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

    if (!Mc_Enabled() || !s_inView || Mc_Captured() || stepY <= 0)
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

/* Only the picture hides: hover and clicks stay live, and any real mouse use
 * clears the flag before they could matter. */
static int Mc_HiddenForPad(void)
{
    extern int g_PsyX_PadInputLast;
    extern int Pc_Touch_UsedRecently(void);

    /* On the glass this cursor IS the pointer -- a finger drags it (see the
     * touch pointer block above). PsyCross clears PadInputLast on mouse use
     * but deliberately skips SDL's touch-synthesized mouse events, so one
     * press on a Bluetooth pad would hide the finger cursor for the rest of
     * the run, with no mouse on a phone able to bring it back. Touch within
     * the last three seconds therefore owns the picture; stop touching and a
     * pad hides it again, exactly as a pad does beside a desktop mouse. */
    if (Pc_Touch_UsedRecently() || Pc_MouseCursor_TouchDriving())
        return 0;

    return g_PsyX_PadInputLast;
}

void Pc_MouseCursor_Show(void)
{
    s_showReq = 1;
}

/* The game's own arrow pointer -- the 32x32 sprite every cursor puzzle passes
 * to Gfx_CursorDraw: BG_ETC.TIM atlas UV(0,64), CLUT (192,0), 4bpp tpage at
 * VRAM (768,0). BG_ETC is uploaded during the boot logos (b_konami.c, or the
 * skip_intros replication in game_main.c) and no region's title art overlaps
 * it (US/JPN title lives at (848,0), PAL at (896,0); PAL's resliced BG_ETC
 * keeps the top half, so the same UVs hold). The arrow tip sits at the
 * sprite's top-left, so no hotspot offset. */
static int Mc_SpriteRgba(unsigned char* out)
{
    /* UV (0,64) -> halfword column 768, row 64, 8 halfwords per row; 16-entry
     * CLUT at halfword (192,0). Texel 0x0000 is transparent on the PSX. */
    unsigned short px[32 * 8];
    unsigned short clut[16];
    int x, y;

    GR_ReadVRAM(px, 768, 64, 8, 32);
    GR_ReadVRAM(clut, 192, 0, 16, 1);

    /* BG_ETC not uploaded yet: an all-zero CLUT would decode to nothing. */
    for (x = 0; x < 16; x++)
        if (clut[x] != 0)
            break;
    if (x == 16)
        return 0;

    for (y = 0; y < 32; y++) {
        for (x = 0; x < 32; x++) {
            unsigned short w = px[y * 8 + (x >> 2)];
            unsigned short c = clut[(w >> ((x & 3) * 4)) & 0xF];
            unsigned char* o = out + (y * 32 + x) * 4;
            o[0] = (unsigned char)((c & 0x1F) << 3);
            o[1] = (unsigned char)(((c >> 5) & 0x1F) << 3);
            o[2] = (unsigned char)(((c >> 10) & 0x1F) << 3);
            o[3] = (c == 0) ? 0 : 255;
        }
    }
    return 1;
}

static GLuint Mc_GlShader(GLenum type, const char* src)
{
    GLuint      sh       = glCreateShader(type);
    GLint       ok       = 0;
    const char* parts[2] = { PsyX_Shader_Preamble(type == GL_FRAGMENT_SHADER, PSYX_GLSL_LEGACY), src };

    glShaderSource(sh, 2, parts, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[512];
        glGetShaderInfoLog(sh, (GLsizei)sizeof(log), NULL, log);
        SH_DBG("[MOUSE] cursor shader failed: %s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

/* Leaves its program, VAO and buffer bound; Pc_MouseCursor_GlDraw restores them. */
static void Mc_GlInit(void)
{
    static const char* vs_src =
        "attribute vec2 a_pos;\n"
        "attribute vec2 a_uv;\n"
        "varying vec2 v_uv;\n"
        "void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
    static const char* fs_src =
        "#ifdef GL_ES\n"
        "precision mediump float;\n"
        "#endif\n"
        "varying vec2 v_uv;\n"
        "uniform sampler2D u_tex;\n"
        "void main() { gl_FragColor = texture2D(u_tex, v_uv); }\n";
    static const float quad[6][4] = {
        { -1.0f,  1.0f, 0.0f, 0.0f }, { -1.0f, -1.0f, 0.0f, 1.0f }, {  1.0f,  1.0f, 1.0f, 0.0f },
        {  1.0f,  1.0f, 1.0f, 0.0f }, { -1.0f, -1.0f, 0.0f, 1.0f }, {  1.0f, -1.0f, 1.0f, 1.0f },
    };
    GLuint vs, fs;
    GLint  ok = 0;

    s_glState = -1;
    vs = Mc_GlShader(GL_VERTEX_SHADER, vs_src);
    fs = Mc_GlShader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs)
    {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return;
    }

    s_glProg = glCreateProgram();
    glAttachShader(s_glProg, vs);
    glAttachShader(s_glProg, fs);
    glBindAttribLocation(s_glProg, 0, "a_pos");
    glBindAttribLocation(s_glProg, 1, "a_uv");
    glLinkProgram(s_glProg);
    glGetProgramiv(s_glProg, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok)
    {
        char log[512];
        glGetProgramInfoLog(s_glProg, (GLsizei)sizeof(log), NULL, log);
        SH_DBG("[MOUSE] cursor link failed: %s", log);
        glDeleteProgram(s_glProg);
        s_glProg = 0;
        return;
    }
    glUseProgram(s_glProg);
    glUniform1i(glGetUniformLocation(s_glProg, "u_tex"), 0);

    glGenVertexArrays(1, &s_glVao);
    glBindVertexArray(s_glVao);
    glGenBuffers(1, &s_glVbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_glVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    s_glState = 1;
}

void Pc_MouseCursor_GlDraw(void)
{
    GLint     pic[4], prevVp[4];
    GLint     prevProg = 0, prevVao = 0, prevBuf = 0, prevTex = 0, prevUnit = GL_TEXTURE0, prevAlign = 4;
    GLint     prevDrawFb = 0, prevReadFb = 0;
    GLint     prevSrcRgb = GL_ONE, prevDstRgb = GL_ZERO, prevSrcA = GL_ONE, prevDstA = GL_ZERO;
    GLint     prevEqRgb = GL_FUNC_ADD, prevEqA = GL_FUNC_ADD;
    GLboolean prevBlend, prevDepth, prevCull, prevScissor;
    int       show, scale, size, left, top;

    show      = s_showReq || Pc_QuickOptions_IsOpen() || Pc_ConfirmDialog_IsOpen() || Pc_BindPanel_IsOpen();
    s_showReq = 0;
    if (!show || !Mc_Enabled() || !s_inView || Mc_HiddenForPad() || s_glState < 0)
        return;
#if defined(__ANDROID__) || defined(SH_IOS)
    /* A phone's pointer starts parked at 0,0 and stays there unless a mouse is
     * plugged in or a finger takes hold of the cursor (SDL synthesizes mouse
     * motion from a tap), so nothing is drawn until something has moved it. */
    if (!s_everMoved)
        return;
#endif

    PsyX_GetDisplayViewport(&pic[0], &pic[1], &pic[2], &pic[3]);
    if (pic[2] <= 0 || pic[3] <= 0)
        return;

    /* Whole screen pixels per texel, from the picture height alone: the sprite
     * stays square and evenly sampled in every display mode. Half the PSX's
     * ~1/15 of the picture height, which read as a 96px arrow at 1440p. */
    scale = (int)floor((double)pic[3] / 960.0 + 0.5);
    if (scale < 1)
        scale = 1;
    size = 32 * scale;
    left = (int)floorf((float)pic[0] + s_vx * (float)pic[2]);
    top  = (int)floorf((float)pic[1] + (1.0f - s_vy) * (float)pic[3]);

    glGetIntegerv(GL_VIEWPORT, prevVp);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDrawFb);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFb);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevUnit);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevAlign);
    glGetIntegerv(GL_BLEND_SRC_RGB, &prevSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &prevDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &prevSrcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &prevDstA);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &prevEqRgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &prevEqA);
    prevBlend   = glIsEnabled(GL_BLEND);
    prevDepth   = glIsEnabled(GL_DEPTH_TEST);
    prevCull    = glIsEnabled(GL_CULL_FACE);
    prevScissor = glIsEnabled(GL_SCISSOR_TEST);

    if (s_glState == 0)
        Mc_GlInit();
    if (s_glState != 1)
        goto restore;

    /* BG_ETC reaches VRAM during the boot logos, after the first frames. */
    if (!s_glTex)
    {
        unsigned char rgba[32 * 32 * 4];

        if (!Mc_SpriteRgba(rgba))
            goto restore;
        glGenTextures(1, &s_glTex);
        glBindTexture(GL_TEXTURE_2D, s_glTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }

    /* Same target as the debug overlay: in borderless, framebuffer 0 is
     * painted over by the scene target's blit at present. */
    glBindFramebuffer(GL_FRAMEBUFFER, GR_ScreenFBO());
    /* The viewport is the sprite rect itself, so the pillarbox never clips it
     * and every texel covers exactly scale x scale pixels. */
    glViewport(left, top - size, size, size);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(s_glProg);
    glBindVertexArray(s_glVao);
    glBindTexture(GL_TEXTURE_2D, s_glTex);
    glDrawArrays(GL_TRIANGLES, 0, 6);

restore:
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prevDrawFb);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prevReadFb);
    glViewport(prevVp[0], prevVp[1], prevVp[2], prevVp[3]);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    glActiveTexture((GLenum)prevUnit);
    glPixelStorei(GL_UNPACK_ALIGNMENT, prevAlign);
    glBlendEquationSeparate((GLenum)prevEqRgb, (GLenum)prevEqA);
    glBlendFuncSeparate((GLenum)prevSrcRgb, (GLenum)prevDstRgb, (GLenum)prevSrcA, (GLenum)prevDstA);
    if (!prevBlend)  glDisable(GL_BLEND);
    if (prevDepth)   glEnable(GL_DEPTH_TEST);
    if (prevCull)    glEnable(GL_CULL_FACE);
    if (prevScissor) glEnable(GL_SCISSOR_TEST);
    glUseProgram((GLuint)prevProg);
    glBindVertexArray((GLuint)prevVao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevBuf);
}
