/*
 * SDL_timer.h - Xbox shim for the SDL2 timer slice used by SH_PC_PORT code
 * (frame pacing in game_main.c, draw-log throttling, voice-wait timing).
 * Backed by the Xbox kernel performance counter in sdl_compat_xbox.c.
 */
#ifndef SH_XBOX_SDL_TIMER_H
#define SH_XBOX_SDL_TIMER_H

#include <SDL_stdinc.h>

#ifdef __cplusplus
extern "C" {
#endif

Uint32 SDL_GetTicks(void);                 /* milliseconds since boot */
void   SDL_Delay(Uint32 ms);
Uint64 SDL_GetPerformanceCounter(void);
Uint64 SDL_GetPerformanceFrequency(void);

#ifdef __cplusplus
}
#endif

#endif /* SH_XBOX_SDL_TIMER_H */
