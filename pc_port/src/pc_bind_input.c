/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_bind_input.c - raw SDL navigation and bind capture. See pc_bind_input.h.
 */

#include <stdio.h>
#include <string.h>

#include <SDL.h>

#include "pc_bind_core.h"
#include "pc_bind_input.h"

extern const char* PsyX_Pad_ConnectedControllerName(void);
extern const char* PsyX_Pad_HeldBindName(void);
extern int         PsyX_Pad_AxisValue(int sdlAxis);
extern int         g_PsyX_WheelUpFrames, g_PsyX_WheelDownFrames;

#define BP_LISTEN_PAD_MS 8000u

static int    s_capRow, s_capCol;
static int    s_listenArmed; /* everything was released since listening began */
static Uint32 s_listenStart;

void BindNav_Reset(BindNav* n)
{
    int i;
    for (i = 0; i < BN_COUNT; i++)
    {
        n->prev[i]     = 1; /* whatever opened the panel must be released first */
        n->repeatAt[i] = 0;
    }
}

static void bp_read_nav(int held[BN_COUNT])
{
    const Uint8* ks = SDL_GetKeyboardState(NULL);
    const int    ly = PsyX_Pad_AxisValue(SDL_CONTROLLER_AXIS_LEFTY);
    const int    lx = PsyX_Pad_AxisValue(SDL_CONTROLLER_AXIS_LEFTX);
    const char*  pb = PsyX_Pad_HeldBindName();

    memset(held, 0, sizeof(int) * BN_COUNT);
    if (ks)
    {
        held[BN_UP]    = ks[SDL_SCANCODE_UP];
        held[BN_DOWN]  = ks[SDL_SCANCODE_DOWN];
        held[BN_LEFT]  = ks[SDL_SCANCODE_LEFT];
        held[BN_RIGHT] = ks[SDL_SCANCODE_RIGHT];
        held[BN_OK]    = ks[SDL_SCANCODE_RETURN] || ks[SDL_SCANCODE_KP_ENTER] || ks[SDL_SCANCODE_SPACE];
        held[BN_BACK]  = ks[SDL_SCANCODE_ESCAPE];
        held[BN_CLEAR] = ks[SDL_SCANCODE_DELETE] || ks[SDL_SCANCODE_BACKSPACE];
    }
    held[BN_UP]    |= ly < -16000 || (pb && strcmp(pb, "dpup") == 0);
    held[BN_DOWN]  |= ly >  16000 || (pb && strcmp(pb, "dpdown") == 0);
    held[BN_LEFT]  |= lx < -16000 || (pb && strcmp(pb, "dpleft") == 0);
    held[BN_RIGHT] |= lx >  16000 || (pb && strcmp(pb, "dpright") == 0);
    if (pb)
    {
        held[BN_OK]    |= strcmp(pb, "a") == 0;
        held[BN_BACK]  |= strcmp(pb, "b") == 0 || strcmp(pb, "start") == 0;
        held[BN_CLEAR] |= strcmp(pb, "x") == 0;
    }
}

/* Press edges, with key-repeat on the four directions. */
static void bp_nav_edges(BindNav* nav, int held[BN_COUNT], int pressed[BN_COUNT])
{
    const Uint32 now = SDL_GetTicks();
    int          i;

    for (i = 0; i < BN_COUNT; i++)
    {
        pressed[i] = 0;
        if (held[i] && !nav->prev[i])
        {
            pressed[i]      = 1;
            nav->repeatAt[i] = now + 360u;
        }
        else if (held[i] && i <= BN_RIGHT && now >= nav->repeatAt[i])
        {
            pressed[i]      = 1;
            nav->repeatAt[i] = now + 90u;
        }
        nav->prev[i] = held[i];
    }
}

void BindNav_Poll(BindNav* n, int pressed[BN_COUNT])
{
    int held[BN_COUNT];

    bp_read_nav(held);
    bp_nav_edges(n, held, pressed);
}

int BindInput_PadConnected(void)
{
    return PsyX_Pad_ConnectedControllerName() != NULL;
}

void BindCapture_Begin(int row, int col)
{
    s_capRow      = row;
    s_capCol      = col;
    s_listenArmed = 0;
    s_listenStart = SDL_GetTicks();
}

/* Returns the captured bind name, "" to cancel, or NULL while still waiting. */
const char* BindCapture_Update(void)
{
    static char  name[32];
    static Uint8 prevKeys[SDL_NUM_SCANCODES];
    const Uint8* ks;
    int          nk, sc;
    Uint32       mb;
    const BcRow* row   = BindCore_Row(s_capRow);
    int          flags = row ? row->flags : 0;

    ks = SDL_GetKeyboardState(&nk);
    mb = SDL_GetMouseState(NULL, NULL);

    if (s_capCol >= BC_PAD1)
    {
        const char* pb = PsyX_Pad_HeldBindName();

        if (ks && ks[SDL_SCANCODE_ESCAPE])
            return "";
        if (SDL_GetTicks() - s_listenStart > BP_LISTEN_PAD_MS || PsyX_Pad_ConnectedControllerName() == NULL)
            return "";
        if (!s_listenArmed)
        {
            if (pb == NULL)
                s_listenArmed = 1;
            return NULL;
        }
        if (pb == NULL)
            return NULL;
        snprintf(name, sizeof(name), "%s", pb);
        return name;
    }

    if (!s_listenArmed)
    {
        int any = (mb & (SDL_BUTTON_LMASK | SDL_BUTTON_MMASK | SDL_BUTTON_RMASK |
                         SDL_BUTTON_X1MASK | SDL_BUTTON_X2MASK)) != 0;
        /* Lock keys are skipped here: some platforms report a toggled lock as
         * held, which would keep the panel waiting forever. They can still be
         * captured as binds below. */
        for (sc = 1; ks && sc < nk && !any; sc++)
            any = ks[sc] && sc != SDL_SCANCODE_CAPSLOCK && sc != SDL_SCANCODE_NUMLOCKCLEAR &&
                  sc != SDL_SCANCODE_SCROLLLOCK;
        if (!any)
        {
            s_listenArmed = 1;
            if (ks)
                memcpy(prevKeys, ks, (size_t)(nk < SDL_NUM_SCANCODES ? nk : SDL_NUM_SCANCODES));
        }
        return NULL;
    }

    if (ks && ks[SDL_SCANCODE_ESCAPE])
        return "";

    /* A key counts only on its press: a lock key that reads as held from the
     * start is never taken. */
    for (sc = 1; ks && sc < nk && sc < SDL_NUM_SCANCODES; sc++)
    {
        const char* kn;
        const int   pressed = ks[sc] && !prevKeys[sc];

        prevKeys[sc] = ks[sc];
        if (!pressed)
            continue;
        kn = SDL_GetScancodeName((SDL_Scancode)sc);
        if (kn == NULL || kn[0] == '\0')
            continue;
        snprintf(name, sizeof(name), "%s", kn);
        return name;
    }

    if (flags & BCF_MOUSE)
    {
        static const struct { Uint32 mask; const char* id; } mouse[] = {
            { SDL_BUTTON_LMASK, "Mouse1" },  { SDL_BUTTON_RMASK, "Mouse2" },
            { SDL_BUTTON_MMASK, "Mouse3" },  { SDL_BUTTON_X1MASK, "Mouse4" },
            { SDL_BUTTON_X2MASK, "Mouse5" },
        };
        int i;
        for (i = 0; i < (int)(sizeof(mouse) / sizeof(mouse[0])); i++)
        {
            if (mb & mouse[i].mask)
                return mouse[i].id;
        }
        if (flags & BCF_WHEEL)
        {
            if (g_PsyX_WheelUpFrames == 2)
                return "MouseWheelUp";
            if (g_PsyX_WheelDownFrames == 2)
                return "MouseWheelDown";
        }
    }
    return NULL;
}
