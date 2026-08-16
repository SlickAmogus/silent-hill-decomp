/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sdl_compat_ps3.c - PS3 implementations of the thin SDL2 slice the decomp's
 * SH_PC_PORT code uses (frame pacing in game_main.c, draw-log throttling,
 * voice-wait timing). Keyboard and mouse report "no input" so the shared input
 * code falls through to the pad path.
 * See xbox_port/include/SDL_timer.h / SDL_scancode.h / SDL_mouse.h.
 *
 * Timing goes through sh_hwperf.h (a bare mftb) and ps3_hal.h rather than
 * PSL1GHT directly, so this TU stays free of <ppu-types.h>.
 */
#include "SDL_timer.h"
#include "SDL_mouse.h"
#include "SDL_scancode.h"

#include "sh_hwperf.h"
#include "ps3_hal.h"

/* Queried once: sysGetTimebaseFrequency is a syscall, and SDL_GetTicks is on
 * the frame-pacing path. */
static unsigned long long TbFreq(void)
{
    static unsigned long long s_freq;
    if (!s_freq)
        s_freq = Ps3_TimebaseFreq();
    return s_freq;
}

Uint32 SDL_GetTicks(void)
{
    return (Uint32)(SH_CYCLES() / (TbFreq() / 1000ULL));
}

Uint64 SDL_GetPerformanceCounter(void)   { return (Uint64)SH_CYCLES(); }
Uint64 SDL_GetPerformanceFrequency(void) { return (Uint64)TbFreq(); }

void SDL_Delay(Uint32 ms)
{
    /* A real sleep, not the 360's time-base spin. GameOS schedules our audio
     * and pad work on lv2 threads, so burning the PPU here would starve them. */
    Ps3_SleepMs((unsigned)ms);
}

/* No keyboard: a permanently-zeroed state array means every
 * g_sdlKeyboardState[SDL_SCANCODE_*] read is "not pressed". */
static const unsigned char s_zeroKeys[SDL_NUM_SCANCODES] = { 0 };
const unsigned char* g_sdlKeyboardState = s_zeroKeys;

Uint32 SDL_GetMouseState(int* x, int* y)          { if (x) *x = 0; if (y) *y = 0; return 0; }
Uint32 SDL_GetRelativeMouseState(int* x, int* y)  { if (x) *x = 0; if (y) *y = 0; return 0; }
int    SDL_SetRelativeMouseMode(SDL_bool enabled) { (void)enabled; return 0; }
