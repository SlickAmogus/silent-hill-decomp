/* MANUALLY MAINTAINED — do NOT regenerate via extract_map_data.py.
 * Source: disc_extract/VIN/MAP0_S01.BIN
 * Map: map0_s01
 *
 * Per-map cutscene data extracted from the original PSX map overlay binary.
 * In the upstream decomp these are `extern` declared in include/maps/map0/map0_s01.h
 * but their data lives only in the PSX overlay binary, not in C source.
 * This file provides them as local definitions so the map DLL is self-contained.
 *
 * NOTE: this file was originally auto-generated, but was edited by hand to add
 * D_800DE124, D_800DE128, D_800DE12C, D_800DE140, D_800DE154 — symbols whose
 * data lives inside g_Cutscene_MapMsgAudioCmds on PSX (BSS aliasing) but the
 * decomp split into separate names. Without these, AirScreamer cutscene voice
 * lines fire SD_Call(0) and the radio static loop has no spatial position.
 * Re-running extract_map_data.py would CLOBBER these additions — don't do it,
 * or fold them into TARGETS first.
 */

#include <assert.h>  /* C11 static_assert macro */
#include "common.h"
#include "game.h"             /* VECTOR3 */
#include "bodyprog/bodyprog.h" /* s_AnmHeader, s_WorldObjectModel etc. (shared.h prereqs) */
#include "main/fsqueue.h"     /* s_FsImageDesc (background_draw.h prereq) */
#include "maps/shared.h"      /* s_WorldObjectPose (D_800DE12C/D_800DE140) */

// 0x800DE0CC  size 0x94 (148 bytes) — main voice cmd table for cafe cutscene
// This is the parent block; D_800DE124/D_800DE128 below alias into it on PSX.
u16 g_Cutscene_MapMsgAudioCmds[74] = {
    0x101F, 0x1022, 0x1001, 0x1028, 0x1003, 0x1010, 0x1009, 0x101D, 0x101B, 0x1030, 0x1019, 0x1008,
    0x102F, 0x1033, 0x1015, 0x1021, 0x1018, 0x1007, 0x1027, 0x1026, 0x102E, 0x1034, 0x102C, 0x102D,
    0x100F, 0x100A, 0x1016, 0x1032, 0x1012, 0x102B, 0x101A, 0x1006, 0x1014, 0x1029, 0x1020, 0x100B,
    0x1025, 0x100E, 0x1013, 0x1011, 0x1031, 0x101C, 0x1002, 0x0000, 0x102A, 0x1004, 0x1005, 0x0000,
    0x5199, 0x0000, 0xF000, 0xFFFF, 0x1B33, 0x0011, 0x0000, 0x024F, 0x0000, 0x0000, 0x4F33, 0x0000,
    0xF000, 0xFFFF, 0xB800, 0x0010, 0x0000, 0x0311, 0x0000, 0x0000, 0x24CC, 0x0000, 0xF667, 0xFFFF,
    0x1E66, 0x0011
};

// 0x800DE124  4 u16s — voice cmd table for AirScreamer cutscene (parts 0-1).
// PSX has this aliased into g_Cutscene_MapMsgAudioCmds[44..47]; on PC we just
// duplicate the data so the symbol exists locally in this DLL.
// Entries: 0x102A "What's that?", 0x1004 "Huh? Radio?", 0x1005 "This is not a dream!", 0
const u16 D_800DE124[4] = { 0x102A, 0x1004, 0x1005, 0x0000 };

// 0x800DE128  2 u16s — voice cmd table for AirScreamerDeath cutscene.
// PSX aliases this into g_Cutscene_MapMsgAudioCmds[46..47]; PC duplicates.
const u16 D_800DE128[2] = { 0x1005, 0x0000 };

// 0x800DE12C  s_WorldObjectPose — health drink #0 placement.
// PSX aliases into g_Cutscene_MapMsgAudioCmds[48..]; PC stores as proper struct.
s_WorldObjectPose D_800DE12C = {
    .position   = { 0x00005199, (s32)0xFFFFF000, 0x00111B33 },
    .rotation_C = { 0, 0x024F, 0 },
};

// 0x800DE140  s_WorldObjectPose — health drink #1 placement.
s_WorldObjectPose D_800DE140 = {
    .position   = { 0x00004F33, (s32)0xFFFFF000, 0x0010B800 },
    .rotation_C = { 0, 0x0311, 0 },
};

// 0x800DE154  VECTOR3 — radio static/interference loop position (cafe cutscene).
// Passed to func_8005DE0C for spatial SFX placement.
VECTOR3 D_800DE154 = { 0x000024CC, (s32)0xFFFFF667, 0x00111E66 };

// 0x800E2380  size 0x10 (16 bytes)
VECTOR3 g_Cutscene_CameraPositionTarget = { 0, 0, 0 };

// 0x800E2390  size 0xC (12 bytes)
VECTOR3 g_Cutscene_CameraLookAtTarget = { 0, 0, 0 };

// 0x800E239C  size 0x4 (4 bytes)
s32 g_Cutscene_Timer = 0;

// 0x800E23A0  size 0xF0 (240 bytes)
u8 g_Cutscene_MapMsgAudioIdx = 0x00;

