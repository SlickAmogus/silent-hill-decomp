/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * map_static_registry.h — overlay lookup for the statically linked map build.
 *
 * Built when SH_STATIC_MAPS is on (iOS, and anything else that will not load a
 * loose dynamic library). Each overlay is compiled to its own archive whose
 * defined symbols were renamed to <map>_<sym> by tools/prefix_map_syms.py, so
 * all 43 can coexist in one binary despite each carrying its own copy of the
 * shared AI/particle/player code. The table itself is generated into the build
 * directory by pc_port/CMakeLists.txt.
 */
#ifndef MAP_STATIC_REGISTRY_H
#define MAP_STATIC_REGISTRY_H

#include "game.h"
#include "bodyprog/bodyprog.h"

typedef struct
{
    const char*      name;
    s_MapOverlayHdr* header;
} s_StaticMapEntry;

/* Returns the overlay header linked in under `name`, or NULL if this build has
 * no such overlay. */
s_MapOverlayHdr* MapStatic_Find(const char* name);

#endif /* MAP_STATIC_REGISTRY_H */
