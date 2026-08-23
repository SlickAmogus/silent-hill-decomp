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
#include "screens/options.h" /* OptionsMenuState_Brightness */
#include "pc_retroachievements.h" /* Pc_Ra_IsSignedIn */

#define TC_MAX_FINGERS 8

/* Roles a contact can take, decided once when it lands and held until release:
 * a thumb that starts on the movement side must keep steering even if it slides
 * across the middle. */
enum { TR_NONE = 0, TR_MOVE, TR_LOOK, TR_BUTTON, TR_ADVANCE };

/* Actions the on-screen buttons drive. Indices into s_Buttons. */
enum { TB_AIM = 0, TB_ITEM, TB_MAP, TB_START, TB_RUN, TB_BACK, TB_FIRE, TB_COUNT };

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
    /* TB_RUN   */ { 0.665f, 0.760f, 0.065f, 0 },
    /* TB_BACK is only ever drawn in the corner escape slot, so its own
     * position is never used -- it exists to carry a glyph and a binding. */
    /* TB_BACK  */ { 0.955f, 0.075f, 0.055f, 0 },
    /* Mirror of Aim on the other thumb, and only while Aim is held: firing
     * meant tapping the steering half of the screen, which fights the stick
     * the same thumb is holding. Hidden the rest of the time so it never eats
     * a movement drag. */
    /* TB_FIRE  */ { 0.095f, 0.760f, 0.105f, 0 },
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
static int            s_AdvanceHeld;   /* a finger is down during an advance state */
static int            s_PadAttached;   /* an SDL game controller is plugged in */
static int            s_PhysicalInput; /* a pad/keyboard was the last thing used */
static Uint32         s_LastTouchMs;

/* Movement stick geometry, in height units. The radius is a thumb's comfortable
 * travel, not a screen fraction that would balloon on a tablet. */
#define TC_STICK_RADIUS   0.150f
#define TC_STICK_DEADZONE 0.150f  /* fraction of the radius */
/* Engage high enough that a normal walk does not trip it, release much
 * lower so the run survives the dips a thumb makes while steering. One
 * threshold for both meant 0.850 had to be reachable AND holdable: it was
 * a long drag to start and dropped the moment the thumb eased off. */
#define TC_RUN_THRESHOLD  0.680f  /* deflection past this starts a run */
#define TC_RUN_RELEASE    0.480f  /* below this it stops -- hysteresis */

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

static int Tc_Enabled(void);

enum { TC_MODE_OFF = 0, TC_MODE_GAMEPLAY, TC_MODE_PAUSE, TC_MODE_MAP, TC_MODE_ADVANCE, TC_MODE_BACK,
       TC_MODE_TITLE };

/* Gameplay gets the full scheme. Pause gets Start ALONE -- nothing else on that
 * screen responds to a pointer, so hiding the controls there left no way back
 * out of a pause once one had been opened by touch. Everywhere else (menus,
 * inventory, the map) touch already works through pc_mouse_cursor and a pad on
 * top of it would fight it. */
static int Tc_Mode(void)
{
    if (!Tc_Enabled())
        return TC_MODE_OFF;

    /* The boot logos and the intro movies are GAME states, not sys states, so
     * the checks below never saw them and the Konami/KCET screens could not be
     * skipped by touch the way Start skips them on a pad. */
    if (g_GameWork.gameState == GameState_KonamiLogo ||
        g_GameWork.gameState == GameState_KcetLogo ||
        g_GameWork.gameState == GameState_MovieIntroFadeIn ||
        g_GameWork.gameState == GameState_MovieIntroAlternate ||
        g_GameWork.gameState == GameState_MovieIntro ||
        g_GameWork.gameState == GameState_MovieOpening ||
        g_GameWork.gameState == GameState_ExitMovie)
        return TC_MODE_ADVANCE;

    /* The brightness screen is a slider with no pointer support, so touch could
     * open it and then had no way out. It leaves on enter|cancel; Back sends
     * cancel, which leaves the setting as it was. */
    if (g_GameWork.gameState == GameState_OptionScreen &&
        g_GameWork.gameStateSteps[0] == OptionsMenuState_Brightness)
        return TC_MODE_BACK;

    /* The save/load screen is cancel-only in the same way, and it is reachable
     * straight from the pause menu, so with no pad a player could get into it
     * and not back out. */
    if (g_GameWork.gameState == GameState_SaveScreen ||
        g_GameWork.gameState == GameState_LoadSavegameScreen)
        return TC_MODE_BACK;

    /* The title screen opens the achievement browser on the Map bind (there is
     * no map to show there). With no pad and no such button on screen, signing
     * in from the options menu left no way to ever look at the list.
     *
     * Self-gating rather than platform-gated, so this file keeps its property
     * of having no platform conditionals: the button appears only once an
     * account is actually on file, and Pc_Ra_IsSignedIn() is a stub returning 0
     * wherever RetroAchievements is not compiled in. The menu underneath stays
     * pointer-driven as before, and the browser itself takes drag and tap
     * through the same cursor. */
    if (g_GameWork.gameState == GameState_MainMenu && Pc_Ra_IsSignedIn())
        return TC_MODE_TITLE;

    if (g_GameWork.gameState != GameState_InGame)
        return TC_MODE_OFF;
    if (g_SysWork.sysState == SysState_Gameplay)
        return TC_MODE_GAMEPLAY;
    if (g_SysWork.sysState == SysState_GamePaused)
        return TC_MODE_PAUSE;
    if (g_SysWork.sysState == SysState_MapScreen)
        return TC_MODE_MAP;

    /* States that are just waiting to be advanced or skipped: message and
     * examine text, scripted scenes, the FMV hand-off, the game-over screen.
     * A pad presses Start or Cross here; a touchscreen had no way to say it,
     * so cutscenes could not be skipped and text could not be advanced.
     *
     * The save menus belong here too. They open with Harry talking, and with
     * neither state listed the whole block fell through to OFF -- no advance,
     * no escape, nothing to tap at the first save point.
     *
     * EXCEPT while a free-cursor puzzle is up: those are already driven as a
     * pointer by pc_mouse_cursor, and injecting a confirm underneath would
     * fire twice on every tap. That test carries the save menus correctly too,
     * because the slot list draws a cursor and the dialogue before it does not:
     * the talking part advances on a tap, and once the slots are up the pointer
     * drives them with the corner Back button for a way out. */
    if (g_SysWork.sysState == SysState_ReadMessage ||
        g_SysWork.sysState == SysState_EventCallback ||
        g_SysWork.sysState == SysState_Fmv ||
        g_SysWork.sysState == SysState_GameOver ||
        g_SysWork.sysState == SysState_SaveMenu0 ||
        g_SysWork.sysState == SysState_SaveMenu1)
    {
        extern int Pc_MouseCursor_PuzzleActive(void);

        /* A free-cursor puzzle is already driven as a pointer by
         * pc_mouse_cursor, so no confirm may be injected underneath -- it would
         * fire twice on every tap. But OFF left the corner empty too, and these
         * screens are cancel-only: with no pad there was no way out at all, so
         * opening one on a phone was a softlock. Give it the same lone Back
         * button the brightness screen gets. The drag still reaches the cursor;
         * only that one corner slot is taken. */
        if (Pc_MouseCursor_PuzzleActive())
            return TC_MODE_BACK;

        return TC_MODE_ADVANCE;
    }

    return TC_MODE_OFF;
}

/* Full-screen states that a pad closes with one specific button, and that no
 * pointer can dismiss (unlike the inventory and options screens, which
 * pc_mouse_cursor already drives). Each keeps exactly that button alive so
 * touch always has a way back out -- opening one with no way to leave it is
 * how the pause screen trapped the player. Drawn in the corner slot whatever
 * the action, so "get out of here" is always in the same place. */
static int Tc_SoloButton(int mode)
{
    if (mode == TC_MODE_PAUSE)
        return TB_START;   /* pause exits on the pause bind */
    if (mode == TC_MODE_MAP)
        return TB_MAP;     /* the map screen exits on the map bind */
    if (mode == TC_MODE_BACK)
        return TB_BACK;    /* brightness and friends leave on cancel */
    if (mode == TC_MODE_TITLE)
        return TB_MAP;     /* opens the achievement browser, and closes it */

    return -1;
}

/* Real hardware wins. Two tests, because one is not enough here: SDL opens a
 * pad as a GameController on most platforms, but on Android it frequently never
 * enumerates one at all -- this project's own GameSir arrives purely as key
 * events, with SDL reporting no joysticks but the accelerometer. So also treat
 * ANY keyboard/pad button as proof that something physical is in use.
 *
 * Touching the screen hands control back, so a pad left connected and idle
 * does not permanently lock out a player who puts it down. */
void Pc_Touch_NoteOtherInput(int padAttached, int keyWord)
{
    s_PadAttached = (padAttached != 0);

    if (keyWord != 0xFFFF)
        s_PhysicalInput = 1;
}

static int Tc_Enabled(void)
{
    if (!g_PcConfig.touchControls)
        return 0;

    return !(s_PadAttached || s_PhysicalInput);
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
    s_AdvanceHeld = 0;
}

void Pc_Touch_Update(void)
{
    SDL_TouchID dev;
    Uint32      now;
    float       aspect;
    int         nDev, nFingers, i, d, mode;
    int         seen[TC_MAX_FINGERS];
    float       lookDx = 0.0f, lookDy = 0.0f;
    int         winW = 0, winH = 0;

    mode = Tc_Mode();
    if (mode == TC_MODE_OFF)
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

    s_PadWord     = 0xFFFF;
    s_AdvanceHeld = 0;
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

            s_PhysicalInput = 0; /* last input wins: the screen is in use again */

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

                if (mode == TC_MODE_ADVANCE)
                {
                    /* Anywhere on the screen, with no target to find: there is
                     * nothing else to touch during a scene or a wall of text. */
                    t->role      = TR_ADVANCE;
                    t->buttonIdx = -1;
                }
                else if (mode != TC_MODE_GAMEPLAY)
                {
                    /* One live control, in the corner slot; a stray thumb
                     * anywhere else must not steer a frozen world. */
                    int   solo = Tc_SoloButton(mode);
                    float sdx  = (vx - s_Buttons[TB_START].cx) * aspect;
                    float sdy  = (vy - s_Buttons[TB_START].cy);
                    float sr   = s_Buttons[TB_START].r * 1.25f;
                    int   onIt = (((sdx * sdx) + (sdy * sdy)) <= (sr * sr));

                    t->role      = (onIt && solo >= 0) ? TR_BUTTON : TR_NONE;
                    t->buttonIdx = (onIt && solo >= 0) ? solo : -1;
                }
                else if (b >= 0)
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
            case TR_ADVANCE:
                s_AdvanceHeld = 1;
                break;

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

                /* Pushing the stick fully BACK used to trip this too, and
                 * Run + Back is the quick back-jump -- so every backward step
                 * became a lurch. Auto-run is for going forward; a deliberate
                 * back-jump is still available by holding the Run button, which
                 * is how a pad does it. dy is screen-down-positive, and the
                 * margin keeps a run alive while turning hard. */
                {
                    const float engage = s_Running ? TC_RUN_RELEASE : TC_RUN_THRESHOLD;
                    s_Running = (mag >= engage) && (dy < (0.35f * len));
                }
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

        if (mode == TC_MODE_GAMEPLAY && t->role != TR_BUTTON && !t->movedFar &&
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
        /* Fire only counts while the gun is up. One-button combat folds it into
         * Aim itself, for players who would rather not hold two things at once. */
        if (s_Buttons[TB_AIM].holdFrames > 0 &&
            (g_PcConfig.oneButtonCombat || s_Buttons[TB_FIRE].holdFrames > 0))
            Tc_PressAction(&s_PadWord, cfg->action);
        if (s_Buttons[TB_ITEM].holdFrames  > 0) Tc_PressAction(&s_PadWord, cfg->item);
        if (s_Buttons[TB_MAP].holdFrames   > 0) Tc_PressAction(&s_PadWord, cfg->map);
        if (s_Buttons[TB_START].holdFrames > 0) Tc_PressAction(&s_PadWord, cfg->pause);
        if (s_Buttons[TB_BACK].holdFrames  > 0) Tc_PressAction(&s_PadWord, cfg->cancel);

        if (s_Running || s_Buttons[TB_RUN].holdFrames > 0)
            Tc_PressAction(&s_PadWord, cfg->run);

        if (s_AdvanceHeld)
            Tc_PressAction(&s_PadWord, cfg->enter);

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

    return Tc_Mode() != TC_MODE_OFF;
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

/* Any finger on the glass right now, whatever the game state. The FMV player
 * runs its own input loop outside the state machine and has to ask directly.
 * Honors the touch-controls setting, so someone on a gamepad can rest a hand
 * on the screen without skipping every movie. */
int Pc_Touch_AnyContact(void)
{
    int n, d;

    if (!Tc_Enabled())
        return 0;

    n = SDL_GetNumTouchDevices();
    for (d = 0; d < n; d++)
    {
        if (SDL_GetNumTouchFingers(SDL_GetTouchDevice(d)) > 0)
            return 1;
    }

    return 0;
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
    int       buf, i, halfW, mode;

    mode = Tc_Mode();
    if (mode == TC_MODE_OFF)
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
    if (s_StickActive && mode == TC_MODE_GAMEPLAY)
    {
        int ox = TC_UX(s_StickOx), oy = TC_UY(s_StickOy);
        int kx = TC_UX(s_StickKx), ky = TC_UY(s_StickKy);
        int rr = TC_UR(TC_STICK_RADIUS);

        Tc_Ring(&batch, ox, oy, rr, (rr * 88) / 100, 150);
        Tc_Octagon(&batch, kx, ky, (rr * 38) / 100, s_Running ? 255 : 190);
    }

    if (mode == TC_MODE_ADVANCE)
    {
        /* Deliberately draws nothing. The whole screen is the control, and a
         * button here would cover the very text it exists to advance. */
        if (batch.used <= 0)
            return;
    }

    for (i = 0; i < TB_COUNT; i++)
    {
        float bcx = s_Buttons[i].cx, bcy = s_Buttons[i].cy, br = s_Buttons[i].r;
        int   cx;

        if (mode == TC_MODE_ADVANCE)
            continue;

        /* TB_BACK carries a glyph and a binding for the corner escape slot; its
         * own position is a copy of Start's. Drawing it in gameplay too put the
         * back mark and the pause bars inside the same ring. */
        if (mode == TC_MODE_GAMEPLAY && i == TB_BACK)
            continue;

        /* Fire appears with the gun and goes away with it. */
        if (i == TB_FIRE &&
            (mode != TC_MODE_GAMEPLAY || g_PcConfig.oneButtonCombat ||
             s_Buttons[TB_AIM].holdFrames <= 0))
            continue;

        if (mode != TC_MODE_GAMEPLAY)
        {
            if (i != Tc_SoloButton(mode))
                continue;

            bcx = s_Buttons[TB_START].cx;
            bcy = s_Buttons[TB_START].cy;
            br  = s_Buttons[TB_START].r;
        }

        cx = TC_UX(bcx);
        {
        int cy = TC_UY(bcy);
        int r  = TC_UR(br);
        int lum = (s_Buttons[i].holdFrames > 0) ? 255 : 140;

        Tc_Ring(&batch, cx, cy, r, (r * 82) / 100, lum);

        /* A distinct mark per button, so they read as different controls
         * without a font: crosshair, square, folded sheet, two bars. */
        switch (i)
        {
            case TB_FIRE:
            {
                int d = (r * 34) / 100;
                Tc_Quad(&batch, cx - d, cy - d, cx + d, cy - d, cx - d, cy + d, cx + d, cy + d, lum);
                Tc_Quad(&batch, cx - (d * 3) / 2, cy, cx, cy - (d * 3) / 2,
                                cx, cy + (d * 3) / 2, cx + (d * 3) / 2, cy, lum);
                break;
            }
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
            case TB_BACK:
            {
                /* Left chevron: two slanted bars meeting at the point. */
                int a = (r * 34) / 100, t = (r * 11) / 100;

                Tc_Quad(&batch, cx + a, cy - a, cx + a + t, cy - a + t,
                                cx - a, cy,     cx - a + t, cy + t,       lum);
                Tc_Quad(&batch, cx - a, cy,     cx - a + t, cy - t,
                                cx + a, cy + a, cx + a + t, cy + a - t,   lum);
                break;
            }
            case TB_RUN:
            {
                /* Three stacked speed lines -- distinct at a glance from
                 * Start's two upright bars. */
                int h = (r * 7) / 100, g = (r * 26) / 100;
                int w0 = (r * 46) / 100, w1 = (r * 34) / 100, w2 = (r * 22) / 100;

                Tc_Quad(&batch, cx - w0, cy - g - h, cx + w0, cy - g - h, cx - w0, cy - g + h, cx + w0, cy - g + h, lum);
                Tc_Quad(&batch, cx - w1, cy - h,     cx + w1, cy - h,     cx - w1, cy + h,     cx + w1, cy + h,     lum);
                Tc_Quad(&batch, cx - w2, cy + g - h, cx + w2, cy + g - h, cx - w2, cy + g + h, cx + w2, cy + g + h, lum);
                break;
            }
        }
        }
    }

    if (batch.used <= 0)
        return;

    /* OT0 is drawn first and OT2 after it. In gameplay OT0 is right -- the
     * controls sit over the world exactly like the crosshair. But the map, save
     * and item screens draw their fullscreen 2D into OT2, which then paints
     * straight over anything left in OT0: the lone escape button was being
     * submitted every frame and buried, so the corner was tappable with nothing
     * visible in it. Put the solo-button modes in OT2 so the way out is drawn
     * on top of the screen it is meant to leave. */
    if (mode != TC_MODE_GAMEPLAY)
        ot = &g_OrderingTable2[g_ActiveBufferIdx];
    else
        ot = &g_OtTags0[buf][4];

    for (i = 0; i < batch.used; i++)
        AddPrim(ot, &batch.p[i]);
    AddPrim(ot, &s_drMode[buf]);

    #undef TC_UX
    #undef TC_UY
    #undef TC_UR
}
