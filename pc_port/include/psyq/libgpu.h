/* PSY-Q to PsyCross compatibility shim */
#ifndef _PSYQ_COMPAT_LIBGPU_H
#define _PSYQ_COMPAT_LIBGPU_H
#include <libgpu.h>

#ifdef SH_XBOX_PORT
/* PsyCross setaddr macro-precedence fix (upstream PsyCross ed2853e), applied
 * in the shim because the Xbox build pins an older PsyCross checkout. The
 * unparenthesized cast in (u_int*)_addr binds BEFORE any arithmetic inside
 * the argument, so addPrim(ot, *poly + 1) linked *poly + 4 BYTES (u_int*
 * arithmetic) instead of the second prim — corrupting the OT whenever the
 * layered blood emit (bodyprog_8005E0DC.c, restored in 93f286416 relying on
 * exactly this fix) passed prim EXPRESSIONS. Symptom on hardware: the player
 * and nearby world geometry vanish for a few frames whenever blood sprays
 * (shooting, monster hits). Parenthesizing _addr makes the argument's own
 * pointer arithmetic happen first, in the caller's prim type. */
#undef setaddr
#if USE_EXTENDED_PRIM_POINTERS
#define setaddr(p, _addr)   (((P_TAG *)(p))->addr = (uintptr_t)((u_int*)(_addr)))
#else
#define setaddr(p, _addr)   (((P_TAG *)(p))->addr = (u_int)((u_int*)(_addr)))
#endif
#endif /* SH_XBOX_PORT */

#endif
