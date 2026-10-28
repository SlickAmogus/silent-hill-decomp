/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sdl_compat_xdk.cpp - the thin SDL2 slice the decomp's SH_PC_PORT code uses,
 * on the XDK. Timing maps to the PPC time base; keyboard/mouse are "no input"
 * so the shared input code falls through to the pad path.
 * See xbox_port/include/SDL_timer.h / SDL_scancode.h / SDL_mouse.h.
 */
#include <xtl.h>
#include <ppcintrinsics.h>

extern "C" {
#include "SDL_timer.h"
#include "SDL_mouse.h"
#include "SDL_scancode.h"
}

/* The time base runs at 50 MHz regardless of core clock. */
#define TB_FREQ 50000000ULL

extern "C" Uint32 SDL_GetTicks(void)
{
    return (Uint32)(__mftb() / (TB_FREQ / 1000));
}

extern "C" Uint64 SDL_GetPerformanceCounter(void)   { return (Uint64)__mftb(); }
extern "C" Uint64 SDL_GetPerformanceFrequency(void) { return (Uint64)TB_FREQ; }

extern "C" void SDL_Delay(Uint32 ms)
{
    Sleep(ms);
}

/* No keyboard: a permanently-zeroed state array means every
 * g_sdlKeyboardState[SDL_SCANCODE_*] read is "not pressed". */
static const unsigned char s_zeroKeys[SDL_NUM_SCANCODES] = { 0 };
extern "C" const unsigned char* g_sdlKeyboardState = s_zeroKeys;

extern "C" Uint32 SDL_GetMouseState(int* x, int* y)          { if (x) *x = 0; if (y) *y = 0; return 0; }
extern "C" Uint32 SDL_GetRelativeMouseState(int* x, int* y)  { if (x) *x = 0; if (y) *y = 0; return 0; }
extern "C" int    SDL_SetRelativeMouseMode(SDL_bool enabled) { (void)enabled; return 0; }
