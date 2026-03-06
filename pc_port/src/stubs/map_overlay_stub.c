/*
 * map_overlay_stub.c - Stub for map overlay header when maps are excluded
 *
 * On PSX, g_MapOverlayHeader is defined per-map overlay. Since we're not
 * compiling map overlays yet, provide a zero-initialized stub.
 */
#include "game.h"
#include "bodyprog/bodyprog.h"

s_MapOverlayHeader g_MapOverlayHeader = {0};
