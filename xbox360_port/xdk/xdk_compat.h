/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * xdk_compat.h - force-included into every TU of the XDK build.
 *
 * The shared tree was only ever compiled by gcc and clang. The Xbox 360 XDK
 * compiler is MSVC 16 (VS2010) for PowerPC, whose C mode is C89 plus Microsoft
 * extensions, so the gcc spellings below have to be mapped before the first
 * line of any source is seen. Everything here is a spelling, not a semantic:
 * nothing changes what the code does on the other ports.
 *
 * Two readers see this file:
 *   - cl.exe, through -FI, for every compile.
 *   - clang -E, for the C99->C89 transpile step (tools/c99to89.py), with
 *     SH_C89_TRANSPILE defined. Spellings that only cl understands must stay
 *     out of that reader's view, because its output is parsed as plain C99.
 */
#ifndef SH_XDK_COMPAT_H
#define SH_XDK_COMPAT_H

/* The tree detects byte order through the gcc/clang predefines, and MSVC for
 * PPC defines neither. This is the one define the whole endian strategy
 * hangs on. */
#ifndef __BIG_ENDIAN__
#define __BIG_ENDIAN__ 1
#endif

#define __extension__
#define __attribute__(x)
#define __func__       __FUNCTION__

#define __builtin_expect(e, c) (e)
#define __builtin_unreachable() __assume(0)
#define __builtin_prefetch(...)
#define __builtin_bswap32(x)   _byteswap_ulong(x)
#define __builtin_bswap64(x)   _byteswap_uint64(x)

/* C89 static assert: a negative array size. __COUNTER__ keeps the typedef
 * names unique across headers that assert on the same line number. */
#define SH_XDK_CAT2(a, b) a##b
#define SH_XDK_CAT(a, b)  SH_XDK_CAT2(a, b)
#define static_assert(c, m)  typedef char SH_XDK_CAT(sh_static_assert_, __COUNTER__)[(c) ? 1 : -1]
#define _Static_assert(c, m) static_assert(c, m)

#ifndef SH_C89_TRANSPILE
/* cl.exe only. */
#ifndef __cplusplus
#define inline     __inline
#define __inline__ __inline
#endif
/* Not `restrict` itself: the XDK CRT headers spell __declspec(restrict). */
#define __restrict__ __restrict

/* MSVC 2010 has the underscore forms only. The wrappers below exist because
 * _snprintf does NOT NUL-terminate on truncation, and the tree relies on the
 * C99 guarantee that it does. */
#include <stdio.h>
#include <stdarg.h>
int sh_xdk_snprintf(char* buf, size_t n, const char* fmt, ...);
int sh_xdk_vsnprintf(char* buf, size_t n, const char* fmt, va_list ap);
#define snprintf  sh_xdk_snprintf
#define vsnprintf sh_xdk_vsnprintf
#define strcasecmp  _stricmp
#define strncasecmp _strnicmp

/* The decomp's StringCopy (bodyprog_80055028.c) collides with a StringCopy
 * inside the XDK's xmcore.lib, which XAudio2 drags in. Renamed throughout
 * this build, definition and callers alike. */
#define StringCopy Sh_StringCopy

/* The transpiler's fake stdarg.h spells the va_* macros as these calls so
 * the parser keeps their arguments; here they become the real thing. */
#include <stdarg.h>
#define __sh_va_start(ap, x) va_start(ap, x)
#define __sh_va_end(ap)      va_end(ap)
void* sh_xdk_va_arg_p(va_list* ap, size_t n);   /* xdk_crt.c: the 360 slot walk */
#define __sh_va_arg_p(app, n) sh_xdk_va_arg_p(app, n)
#else
/* clang -E for the transpiler: keep C99 spellings the parser knows. */
#define __inline__   inline
#define __restrict__ restrict
#endif

#endif /* SH_XDK_COMPAT_H */
