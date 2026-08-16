/* SPDX-License-Identifier: GPL-3.0-or-later */
/* PC port: touchscreen controls. See pc_touch.h for the scheme.
 *
 * Fingers are POLLED rather than taken from SDL events. SDL keeps live touch
 * state (SDL_GetTouchFinger) updated by the pump the frame loop already runs,
 * so polling needs no hook in PsyCross's event loop and cannot get out of step
 * with it. Finger IDs are stable for the life of a contact, which is all the
 * roles below need.
 *
 * Positions are carried in VIEWPORT space (0..1 across the presented picture,
 * via PsyX_MapWindowToViewport) rather than window space, so letterboxing and
 * pillarboxing cannot put a control somewhere the picture is not, and the same
 * numbers drive both hit-testing and drawing.
 */
#include "game.h"
#include "pc_config.h"
#include "pc_touch.h"

#include <libetc.h>
#include <libgs.h>
#include <SDL.h>
#include <PsyX/PsyX_public.h>

#include "bodyprog/screen/screen_data.h"
#include "bodyprog/sys/joy.h"

#define TC_MAX_FINGERS 8

/* Roles a contact can take, decided once when it lands and held until release:
 * a thumb that starts on the movement side must keep steering even if it slides
 * across the middle. */
enum { TR_NONE = 0, TR_MOVE, TR_LOOK, TR_BUTTON };

/* Actions the on-screen buttons drive. Indices into s_Buttons. */
enum { TB_AIM = 0, TB_ITEM, TB_MAP, TB_START, TB_COUNT };

typedef struct
{
    float cx, cy;   /* centre, viewport space */
    float r;        /* radius in HEIGHT units (x is scaled by aspect) */
    int   held;     /* 1 while a finger is on it */
    int   holdFrames; /* minimum-press latch, see TC_BUTTON_MIN_FRAMES */
} s_TouchButton;

/* Fingers are sampled once per pad update, so a very brief contact could be
 * pressed and released between two samples and register as nothing at all.
 * Latch a floor on the press instead: any contact that is seen even once
 * produces a press the game cannot miss the edge of. Costs a few frames of
 * extra hold on release, which is imperceptible on Aim and irrelevant on the
 * rest. (A contact shorter than a single frame is still lost, but that is far
 * below what a thumb can do -- it took an injected 10ms synthetic tap to
 * produce one.) */
#define TC_BUTTON_MIN_FRAMES 3

/* Bottom-right cluster for the two combat-adjacent actions, with Item and Map
 * above them and Start out of the way in the corner. Right-handed layout: the
 * looking thumb is already on this side. */
static s_TouchButton s_Buttons[TB_COUNT] = {
    /* TB_AIM   */ { 0.905f, 0.760f, 0.105f, 0 },
    /* TB_ITEM  */ { 0.760f, 0.830f, 0.070f, 0 },
    /* TB_MAP   */ { 0.905f, 0.510f, 0.070f, 0 },
    /* TB_START */ { 0.955f, 0.075f, 0.055f, 0 },
};

typedef struct
{
    SDL_TouchID  dev;   /* a finger id is only unique WITHIN its device */
    SDL_FingerID id;
    int   active;
    int   role;
    int   buttonIdx;
    float x, y;             /* current, viewport space */
    float startX, startY;
    float originX, originY; /* movement stick origin (floating) */
    float lastX, lastY;     /* previous frame, for look deltas */
    Uint32 startMs;
    int   movedFar;         /* travelled beyond the tap slop */
} s_TouchFinger;

static s_TouchFinger s_Fingers[TC_MAX_FINGERS];

/* Built state, read by Pc_Touch_GetPad and the draw. */
static unsigned short s_PadWord = 0xFFFF;
static unsigned char  s_LeftX = 128, s_LeftY = 128, s_RightX = 128, s_RightY = 128;
static int            s_StickActive;
static float          s_StickOx, s_StickOy, s_StickKx, s_StickKy;
static int            s_Running;
static int            s_ActionFrames;  /* tap pulse, in pad updates */
static Uint32         s_LastTouchMs;

/* Movement stick geometry, in height units. The radius is a thumb's comfortable
 * travel, not a screen fraction that would balloon on a tablet. */
#define TC_STICK_RADIUS   0.150f
#define TC_STICK_DEADZONE 0.150f  /* fraction of the radius */
#define TC_RUN_THRESHOLD  0.850f  /* deflection past this also holds Run */

/* A contact is a tap if it is released quickly without travelling far. The slop
 * is generous: thumbs roll, and a tap that gets misread as a drag reads to the
 * player as an input that did nothing. */
#define TC_TAP_MS        260
#define TC_TAP_SLOP      0.035f
#define TC_ACTION_FRAMES 3        /* hold Action long enough to survive an edge test */

/* Full right-stick deflection for a drag crossing this much of the picture in
 * one update. Small enough that a flick whips the camera, large enough that a
 * slow drag creeps. Scaled by the look-sensitivity setting. */
#define TC_LOOK_SPAN 0.085f

#define TC_LEFT_ZONE 0.45f /* left of this (and not on a button) steers */

static float Tc_Aspect(void)
{
    int sw = 0, sh = 0;

    PsyX_GetScreenSize(&sw, &sh);
    if (sw <= 0 || sh <= 0)
        return 4.0f / 3.0f;

    return (float)sw / (float)sh;
}

static float Tc_LookGain(void)
{
    float s = g_PcConfig.touchLookSensitivity;

    if (s <= 0.0f)
        s = 1.0f;

    return s;
}

/* Settled gameplay only. Everywhere else -- menus, inventory, cutscenes, the
 * map screen -- touch already works as a pointer through pc_mouse_cursor, and
 * injecting a pad on top of that would fight it. */
static int Tc_InGameplay(void)
{
    if (g_GameWork.gameState != GameState_InGame)
        return 0;
    if (g_SysWork.sysState != SysState_Gameplay)
        return 0;

    return 1;
}

static int Tc_Enabled(void)
{
    return g_PcConfig.touchControls != 0;
}

static unsigned char Tc_AxisByte(float v)
{
    int b;

    if (v < -1.0f) v = -1.0f;
    if (v >  1.0f) v =  1.0f;

    b = 128 + (int)(v * 127.0f);
    if (b < 0)   b = 0;
    if (b > 255) b = 255;

    return (unsigned char)b;
}

/* Press whatever the player has bound to an action. Reading the live config
 * rather than hardcoding Cross/Square means a rebound pad keeps working, and
 * the region/difficulty defaults come along for free. */
static void Tc_PressAction(unsigned short* word, unsigned short mask)
{
    if (mask != 0)
        *word &= (unsigned short)~mask;
}

static int Tc_HitButton(float x, float y, float aspect)
{
    int i;

    for (i = 0; i < TB_COUNT; i++)
    {
        float dx = (x - s_Buttons[i].cx) * aspect;
        float dy = (y - s_Buttons[i].cy);
        float r  = s_Buttons[i].r;

        /* Hit radius is padded over the drawn radius: a control you can see is
         * one players expect to hit near the edge of, and fingers are wide. */
        r *= 1.25f;

        if ((dx * dx) + (dy * dy) <= (r * r))
            return i;
    }

    return -1;
}

static s_TouchFinger* Tc_FindFinger(SDL_TouchID dev, SDL_FingerID id)
{
    int i;

    for (i = 0; i < TC_MAX_FINGERS; i++)
    {
        if (s_Fingers[i].active && s_Fingers[i].id == id && s_Fingers[i].dev == dev)
            return &s_Fingers[i];
    }

    return NULL;
}

static s_TouchFinger* Tc_NewFinger(void)
{
    int i;

    for (i = 0; i < TC_MAX_FINGERS; i++)
    {
        if (!s_Fingers[i].active)
            return &s_Fingers[i];
    }

    return NULL;
}

static void Tc_Reset(void)
{
    int i;

    for (i = 0; i < TC_MAX_FINGERS; i++)
        s_Fingers[i].active = 0;
    for (i = 0; i < TB_COUNT; i++)
    {
        s_Buttons[i].held       = 0;
        s_Buttons[i].holdFrames = 0;
    }

    s_PadWord     = 0xFFFF;
    s_LeftX = s_LeftY = s_RightX = s_RightY = 128;
    s_StickActive = 0;
    s_Running     = 0;
}

void Pc_Touch_Update(void)
{
    SDL_TouchID dev;
    Uint32      now;
    float       aspect;
    int         nDev, nFingers, i, d;
    int         seen[TC_MAX_FINGERS];
    float       lookDx = 0.0f, lookDy = 0.0f;
    int         winW = 0, winH = 0;

    if (!Tc_Enabled())
    {
        Tc_Reset();
        return;
    }

    nDev = SDL_GetNumTouchDevices();
    if (nDev <= 0)
    {
        Tc_Reset();
        return;
    }

    now    = SDL_GetTicks();
    aspect = Tc_Aspect();
    PsyX_GetScreenSize(&winW, &winH);

    for (i = 0; i < TC_MAX_FINGERS; i++)
        seen[i] = 0;

    s_PadWord = 0xFFFF;
    for (i = 0; i < TB_COUNT; i++)
    {
        s_Buttons[i].held = 0;
        if (s_Buttons[i].holdFrames > 0)
            s_Buttons[i].holdFrames--;
    }

    /* EVERY touch device, not just index 0. An Android phone registers several
     * (this one: 4) and the touchscreen is not necessarily the first -- polling
     * only device 0 reported zero fingers no matter where the screen was
     * touched, which looked exactly like touch not working at all. */
    for (d = 0; d < nDev; d++)
    {
    dev      = SDL_GetTouchDevice(d);
    nFingers = SDL_GetNumTouchFingers(dev);

    if (nFingers > 0)
        s_LastTouchMs = now;

    for (i = 0; i < nFingers; i++)
    {
        SDL_Finger*    f = SDL_GetTouchFinger(dev, i);
        s_TouchFinger* t;
        float          vx, vy;
        int            slot;

        if (f == NULL)
            continue;

        /* Window-normalized -> viewport-normalized, so a letterboxed picture
         * does not shift every control off where it is drawn. */
        {
            float fx = 0.0f, fy = 0.0f;
            int   px = (int)(f->x * (float)winW);
            int   py = (int)(f->y * (float)winH);

            if (!PsyX_MapWindowToViewport(px, py, &fx, &fy))
                continue; /* inside the black bars -- not on the picture at all */

            vx = fx;
            vy = fy;
        }

        t = Tc_FindFinger(dev, f->id);
        if (t == NULL)
        {
            t = Tc_NewFinger();
            if (t == NULL)
                continue;

            t->dev       = dev;
            t->id        = f->id;
            t->active    = 1;
            t->startX    = vx;
            t->startY    = vy;
            t->originX   = vx;
            t->originY   = vy;
            t->lastX     = vx;
            t->lastY     = vy;
            t->startMs   = now;
            t->movedFar  = 0;
            t->buttonIdx = -1;

            /* Role is decided once, here. A button wins over the zones so the
             * controls stay reachable with the looking thumb already down. */
            {
                int b = Tc_HitButton(vx, vy, aspect);

                if (b >= 0)
                {
                    t->role      = TR_BUTTON;
                    t->buttonIdx = b;
                }
                else if (vx < TC_LEFT_ZONE && !s_StickActive)
                {
                    t->role = TR_MOVE;
                }
                else
                {
                    t->role = TR_LOOK;
                }
            }
        }

        t->x = vx;
        t->y = vy;

        {
            float sdx = (vx - t->startX) * aspect;
            float sdy = (vy - t->startY);

            if (((sdx * sdx) + (sdy * sdy)) > (TC_TAP_SLOP * TC_TAP_SLOP))
                t->movedFar = 1;
        }

        slot = (int)(t - s_Fingers);
        if (slot >= 0 && slot < TC_MAX_FINGERS)
            seen[slot] = 1;

        switch (t->role)
        {
            case TR_BUTTON:
                if (t->buttonIdx >= 0 && t->buttonIdx < TB_COUNT)
                {
                    s_Buttons[t->buttonIdx].held       = 1;
                    s_Buttons[t->buttonIdx].holdFrames = TC_BUTTON_MIN_FRAMES;
                }
                break;

            case TR_MOVE:
            {
                float dx  = (vx - t->originX) * aspect;
                float dy  = (vy - t->originY);
                float len = SDL_sqrtf((dx * dx) + (dy * dy));
                float dead = TC_STICK_RADIUS * TC_STICK_DEADZONE;
                float mag;

                /* Drag the origin along once the thumb reaches the rim, so a
                 * long push cannot run out of stick and stall mid-corridor. */
                if (len > TC_STICK_RADIUS)
                {
                    float ox = dx / len * TC_STICK_RADIUS;
                    float oy = dy / len * TC_STICK_RADIUS;

                    t->originX = vx - (ox / aspect);
                    t->originY = vy - oy;
                    dx  = ox;
                    dy  = oy;
                    len = TC_STICK_RADIUS;
                }

                if (len <= dead)
                {
                    mag = 0.0f;
                }
                else
                {
                    mag = (len - dead) / (TC_STICK_RADIUS - dead);
                    if (mag > 1.0f)
                        mag = 1.0f;
                }

                s_StickActive = 1;
                s_StickOx     = t->originX;
                s_StickOy     = t->originY;
                s_StickKx     = vx;
                s_StickKy     = vy;

                if (mag > 0.0f && len > 0.0f)
                {
                    s_LeftX = Tc_AxisByte((dx / len) * mag);
                    s_LeftY = Tc_AxisByte((dy / len) * mag);
                }
                else
                {
                    s_LeftX = s_LeftY = 128;
                }

                s_Running = (mag >= TC_RUN_THRESHOLD);
                break;
            }

            case TR_LOOK:
            default:
                lookDx += (vx - t->lastX) * aspect;
                lookDy += (vy - t->lastY);
                break;
        }

        t->lastX = vx;
        t->lastY = vy;
    }
    }

    /* Released contacts: a short, stationary one was a tap. Buttons already
     * fired while held, so only the free zones produce Action. */
    for (i = 0; i < TC_MAX_FINGERS; i++)
    {
        s_TouchFinger* t = &s_Fingers[i];

        if (!t->active || seen[i])
            continue;

        if (t->role != TR_BUTTON && !t->movedFar &&
            (now - t->startMs) <= TC_TAP_MS)
        {
            s_ActionFrames = TC_ACTION_FRAMES;
        }

        if (t->role == TR_MOVE)
        {
            s_StickActive = 0;
            s_LeftX = s_LeftY = 128;
            s_Running = 0;
        }

        t->active = 0;
        t->role   = TR_NONE;
    }

    /* Look is a RATE: deflection tracks drag speed, so the camera stops the
     * moment the thumb does instead of drifting to an absolute position. */
    {
        float gain = Tc_LookGain() / TC_LOOK_SPAN;

        s_RightX = Tc_AxisByte(lookDx * gain);
        s_RightY = Tc_AxisByte(lookDy * gain);

        if (g_PcConfig.invertControllerY)
            s_RightY = Tc_AxisByte(-(lookDy * gain));
    }

    /* Buttons and gestures -> the player's own bindings. */
    {
        const s_ControllerConfig* cfg = &g_GameWorkPtr->config.controllerConfig;

        if (s_Buttons[TB_AIM].holdFrames   > 0) Tc_PressAction(&s_PadWord, cfg->aim);
        if (s_Buttons[TB_ITEM].holdFrames  > 0) Tc_PressAction(&s_PadWord, cfg->item);
        if (s_Buttons[TB_MAP].holdFrames   > 0) Tc_PressAction(&s_PadWord, cfg->map);
        if (s_Buttons[TB_START].holdFrames > 0) Tc_PressAction(&s_PadWord, cfg->pause);

        if (s_Running)
            Tc_PressAction(&s_PadWord, cfg->run);

        if (s_ActionFrames > 0)
        {
            Tc_PressAction(&s_PadWord, cfg->action);
            s_ActionFrames--;
        }
    }
}

int Pc_Touch_Active(void)
{
    if (!Tc_Enabled())
        return 0;
    if (SDL_GetNumTouchDevices() <= 0)
        return 0;

    return Tc_InGameplay();
}

void Pc_Touch_GetPad(unsigned short* word,
                     unsigned char* rightX, unsigned char* rightY,
                     unsigned char* leftX,  unsigned char* leftY)
{
    if (word   != NULL) *word   = s_PadWord;
    if (rightX != NULL) *rightX = s_RightX;
    if (rightY != NULL) *rightY = s_RightY;
    if (leftX  != NULL) *leftX  = s_LeftX;
    if (leftY  != NULL) *leftY  = s_LeftY;
}

int Pc_Touch_UsedRecently(void)
{
    if (s_LastTouchMs == 0)
        return 0;

    return (SDL_GetTicks() - s_LastTouchMs) < 3000;
}

/* ------------------------------------------------------------------ draw --- */

/* The overlay layer is centre-origin, and how far it reaches sideways depends on
 * whether the picture is widened.
 *
 * This deliberately does NOT reuse the minimap's predicate. That one also has to
 * describe MENUS, which stay pillarboxed under Hor+ and so only widen in stretch
 * mode. These controls draw during gameplay only, where Hor+ widens the picture
 * as well — testing for stretch alone computed 160 against a layer that really
 * spanned +-260, which pulled every button ~15% of the screen toward the centre.
 * Mode 0 is the genuinely pillarboxed case and keeps the 4:3 extent, which is
 * also what viewport-space touch coords are relative to. */
static int Tc_HalfWidth(void)
{
    extern int g_PcWidescreenMode;

    float psxA = (float)SCREEN_WIDTH / (float)SCREEN_HEIGHT;
    float winA;
    int   sw = 0, sh = 0;

    PsyX_GetScreenSize(&sw, &sh);
    winA = (sw > 0 && sh > 0) ? (float)sw / (float)sh : psxA;

    if (g_PcWidescreenMode != 0 && winA > psxA + 0.01f)
        return (int)((160.0f * (winA / psxA)) + 0.5f);

    return 160;
}

#define TC_MAX_QUADS 220

typedef struct
{
    POLY_G4* p;
    int      used;
} s_TcBatch;

static void Tc_Quad(s_TcBatch* b, int x0, int y0, int x1, int y1,
                    int x2, int y2, int x3, int y3, int lum)
{
    POLY_G4* q;

    if (b->used >= TC_MAX_QUADS)
        return;

    q = &b->p[b->used++];
    setXY4(q, x0, y0, x1, y1, x2, y2, x3, y3);
    q->r0 = q->r1 = q->r2 = q->r3 = (u_char)lum;
    q->g0 = q->g1 = q->g2 = q->g3 = (u_char)lum;
    q->b0 = q->b1 = q->b2 = q->b3 = (u_char)lum;
}

/* Filled octagon as three non-overlapping quads (cap, band, cap). Overlapping
 * pieces would double-blend through the translucent DR_MODE and show their
 * seams as bright bands. */
static void Tc_Octagon(s_TcBatch* b, int cx, int cy, int r, int lum)
{
    int a = (r * 41) / 100;

    Tc_Quad(b, cx - a, cy - r, cx + a, cy - r, cx - r, cy - a, cx + r, cy - a, lum);
    Tc_Quad(b, cx - r, cy - a, cx + r, cy - a, cx - r, cy + a, cx + r, cy + a, lum);
    Tc_Quad(b, cx - r, cy + a, cx + r, cy + a, cx - a, cy + r, cx + a, cy + r, lum);
}

/* Octagonal ring between radii rOuter and rInner: eight annulus quads, the same
 * construction the crosshair's circle style uses. */
static void Tc_Ring(s_TcBatch* b, int cx, int cy, int rOuter, int rInner, int lum)
{
    static const int SX[8] = { 100,  71,   0, -71, -100, -71,    0,  71 };
    static const int SY[8] = {   0,  71, 100,  71,    0, -71, -100, -71 };
    int i;

    for (i = 0; i < 8; i++)
    {
        int j = (i + 1) & 7;
        int ox0 = cx + ((SX[i] * rOuter) / 100), oy0 = cy + ((SY[i] * rOuter) / 100);
        int ox1 = cx + ((SX[j] * rOuter) / 100), oy1 = cy + ((SY[j] * rOuter) / 100);
        int ix0 = cx + ((SX[i] * rInner) / 100), iy0 = cy + ((SY[i] * rInner) / 100);
        int ix1 = cx + ((SX[j] * rInner) / 100), iy1 = cy + ((SY[j] * rInner) / 100);

        Tc_Quad(b, ox0, oy0, ox1, oy1, ix0, iy0, ix1, iy1, lum);
    }
}

void Pc_Touch_Draw(void)
{
    static POLY_G4 s_pool[2][TC_MAX_QUADS];
    static DR_MODE s_drMode[2];
    static int     s_inited = 0;

    s_TcBatch batch;
    GsOT*     ot;
    int       buf, i, halfW;

    if (!Tc_Enabled())
        return;
    if (!Tc_InGameplay())
        return;
    if (SDL_GetNumTouchDevices() <= 0)
        return;

    if (!s_inited)
    {
        s_inited = 1;
        for (i = 0; i < 2; i++)
        {
            setcode(&s_drMode[i], 0xE1);
            setlen(&s_drMode[i], 1);
            s_drMode[i].code[0] = 0xE1000200; /* ABR=0 -> 0.5*src + 0.5*dst */
        }
        for (i = 0; i < 2 * TC_MAX_QUADS; i++)
        {
            POLY_G4* q = (POLY_G4*)&s_pool[0][0] + i;
            setPolyG4(q);
            setSemiTrans(q, 1);
        }
    }

    buf         = g_ActiveBufferIdx;
    batch.p     = s_pool[buf];
    batch.used  = 0;
    halfW       = Tc_HalfWidth();

    /* Viewport space -> the centre-origin overlay. Y is always the 4:3 -120..120
     * band; X widens with the Hor+ ortho. */
    #define TC_UX(vx) ((int)((((vx) - 0.5f) * 2.0f * (float)halfW) + 0.5f))
    #define TC_UY(vy) ((int)((((vy) - 0.5f) * 240.0f) + 0.5f))
    /* A radius given in height units is 240 tall-units across the whole screen. */
    #define TC_UR(r)  ((int)(((r) * 240.0f) + 0.5f))

    /* Movement stick: only while a thumb is down. A permanently drawn stick is
     * clutter on a screen this small, and the floating origin means a fixed
     * one would be lying about where it is anyway. */
    if (s_StickActive)
    {
        int ox = TC_UX(s_StickOx), oy = TC_UY(s_StickOy);
        int kx = TC_UX(s_StickKx), ky = TC_UY(s_StickKy);
        int rr = TC_UR(TC_STICK_RADIUS);

        Tc_Ring(&batch, ox, oy, rr, (rr * 88) / 100, 150);
        Tc_Octagon(&batch, kx, ky, (rr * 38) / 100, s_Running ? 255 : 190);
    }

    for (i = 0; i < TB_COUNT; i++)
    {
        int cx = TC_UX(s_Buttons[i].cx);
        int cy = TC_UY(s_Buttons[i].cy);
        int r  = TC_UR(s_Buttons[i].r);
        int lum = (s_Buttons[i].holdFrames > 0) ? 255 : 140;

        Tc_Ring(&batch, cx, cy, r, (r * 82) / 100, lum);

        /* A distinct mark per button, so they read as different controls
         * without a font: crosshair, square, folded sheet, two bars. */
        switch (i)
        {
            case TB_AIM:
            {
                int t = (r * 9) / 100, l = (r * 46) / 100;
                Tc_Quad(&batch, cx - l, cy - t, cx + l, cy - t, cx - l, cy + t, cx + l, cy + t, lum);
                Tc_Quad(&batch, cx - t, cy - l, cx + t, cy - l, cx - t, cy + l, cx + t, cy + l, lum);
                break;
            }
            case TB_ITEM:
            {
                int s = (r * 34) / 100;
                Tc_Quad(&batch, cx - s, cy - s, cx + s, cy - s, cx - s, cy + s, cx + s, cy + s, lum);
                break;
            }
            case TB_MAP:
            {
                int w = (r * 42) / 100, h = (r * 32) / 100;
                Tc_Quad(&batch, cx - w, cy - h, cx + w, cy - h, cx - w, cy + h, cx + w, cy + h, lum);
                break;
            }
            case TB_START:
            default:
            {
                int w = (r * 12) / 100, h = (r * 34) / 100, g = (r * 22) / 100;
                Tc_Quad(&batch, cx - g - w, cy - h, cx - g + w, cy - h, cx - g - w, cy + h, cx - g + w, cy + h, lum);
                Tc_Quad(&batch, cx + g - w, cy - h, cx + g + w, cy - h, cx + g - w, cy + h, cx + g + w, cy + h, lum);
                break;
            }
        }
    }

    if (batch.used <= 0)
        return;

    ot = &g_OtTags0[buf][4];
    for (i = 0; i < batch.used; i++)
        AddPrim(ot, &batch.p[i]);
    AddPrim(ot, &s_drMode[buf]);

    #undef TC_UX
    #undef TC_UY
    #undef TC_UR
}
