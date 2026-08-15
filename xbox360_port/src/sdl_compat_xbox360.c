/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sdl_compat_xbox360.c - 360 implementations of the thin SDL2 slice the decomp's
 * SH_PC_PORT code uses. Timing maps to the PPC time base; keyboard/mouse are
 * "no input" so the shared input code falls through to the pad path.
 * See xbox_port/include/SDL_timer.h / SDL_scancode.h / SDL_mouse.h.
 */
#include <ppc/timebase.h>

#include "SDL_timer.h"
#include "SDL_mouse.h"
#include "SDL_scancode.h"

Uint32 SDL_GetTicks(void)
{
    return (Uint32)(mftb() / (PPC_TIMEBASE_FREQ / 1000));
}

Uint64 SDL_GetPerformanceCounter(void)   { return (Uint64)mftb(); }
Uint64 SDL_GetPerformanceFrequency(void) { return (Uint64)PPC_TIMEBASE_FREQ; }

void SDL_Delay(Uint32 ms)
{
    /* Busy-wait on the time base, matching the Xbox port: only used for
     * sub-frame pacing slack, so the spin is short, and it avoids depending on
     * a scheduler that does not exist under a bare-metal libXenon app. */
    uint64_t end = mftb() + ((uint64_t)PPC_TIMEBASE_FREQ * ms) / 1000ULL;
    while (mftb() < end) { }
}

/* No keyboard: a permanently-zeroed state array means every
 * g_sdlKeyboardState[SDL_SCANCODE_*] read is "not pressed". */
static const unsigned char s_zeroKeys[SDL_NUM_SCANCODES] = { 0 };
const unsigned char* g_sdlKeyboardState = s_zeroKeys;

Uint32 SDL_GetMouseState(int* x, int* y)          { if (x) *x = 0; if (y) *y = 0; return 0; }
Uint32 SDL_GetRelativeMouseState(int* x, int* y)  { if (x) *x = 0; if (y) *y = 0; return 0; }
int    SDL_SetRelativeMouseMode(SDL_bool enabled) { (void)enabled; return 0; }
