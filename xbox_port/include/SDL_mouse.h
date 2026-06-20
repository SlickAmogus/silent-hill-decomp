/*
 * SDL_mouse.h - Xbox shim. SH_PC_PORT debug/aim paths poll the mouse; Xbox has
 * none, so these return "no movement / no buttons" (see sdl_compat_xbox.c) and
 * the game uses the pad path.
 */
#ifndef SH_XBOX_SDL_MOUSE_H
#define SH_XBOX_SDL_MOUSE_H

#include <SDL_stdinc.h>

#define SDL_BUTTON(X)     (1u << ((X) - 1))
#define SDL_BUTTON_LEFT   1
#define SDL_BUTTON_MIDDLE 2
#define SDL_BUTTON_RIGHT  3

#ifdef __cplusplus
extern "C" {
#endif

Uint32 SDL_GetMouseState(int* x, int* y);
Uint32 SDL_GetRelativeMouseState(int* x, int* y);
int    SDL_SetRelativeMouseMode(SDL_bool enabled);

#ifdef __cplusplus
}
#endif

#endif /* SH_XBOX_SDL_MOUSE_H */
