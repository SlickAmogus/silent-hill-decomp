/*
 * SDL_stdinc.h - Xbox shim: the few SDL scalar types the decomp's SH_PC_PORT
 * paths reference. The PC port reads input/timing through a thin slice of the
 * SDL2 API; on Xbox we satisfy that slice with kernel-timer + "no input" stubs
 * (see sdl_compat_xbox.c) so the shared SH_PC_PORT code compiles unchanged.
 */
#ifndef SH_XBOX_SDL_STDINC_H
#define SH_XBOX_SDL_STDINC_H

typedef unsigned char      Uint8;
typedef unsigned short     Uint16;
typedef unsigned int       Uint32;
typedef unsigned long long Uint64;

typedef enum { SDL_FALSE = 0, SDL_TRUE = 1 } SDL_bool;

#endif /* SH_XBOX_SDL_STDINC_H */
