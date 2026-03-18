/*
 * map_dll_fixups.h — Workarounds for Windows DLL initializer limitations.
 *
 * Force-included (-include) in all map DLL compilations.
 * Currently handles <stdio.h> for PC port debug logging in shared headers.
 */
#ifndef MAP_DLL_FIXUPS_H
#define MAP_DLL_FIXUPS_H

#ifdef SH_MAP_DLL
#include <stdio.h>
#endif

#endif /* MAP_DLL_FIXUPS_H */
