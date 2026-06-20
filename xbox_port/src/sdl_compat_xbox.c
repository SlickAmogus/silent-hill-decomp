/*
 * sdl_compat_xbox.c - Xbox implementations of the thin SDL2 slice the decomp's
 * SH_PC_PORT code uses. Timing maps to the Xbox kernel performance counter;
 * keyboard/mouse are "no input" so the shared input code falls through to the
 * USB-pad path. See SDL_timer.h / SDL_scancode.h / SDL_mouse.h.
 */
#include <xboxkrnl/xboxkrnl.h>
#include "SDL_timer.h"
#include "SDL_mouse.h"
#include "SDL_scancode.h"

Uint32 SDL_GetTicks(void)
{
    return (Uint32)((KeQueryPerformanceCounter() * 1000ULL) / KeQueryPerformanceFrequency());
}

Uint64 SDL_GetPerformanceCounter(void)   { return (Uint64)KeQueryPerformanceCounter(); }
Uint64 SDL_GetPerformanceFrequency(void) { return (Uint64)KeQueryPerformanceFrequency(); }

void SDL_Delay(Uint32 ms)
{
    /* Busy-wait on the perf counter. Only used for sub-frame pacing slack, so
     * the spin is short; avoids pulling in a kernel sleep + its header tangle. */
    ULONGLONG freq = KeQueryPerformanceFrequency();
    ULONGLONG end  = KeQueryPerformanceCounter() + (freq * (ULONGLONG)ms) / 1000ULL;
    while (KeQueryPerformanceCounter() < end) { }
}

/* No keyboard on Xbox: a permanently-zeroed state array means every
 * g_sdlKeyboardState[SDL_SCANCODE_*] read is "not pressed". */
static const unsigned char s_zeroKeys[SDL_NUM_SCANCODES] = { 0 };
const unsigned char* g_sdlKeyboardState = s_zeroKeys;

Uint32 SDL_GetMouseState(int* x, int* y)         { if (x) *x = 0; if (y) *y = 0; return 0; }
Uint32 SDL_GetRelativeMouseState(int* x, int* y) { if (x) *x = 0; if (y) *y = 0; return 0; }
int    SDL_SetRelativeMouseMode(SDL_bool enabled) { (void)enabled; return 0; }
