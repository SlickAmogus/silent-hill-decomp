/* split_head support funcs (sharedFunc_800CAAD0_1_s05 family) — compiled
 * per-map in retail hosts, absent from the exe import lib.
 * unk_draw.c is only compilable as one of its three host variants (the map
 * define selects the map header its shared-data types/sizes come from), so
 * this TU builds the map1_s05 variant — the split_head host. The define is
 * TU-local: nothing else is compiled in this file. */
#define MAP1_S05

#include "../src/maps/unk_draw.c"
