/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The phone itself as the rumble motor.
 *
 * The game's vibration engine writes two DualShock actuator bytes a frame (the
 * small motor, on or off, and the big one, 0-255); PsyCross sends them to a
 * controller. Played on the touchscreen there is no controller, so the Options
 * Vibration switch did nothing. With no pad in use the same two bytes drive
 * the device's own vibrator here. */
#if defined(__ANDROID__) || defined(SH_IOS)

#include "game.h"
#include "sh_log.h"

#include <SDL.h>
#include <string.h>

extern const char* PsyX_Pad_ConnectedControllerName(void);
extern int         Pc_Touch_UsedRecently(void);

#define PH_PULSE_MS  120 /* one buzz; renewed while the game keeps the motors on */
#define PH_RENEW_MS  80

#if defined(SH_IOS)
void Ios_HapticPulse(float strength);
#else
static SDL_Haptic* s_haptic;
static int         s_tried;

/* SDL lists the phone's own vibrator under this name, after any controller's. */
static SDL_Haptic* Ph_Device(void)
{
    int i, n;

    if (s_tried)
        return s_haptic;
    s_tried = 1;
    n = SDL_NumHaptics();
    for (i = 0; i < n; i++)
    {
        const char* name = SDL_HapticName(i);

        if (name != NULL && strcmp(name, "VIBRATOR_SERVICE") == 0)
        {
            s_haptic = SDL_HapticOpen(i);
            break;
        }
    }
    if (s_haptic != NULL && SDL_HapticRumbleInit(s_haptic) != 0)
    {
        SDL_HapticClose(s_haptic);
        s_haptic = NULL;
    }
    SH_DBG("[HAPTIC] phone vibrator: %s (%d haptic devices)", s_haptic ? "ready" : "none", n);
    return s_haptic;
}
#endif

void Pc_PhoneHaptics_Update(void)
{
    static Uint32 s_lastMs;
    static int    s_on;
    const u8*     act   = (const u8*)&g_SysWork.field_2514.actuatorData_4;
    const int     small = act[0] != 0;
    const int     big   = act[1];
    const Uint32  now   = SDL_GetTicks();
    float         strength;

    /* A pad in the hand has its own motors; the phone stays still unless the
     * screen is what is being played on. */
    if ((!small && big == 0) || !g_GameWork.config.vibrationEnabled ||
        (PsyX_Pad_ConnectedControllerName() != NULL && !Pc_Touch_UsedRecently()))
    {
#if !defined(SH_IOS)
        if (s_on && s_haptic != NULL)
            SDL_HapticRumbleStop(s_haptic);
#endif
        s_on = 0;
        return;
    }
    if (s_on && (now - s_lastMs) < PH_RENEW_MS)
        return;

    strength = (float)big / 255.0f;
    if (small && strength < 0.35f)
        strength = 0.35f;
    if (strength < 0.15f)
        strength = 0.15f;

#if defined(SH_IOS)
    Ios_HapticPulse(strength);
#else
    if (Ph_Device() != NULL)
        SDL_HapticRumblePlay(s_haptic, strength, PH_PULSE_MS);
#endif
    s_on     = 1;
    s_lastMs = now;
}

#endif
