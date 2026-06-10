/* MANUALLY MAINTAINED — do NOT regenerate via extract_map_data.py.
 * Source: disc_extract/VIN/MAP0_S00.BIN
 * Map: map0_s00
 *
 * Per-map cutscene data extracted from the original PSX map overlay binary.
 * In the upstream decomp these are `extern` declared in include/maps/map0/map0_s00.h
 * but their data lives only in the PSX overlay binary, not in C source.
 * This file provides them as local definitions so the map DLL is self-contained.
 *
 * NOTE: this file was originally auto-generated, but was edited by hand to add
 * D_800DFAC4 / D_800DFAC8 / D_800DFB61 — symbols whose data lives inside
 * g_Cutscene_MapMsgAudioCmds on PSX (BSS aliasing) but the decomp split them
 * into separate names. D_800DFAC4 in particular is the alley camera warp
 * flag — when zero (the prior stub default) the chase-scene camera fails to
 * warp into the alley and shows void/stale framebuffer. Re-running
 * extract_map_data.py would CLOBBER these additions; don't do it.
 */

#include <assert.h>  /* C11 static_assert macro */
#include "common.h"
#include "game.h"  /* VECTOR3 */

// 0x800DF2F8  size 0x8 (8 bytes) — BGM layer maximum-volume caps for the
// Cheryl chase / alley sequence. All 0x80 (128) = all layers can play at
// 100% if their gating event flag is set. Same as the global default,
// but Map_RoomBgmInit explicitly passes &D_800DF2F8 so we have to carry
// the data even though it's identical content.
u8 D_800DF2F8[8] = { 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80 };

// 0x800DF300  s16 array — BGM layer-to-EventFlag mapping table for map0_s00.
// Indexed by layer (1..6) in Map_RoomBgmInit; the loop turns layer i on
// IFF Savegame_EventFlagGet(D_800DF300[i]) is true. Without this real
// data the prior u8[256] zero-stub made every layer gate on EventFlag_0
// (the "any flag set" garbage) so all layers were either always on or
// always off depending on first byte. Extracted from MAP0_S00.BIN at
// file offset 0x15D88. Header declares as `extern s16 D_800DF300[]`.
//
// Layer mapping (alley progression, per the chase script's flag-set
// pattern in map0_s00_2.c):
//   [0] = 0   (sentinel — layer 0 not gated this way)
//   [1] = 3   (EventFlag_3:  initial layer — set early)
//   [2] = 20  (EventFlag_20: alley1 entry)
//   [3] = 22  (EventFlag_22: alley2 progression)
//   [4] = 23  (EventFlag_23: alley2 deeper)
//   [5] = 21  (EventFlag_21: alley3 entry / dark area)
//   [6] = 24  (EventFlag_24: alley3 grey-children spawn point)
s16 D_800DF300[16] = {
    0, 3, 20, 22, 23, 21, 24, 0,
    0, 0, 0, 0, 0, 0, 0, 0
};

// 0x800DFAB4  size 0x8 (8 bytes)
s32 g_Cutscene_Timer = -1;

// 0x800DFABC  size 0x1C (28 bytes) — main voice cmd table for cheryl chase.
// D_800DFAC4 / D_800DFAC8 below alias into this on PSX.
u16 g_Cutscene_MapMsgAudioCmds[14] = {
    0x100D, 0x1023, 0x1024, 0x0001, 0x0001, 0x0000, 0x101E, 0x1017, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000
};

// 0x800DFAC4  s32 — alley camera warp flag (used by chase scene).
// PSX initializes to 1 from binary so the first frame of the alley sequence
// performs Camera_PositionSet(...,warp=true). After that, code sets it to 0.
// With the prior zero-init stub, the warp never fired and the camera stayed
// at its previous position — that's the "alley cameras glitched, void/partial
// view" symptom on the FIRST chase camera. Subsequent waypoints are gated on
// g_WarpCamera below.
s32 D_800DFAC4 = 1;

// 0x800DFAD8  s32 — chase corridor camera warp flag (separate from D_800DFAC4).
// Same story as D_800DFAC4: starts at 1 in binary, code toggles it as Harry
// crosses chase-section boundaries. With the zero stub, the second/third
// corridor cameras (alley deep + corner) never warped into place either,
// leaving the camera stuck at the previous waypoint and clipping into walls.
s32 g_WarpCamera = 1;

// 0x800DFAC8  u16[] — voice cmd table for "What is this?" / "Hey wait, stop!".
// Aliases into g_Cutscene_MapMsgAudioCmds[6..]; PC duplicates as a
// contiguous u16 array since Map_MessageWithAudio reads audioCmds[idx]
// for indices > 0. Header declares as single u16; the array def works
// because the linker matches by name, not by type. The 4 trailing zeros
// give Map_MessageWithAudio idle-state safety if audioIdx overruns.
u16 D_800DFAC8[6] = { 0x101E, 0x1017, 0x0000, 0x0000, 0x0000, 0x0000 };

// 0x800DF1FC  size 0xE0 (224 bytes) — fallback room-index grid for
// Map_RoomIdxGet: 14 x-cells * 16 z-cells of 40.0 world units, covering
// X [-320,240) / Z [-240,400). The exe zero-stub made mapRoomIdx always 0
// in the streets, killing per-room BGM/ambience gating. This file is linked
// into the EXE (map0_s00 is static), so the data_stubs.c stub was removed.
u8 MAP_ROOM_IDXS[224] = {
    0x00, 0x1B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x1B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x19, 0x19, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x00, 0x17, 0x00, 0x20,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x16, 0x16, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x28, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x22, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1C, 0x00, 0x03, 0x24, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F,
    0x00, 0x00, 0x00, 0x1D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x25, 0x00, 0x26, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x06, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x25, 0x00, 0x26, 0x00, 0x1E,
    0x00, 0x00, 0x00, 0x21, 0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x21, 0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x23, 0x00, 0x00, 0x27, 0x00
};

// 0x800DF2DC  size 0x19 (25 bytes) — secondary (primary-priority) street grid
// for MAP_HAS_SECONDARY_GRID maps, indexed (GetXIdx(x)*5)+GetYIdx(x,z), both
// 0..4. Rooms 0x01..0x15 = the named street blocks; fallback grid above
// handles everything this misses. Stub removed from data_stubs.c (exe link).
u8 sharedData_800DF2DC_0_s00[25] = {
    0x00, 0x05, 0x06, 0x07, 0x08, 0x01, 0x09, 0x0A, 0x0B, 0x0C, 0x02, 0x0D, 0x00, 0x0E, 0x0F, 0x03,
    0x00, 0x10, 0x11, 0x12, 0x04, 0x00, 0x13, 0x14, 0x15
};

// 0x800DFB58  size 0xC (12 bytes)
u8 g_Cutscene_MapMsgAudioIdx = 0x00;

// 0x800DFB61  u8 — separate audio idx counter, all zero in binary.
u8 D_800DFB61 = 0;

