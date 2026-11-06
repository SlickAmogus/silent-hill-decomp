/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdlib.h>

#include <SDL.h>

#include "pc_mouse_cursor.h"
#include "pc_reset_settings.h"

extern int PsyX_RawControllerBindHeld(int buttonOrAxis);
extern int PsyX_Pad_AxisValue(int sdlAxis);

#if defined(__ANDROID__) || defined(SH_IOS) || defined(__EMSCRIPTEN__)
#define RS_HAS_TOUCH 1
extern int Pc_Touch_IsDrivingInput(void);
#else
#define RS_HAS_TOUCH 0
#endif

/* The analog trigger counts as pulled where the pad word does (PsyX_pad.cpp
 * PadBtnPressed presses above 24000). */
#define RS_TRIGGER_PULLED 24000

static int s_mode = RS_HAS_TOUCH ? PC_INPUT_TOUCH : PC_INPUT_KEYBOARD;
static int s_touchRequest;
static int s_rWas = 1;
static int s_triggersWas = 1;

void Pc_ResetSettings_Track(void)
{
    const Uint8* ks;
    int          n, i;

#if RS_HAS_TOUCH
    /* Taps also arrive as mouse events, so touch is asked first and the mouse
     * is not read as a sign of a desktop player while it drives. */
    if (Pc_Touch_IsDrivingInput())
    {
        s_mode = PC_INPUT_TOUCH;
        return;
    }
#endif
    for (i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++)
    {
        if (PsyX_RawControllerBindHeld(i))
        {
            s_mode = PC_INPUT_PAD;
            return;
        }
    }
    for (i = 0; i < SDL_CONTROLLER_AXIS_MAX; i++)
    {
        if (abs(PsyX_Pad_AxisValue(i)) > 16384)
        {
            s_mode = PC_INPUT_PAD;
            return;
        }
    }
    ks = SDL_GetKeyboardState(&n);
    for (i = 0; ks != NULL && i < n; i++)
    {
        if (ks[i])
        {
            s_mode = PC_INPUT_KEYBOARD;
            return;
        }
    }
#if !RS_HAS_TOUCH
    if (Pc_MouseCursor_Moved() || Pc_MouseCursor_LeftClicked() || Pc_MouseCursor_RightClicked())
        s_mode = PC_INPUT_KEYBOARD;
#endif
}

int Pc_ResetSettings_InputMode(void)
{
    return s_mode;
}

const char* Pc_ResetSettings_Hint(void)
{
    return s_mode == PC_INPUT_PAD ? "Press triggers to reset" : "Press R to reset";
}

void Pc_ResetSettings_TouchRequest(void)
{
    s_touchRequest = 1;
}

int Pc_ResetSettings_RequestEdge(void)
{
    const Uint8* ks       = SDL_GetKeyboardState(NULL);
    int          r        = ks != NULL && ks[SDL_SCANCODE_R];
    int          triggers = PsyX_Pad_AxisValue(SDL_CONTROLLER_AXIS_TRIGGERLEFT) > RS_TRIGGER_PULLED &&
                            PsyX_Pad_AxisValue(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > RS_TRIGGER_PULLED;
    int          fire     = (r && !s_rWas) || (triggers && !s_triggersWas) || s_touchRequest;

    s_rWas         = r;
    s_triggersWas  = triggers;
    s_touchRequest = 0;
    return fire;
}
