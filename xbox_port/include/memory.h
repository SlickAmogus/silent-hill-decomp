/*
 * memory.h - Xbox shim for the legacy <memory.h> header.
 *
 * <memory.h> is an old SVID alias that just exposes the mem* family (memcpy,
 * memset, memmove, memcmp) — historically separate from <string.h>. pdclib has
 * no <memory.h>, so the decomp's `#include <memory.h>` would otherwise resolve
 * to the host Windows 10 SDK's ucrt/memory.h. That drags in MSVC vcruntime.h +
 * vadefs.h, which redefine uintptr_t/intptr_t as `unsigned int`/`int` and clash
 * with pdclib's `unsigned long`/`long`. xbox_port/include is first on the -I
 * path, so this file shadows the host header; forward to pdclib's <string.h>.
 */
#ifndef SH_XBOX_MEMORY_H
#define SH_XBOX_MEMORY_H
#include <string.h>
#endif /* SH_XBOX_MEMORY_H */
