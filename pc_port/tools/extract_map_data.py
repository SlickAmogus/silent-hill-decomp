#!/usr/bin/env python3
"""
Extract per-map cutscene voice/timer/camera data from original PSX map binaries.

Reads:
  configs/USA/maps/sym.map*.txt — symbol address tables
  disc_extract/VIN/MAP*.BIN     — original PSX map binaries

Generates:
  pc_port/build_gen/extracted_data/{map}_data.c — one C file per map with
  the extracted data as static initializers.

These are the symbols whose data lives in the PSX map binary but is
declared `extern` only in the decomp source. Without this data, voice
playback is silent and cutscene state machines have zeroed timers.
"""

import os
import re
import struct
import sys
from pathlib import Path

# ---------- config ----------

REPO_ROOT = Path(__file__).resolve().parents[2]
SYM_DIR   = REPO_ROOT / "configs" / "USA" / "maps"
VIN_DIR   = REPO_ROOT.parent / "disc_extract" / "VIN"
OUT_DIR   = REPO_ROOT / "pc_port" / "build_gen" / "extracted_data"

# Symbol -> (C type, fallback element width if size unknown)
# Width is bytes; arrays use this for size inference when next-symbol gap is small.
TARGETS = {
    # voice
    "g_Cutscene_MapMsgAudioCmds":       ("u16",   2),
    "g_Cutscene_MapMsgAudioCmds0":      ("u16",   2),
    "g_Cutscene_MapMsgAudioCmds1":      ("u16",   2),
    "g_Cutscene_MapMsgAudioCmds2":      ("u16",   2),
    "g_Cutscene_MapMsgAudioIdx":        ("u8",    1),
    "g_Cutscene_MapMsgAudioIdx0":       ("u8",    1),
    "g_Cutscene_MapMsgAudioIdx1":       ("u8",    1),
    "g_Cutscene_MapMsgAudioIdx2":       ("u8",    1),
    # timers (scalars)
    "g_Cutscene_Timer":                 ("s32",   4),
    "g_Cutscene_Timer0":                ("s32",   4),
    "g_Cutscene_Timer1":                ("s32",   4),
    "g_Cutscene_Timer2":                ("s32",   4),
    "g_Cutscene_Timer3":                ("s32",   4),
    # cutscene camera (VECTOR3 = 3x s32)
    "g_Cutscene_CameraLookAt":          ("VECTOR3", 12),
    "g_Cutscene_CameraPosition":        ("VECTOR3", 12),
    "g_Cutscene_CameraLookAtTarget":    ("VECTOR3", 12),
    "g_Cutscene_CameraPositionTarget":  ("VECTOR3", 12),
    # door-of-eclipse lock TIM file indices (map2_s00) — 8 entries, indexed
    # by (LionOpen?4:0)+(WoodmanOpen?2:0)+(ScarecrowOpen?1:0). Without this
    # the eclipse-door key insertion crashes on step 6 (Fs_QueueStartReadTim
    # with file enum 0 from the all-zero stub).
    "g_Gfx_LockTimFileIdxs":            ("s16",   2),
    # per-map inventory item ID list (null-terminated u8 array).
    # MapOverlayHeader.loadableItems_2C points here; Gfx_Items_Draw uses it to
    # match inventory items to TMD model slots.  Without a local definition the
    # DLL inherits map0_s00's 8-item EXE export, causing all inventory items on
    # every other map to render with stale (wrong) 3-D models.
    "LOADABLE_INVENTORY_ITEMS":         ("u8",    1),
    # per-map world item pose array (s_WorldObjectPose[] — position + rotation).
    # Declared extern in map headers; data lives only in the PSX overlay binary.
    # Without a local definition all world pickups spawn at (0,0,0) and are
    # invisible because they're behind the camera from the player spawn point.
    "g_CommonWorldObjectPoses":         ("s_WorldObjectPose", 20),
    # BGM layer tables. Two kinds, both fed to Bgm_Update:
    #   - layer limits (s_BgmLayerLimits = u8[8]): per-room max-volume caps,
    #     applied as (vol * limit) >> 7. An all-zero stub mutes EVERY layer
    #     regardless of the flag bits -> map BGM is silent (school bug).
    #   - room flag tables (u16[roomCount]): which layers a room enables.
    "sharedData_800E1208_1_s02":        ("u8",  1),  # map1_s02/s03 limits
    "sharedData_800F06D4_2_s00":        ("u8",  1),  # map2_s00/s03 limits
    "sharedData_800ED43C_2_s02":        ("u8",  1),  # map2_s02, map4_s00/s06 limits
    "sharedData_800ED418_4_s02":        ("u8",  1),  # map4_s02/s03/s05 limits
    "sharedData_800ED42C_4_s02":        ("u16", 2),  # map4_s02/s03/s05 room flags
    "sharedData_800EFCFC_6_s00":        ("u8",  1),  # map6_s00/s02 limits
    "sharedData_800D1D14_3_s02":        ("u8",  1),  # map3_s02..s05, map4_s04 limits
    "sharedData_800D1D1C_3_s02":        ("u16", 2),  # map3_s02..s05, map4_s04 room flags
    "sharedData_800EB738_6_s04":        ("u8",  1),  # map6_s04/s05 limits (3 tables)
    "sharedData_800EB740_6_s04":        ("u8",  1),
    "sharedData_800EB748_6_s04":        ("u8",  1),
    "sharedData_800D2F18_7_s00":        ("u8",  1),  # map7_s00..s03 limits (5 tables)
    "sharedData_800D2F20_7_s00":        ("u8",  1),
    "sharedData_800D2F74_7_s00":        ("u8",  1),
    "sharedData_800D2F7C_7_s00":        ("u8",  1),
    "sharedData_800D2F84_7_s00":        ("u8",  1),
    # D_*-named BGM tables (addresses encoded in the names; not listed in the
    # sym files, so they enter through EXTRA_SYMBOLS below).
    # map4_s05 Floatstinger boss rodata cluster (raw u8 keeps the mixed
    # struct layouts byte-exact; the DLL externs re-type them).
    "D_800D780C":                       ("u8",  1),  # flight boundary boxes
    "D_800D7848":                       ("u8",  1),  # bone index list
    "D_800D7A20":                       ("u8",  1),  # SVECTOR[2]
    "D_800D7A30":                       ("u8",  1),  # SVECTOR
    "D_800D7A38":                       ("u8",  1),  # s16[16] wing amplitudes
    "D_800D7A58":                       ("u8",  1),  # bone index list
    "D_800D7A5C":                       ("u8",  1),  # SVECTOR[3]
    # map4_s03 TV-bank static/sigil effect cluster (raw u8, byte-exact).
    "D_800DB7C8":                       ("u8",  1),  # WorldGfx object ref
    "D_800DB7D4":                       ("u8",  1),  # big TV quad corners
    "D_800DB7E4":                       ("u8",  1),  # 3x3 TV grid quad corners
    "D_800DB874":                       ("u8",  1),  # pattern tpage/clut/UV table
    "D_800DB924":                       ("u8",  1),  # sign position SVECTOR3
    "D_800DCC4C":                       ("u8",  1),  # map1_s00 limits
    "D_800DCC54":                       ("u16", 2),  # map1_s00 room flags
    "D_800DC9FC":                       ("u8",  1),  # map1_s01 limits
    "D_800DCA04":                       ("u16", 2),  # map1_s01 room flags
    "D_800CCF54":                       ("u8",  1),  # map1_s04 limits
    "D_800D5C3C":                       ("u8",  1),  # map1_s05 limits
    "D_800D71E8":                       ("u8",  1),  # map1_s06 limits
    "D_800DA570":                       ("u8",  1),  # map5_s00 limits
    "D_800DA578":                       ("u16", 2),  # map5_s00 room flags
    "D_800EFC74":                       ("u8",  1),  # map5_s01 limits
    "D_800DBCDC":                       ("u8",  1),  # map6_s03 limits
    # Per-map fallback room-index grid read by Map_RoomIdxGet (shared header).
    # Maps room positions to room indices for BGM room flags, ambience, etc.
    # Without it mapRoomIdx is always 0 game-wide (exe stub is all zeros).
    "MAP_ROOM_IDXS":                    ("u8",  1),
    # Secondary street grid for MAP_HAS_SECONDARY_GRID maps (map0_s00/s01,
    # map2_s00/s03). Listed here (not just auto-discovered) because its
    # data_stubs.c stub was removed once real data landed.
    "sharedData_800DF2DC_0_s00":        ("u8",  1),
    # Romper movement scalars (romper.c). Auto-discovery skipped these because
    # gap inference over-extends into neighbouring pointer tables; they are
    # plain scalars: walk speed Q12(8.0), lunge speed Q12(6.5), anim base 109.
    # Zero stubs made Romper moveDist 0 in chase/lunge states.
    "sharedData_800ECA4C_2_s02":        ("s32", 4),
    "sharedData_800ECACC_2_s02":        ("s32", 4),
    "sharedData_800ECBD0_2_s02":        ("s16", 2),
    # Cutscene voice-command tables (Event_DisplayMapMsgWithAudio /
    # Map_MessageWithAudio third arg): u16 SD_Call commands (0x1xxx = XA
    # voice line), one per dialogue page. All 32 were zero stubs — SD_Call(0)
    # is a no-op, so every cutscene using a raw D_*-named table played NO
    # voices, while maps whose table got the g_Cutscene_MapMsgAudioCmds name
    # were extracted and worked. Headers declare these as scalar `extern
    # u16`, so auto-discovery never saw an array; they enter via
    # EXTRA_SYMBOLS like the BGM limit tables.
    "D_800D24F0":                       ("u16", 2),  # map3_s00 voices
    "D_800D26D0":                       ("u16", 2),  # map3_s06 voices
    "D_800D31C4":                       ("u16", 2),  # map7_s00 voices
    "D_800D3734":                       ("u16", 2),  # map4_s04 voices
    "D_800D3778":                       ("u16", 2),  # map4_s04 voices
    "D_800D3B40":                       ("u16", 2),  # map6_s02 voices
    "D_800D3B6C":                       ("u16", 2),  # map6_s02 voices
    "D_800D3BDC":                       ("u16", 2),  # map5_s03 voices
    "D_800D4108":                       ("u16", 2),  # map6_s01 voices
    "D_800D599C":                       ("u16", 2),  # map3_s04 voices
    "D_800D6B54":                       ("u16", 2),  # map3_s03 voices
    "D_800D775C":                       ("u16", 2),  # map1_s06 voices
    "D_800D7D74":                       ("u16", 2),  # map4_s05 voices
    "D_800D947C":                       ("u16", 2),  # map5_s02 voices (Dahlia)
    "D_800DAC70":                       ("u16", 2),  # map3_s05 voices
    "D_800DB92C":                       ("u16", 2),  # map4_s03 voices
    "D_800E14E8":                       ("u16", 2),  # map7_s01 voices
    "D_800E1500":                       ("u16", 2),  # map7_s01 voices
    "D_800E9CE4":                       ("u16", 2),  # map7_s02 voices
    "D_800E9CFC":                       ("u16", 2),  # map7_s02 voices
    "D_800E9D30":                       ("u16", 2),  # map7_s02 voices
    "D_800E9D50":                       ("u16", 2),  # map7_s02 voices
    "D_800EBA34":                       ("u16", 2),  # map6_s04 voices
    "D_800EBA64":                       ("u16", 2),  # map6_s04 voices
    "D_800ED768":                       ("u16", 2),  # map7_s03 voices
    "D_800ED77C":                       ("u16", 2),  # map7_s03 voices
    "D_800ED7B4":                       ("u16", 2),  # map7_s03 voices
    "D_800ED88C":                       ("u16", 2),  # map7_s03 voices
    "D_800ED898":                       ("u16", 2),  # map7_s03 voices
    "D_800ED9B4":                       ("u16", 2),  # map7_s03 voices
    "D_800F0038":                       ("u16", 2),  # map6_s00 voices
    # INCLUDE_RODATA symbols that are no-op on PC -> zero-stubs (same class as
    # the Cybil DMS strings). Pure data (positions/RECT/imageDesc), raw bytes,
    # consumer casts via its extern type. map6_s02 lighthouse effect + map6_s04
    # Cybil-boss positions.
    "D_800CAB90":                       ("u8", 8),   # map6_s02 LHEFFECT TIM imageDesc
    "D_800CAB98":                       ("u8", 8),   # map6_s02 LHEFFECT StoreImage RECT
    "D_800CC7C0":                       ("u8", 12),  # map6_s04 boss SFX pos (VECTOR3)
    "D_800CC7CC":                       ("u8", 12),  # map6_s04 boss SFX pos (VECTOR3)
    "D_800CB728":                       ("u8", 12),  # map6_s04 VECTOR3
    # Remaining INCLUDE_RODATA stubs — SFX 3D positions / object rotations /
    # anim data (all pointer-free, raw bytes, consumer casts via extern type).
    "D_800CB0CC":                       ("u8", 12),  # map5_s00 SFX pos (VECTOR3)
    "D_800CB370":                       ("u8", 12),  # map3_s04 SFX pos (VECTOR3)
    "D_800CC984":                       ("u8", 12),  # map7_s01 SFX pos (VECTOR3)
    "D_800CC998":                       ("u8", 12),  # map7_s01 SFX pos (VECTOR3)
    "D_800CC990":                       ("u8", 8),   # map7_s01 obj rotation (SVECTOR3)
    "D_800CB61C":                       ("u8", 8),   # map7_s00 obj rotation (SVECTOR3)
    "D_800CA788":                       ("u8", 8),   # map4_s03 twinfeeler (SVECTOR)
    # D_800CC424 (s_AnimInfo[8]) is NOT extractable raw: s_AnimInfo has function
    # pointers (playbackFunc + variableFunc union), so the PSX 16-byte layout is
    # invalid on 64-bit (extracted 0x80044CA4 = dead PSX ptr). Needs pointer
    # reformat; left zero-stubbed.
    "D_800CC4A4":                       ("u8", 0x20),# map6_s04 field_38 (s_UnkStruct3_Mo[4], pointer-free)
    # map6_s00 otherworld-transition per-cell thresholds, DVECTOR[17][17].
    # Raw u8 bytes; consumer indexes through the header's DVECTOR extern.
    "D_800F0084":                       ("u8", 0x484),
    "D_800F0174":                       ("u16", 2),  # map5_s01 voices
    # Cursor-click puzzle data (keypads/dials): button hit rects, answer
    # codes, layout RECTs, sfx positions. All were zero stubs — the cursor
    # moved but no click could land inside an all-zero rect (map5_s01 door
    # keypad reported; map3_s03 / map7_s01 / map7_s02 share the pattern).
    "D_800F0158":                       ("u8",  1),  # map5_s01 keypad rects
    "D_800F0170":                       ("u8",  1),  # map5_s01 door code
    "D_800D6B40":                       ("u8",  1),  # map3_s03 keypad rects
    "D_800D6B50":                       ("u8",  1),  # map3_s03 code
    "D_800E1504":                       ("u8",  1),  # map7_s01 sfx pos (VECTOR3)
    "D_800E1510":                       ("u8",  1),  # map7_s01 keypad rects
    "D_800E1544":                       ("u8",  1),  # map7_s01 entry buffer init
    "D_800E154C":                       ("u16", 2),  # map7_s01 table
    "D_800E155C":                       ("u16", 1),  # map7_s01 astrology puzzle solution
    "D_800E1560":                       ("s16", 7),  # map7_s01 astrology panel BG texture IDs
    "D_800E1680":                       ("u8",  1),  # map7_s01 puzzle state init
    "D_800E1688":                       ("u8",  1),  # map7_s01 answer code
    "D_800E168D":                       ("u8",  1),  # map7_s01
    "D_800E168E":                       ("u16", 2),  # map7_s01 (q4_12)
    "D_800E1690":                       ("u8",  1),  # map7_s01
    "D_800E9D00":                       ("u8",  1),  # map7_s02 sfx pos (VECTOR3)
    "D_800E9D0C":                       ("u8",  1),  # map7_s02 layout RECTs
    "D_800E9D1C":                       ("u8",  1),  # map7_s02 layout RECT
    "D_800E9D24":                       ("u16", 2),  # map7_s02 table
    "D_800E9D2C":                       ("u16", 2),  # map7_s02
    "D_800E9D6C":                       ("u16", 2),  # map7_s02 table
    "D_800E9D7C":                       ("u16", 2),  # map7_s02
    "D_800E9D80":                       ("s16", 2),  # map7_s02 table
    "D_800E9D8E":                       ("u8",  1),  # map7_s02
    "D_800E9E1C":                       ("u8",  5),  # map7_s02 keypad puzzle solution
    "D_800EC770":                       ("u16", 20), # map7_s03 boss hit-SFX descriptors (s_800EC770[5])
    "D_800DB210":                       ("s32", 4),  # map4_s03 twinfeeler bounding box
    "D_800D3C2C":                       ("u16", 15), # map6_s02 threshold table
    "D_800EA776":                       ("s16", 1),  # MonsterCybil keyframe triggers
    "D_800EA7D4":                       ("s16", 1),
    "D_800EA7D6":                       ("s16", 1),
    "D_800EA816":                       ("s16", 1),
    "D_800EA836":                       ("s16", 1),
    "D_800EA856":                       ("s16", 1),
    "D_800EA894":                       ("s16", 1),
    "D_800EA896":                       ("s16", 1),
    "D_800EBAAC":                       ("s32", 10),  # map6_s04 carousel horse X offsets
    "D_800EBAD4":                       ("s32", 10),  # map6_s04 carousel horse Z offsets
    "D_800EBAFC":                       ("s16", 10),  # map6_s04 carousel horse angles
    "D_800E9DE8":                       ("u8",  1),  # map7_s02 keypad rects
}

# MAP_ROOM_IDXS byte size = strideX * strideZ, stride = (MAX - MIN) / CHUNK_CELL_SIZE
# with CHUNK_CELL_SIZE = Q12(40.0), from each map's MAP_ROOM_MIN/MAX_X/Z macros in
# include/maps/mapN/mapN_sNN.h. Cross-checked against the sym files that annotate
# a size (map0_s02/map2_s01/map2_s04/map4_s01 all say size:0x1E = 10*3).
# map6_s04/map6_s05 compute room idx analytically and have no table.
MAP_ROOM_IDXS_SIZE = {
    "map0_s00": 14 * 16, "map0_s01": 14 * 16, "map0_s02": 10 * 3,
    "map1_s00": 8 * 7,   "map1_s01": 8 * 7,   "map1_s02": 8 * 7,
    "map1_s03": 8 * 7,   "map1_s04": 8 * 7,   "map1_s05": 8 * 7,
    "map1_s06": 8 * 7,
    "map2_s00": 14 * 16, "map2_s01": 10 * 3,  "map2_s02": 12 * 9,
    "map2_s03": 14 * 16, "map2_s04": 10 * 3,
    "map3_s00": 5 * 6,   "map3_s01": 5 * 6,   "map3_s02": 8 * 8,
    "map3_s03": 8 * 8,   "map3_s04": 8 * 8,   "map3_s05": 8 * 8,
    "map3_s06": 5 * 6,
    "map4_s00": 12 * 9,  "map4_s01": 10 * 3,  "map4_s02": 12 * 9,
    "map4_s03": 12 * 9,  "map4_s04": 8 * 8,   "map4_s05": 12 * 9,
    "map4_s06": 12 * 9,
    "map5_s00": 5 * 5,   "map5_s01": 7 * 6,   "map5_s02": 10 * 3,
    "map5_s03": 10 * 3,
    "map6_s00": 7 * 9,   "map6_s01": 10 * 3,  "map6_s02": 7 * 9,
    "map7_s00": 10 * 4,  "map7_s01": 10 * 4,  "map7_s02": 10 * 4,
    "map7_s03": 10 * 4,
}

# Per-map symbols that are NOT listed in the sym files. (name, va, size).
# The D_* names encode their PSX virtual address. Sizes are verified by
# tiling against the neighbouring symbols that ARE in the sym files
# (e.g. map1_s00: MAP_ROOM_IDXS 0x800DCC14 + 0x38 -> limits 0x800DCC4C + 8
#  -> flags 0x800DCC54 + 42*2 == map1_s00_header 0x800DCCA8).
EXTRA_SYMBOLS = {
    "map1_s00": [("D_800DCC4C", 0x800DCC4C, 8), ("D_800DCC54", 0x800DCC54, 84)],
    "map1_s01": [("D_800DC9FC", 0x800DC9FC, 8), ("D_800DCA04", 0x800DCA04, 84)],
    "map1_s04": [("D_800CCF54", 0x800CCF54, 8)],
    "map1_s05": [("D_800D5C3C", 0x800D5C3C, 8)],
    "map1_s06": [("D_800D71E8", 0x800D71E8, 8),
                 ("D_800D775C", 0x800D775C, 8)],
    # map2_s00 progression-specific spawn variants. 3 variants x 32 s_SpawnInfo
    # entries (12 B PSX layout) = 1152 B. func_800EE5D0 swaps the variant chosen
    # by func_800EE518 (event flags 172/173/174/131/181) into charaSpawnInfos.
    # Zero-stubbed -> the swap was skipped -> wrong street enemies (air screamers
    # where the progression should spawn dogs). (D_800F1CA8 idx->state map is
    # already auto-extracted.)
    "map2_s00": [("D_800F1CAC", 0x800F1CAC, 1152)],
    "map5_s00": [("D_800DA570", 0x800DA570, 8), ("D_800DA578", 0x800DA578, 44),
                 ("D_800CB0CC", 0x800CB0CC, 12)],
    "map5_s01": [("D_800EFC74", 0x800EFC74, 8),
                 ("D_800F0158", 0x800F0158, 24),
                 ("D_800F0170", 0x800F0170, 4),
                 ("D_800F0174", 0x800F0174, 24)],
    "map6_s03": [("D_800DBCDC", 0x800DBCDC, 8)],
    # Cutscene voice-command tables (see TARGETS). Sizes = gap to the next
    # known symbol (same-cluster D_* table where adjacent, else the next
    # sym-file symbol). Over-extraction copies inert ROM bytes — the scenes
    # index only as many entries as they have dialogue pages.
    "map3_s00": [("D_800D24F0", 0x800D24F0, 76)],
    "map3_s03": [("D_800D6B40", 0x800D6B40, 16),
                 ("D_800D6B50", 0x800D6B50, 4),
                 ("D_800D6B54", 0x800D6B54, 4)],
    "map3_s04": [("D_800D599C", 0x800D599C, 64),
                 ("D_800CB370", 0x800CB370, 12)],
    "map3_s05": [("D_800DAC70", 0x800DAC70, 8)],
    "map3_s06": [("D_800D26D0", 0x800D26D0, 52)],
    # map4_s03: TV-bank static/sigil effect rodata (func_800D7548/func_800D88C8).
    # Tiles exactly: 7C8+0xC=7D4, +0x10=7E4, +0x90=874, +0x24=898; 924+8=92C.
    "map4_s03": [("D_800CA788", 0x800CA788, 8),
                 ("D_800DB210", 0x800DB210, 16),  # s_800DB210 twinfeeler bounding box (field_0/4/8/C); zero box -> boundary check always false
                 ("D_800DB92C", 0x800DB92C, 4),
                 ("D_800DB7C8", 0x800DB7C8, 0xC),   # WorldGfx object ref for TV sign
                 ("D_800DB7D4", 0x800DB7D4, 0x10),  # big TV quad corners (SVECTOR pair)
                 ("D_800DB7E4", 0x800DB7E4, 0x90),  # 3x3 TV grid quad corners
                 # screen pattern tpage/clut/UV windows, s_800DB874[15] (10 B
                 # each). func_800D88C8 indexes [field_30]; field_30 reaches
                 # arg1+6 with arg1<=8 => index 14, so 15 entries = 150 B (0x96).
                 # The old 0x24 (~3 entries) left the cult-symbol patterns (idx
                 # 4-14) reading zeros = TVs showed only static.
                 ("D_800DB874", 0x800DB874, 0x96),
                 # TV screen upload imageDesc (s_FsImageDesc) reused for TV1/2/3
                 # in func_800D7450. The switch sets tPage[1]/u/v/clutY per TV
                 # but RELIES on clutX (=0x1C0=448) and tPage[0] persisting from
                 # this initializer. Zero-stubbed it loaded every TV CLUT to
                 # x=0 instead of 448, so the off screens (clut 0x5c=(448,1))
                 # sampled empty VRAM = transparent "empty rectangles" instead
                 # of TV1's CLUT[0]=0x8000 black.
                 ("D_800DB91C", 0x800DB91C, 8),
                 ("D_800DB924", 0x800DB924, 8)],    # sign position SVECTOR3 (+frame ctr)
    "map4_s04": [("D_800D3734", 0x800D3734, 68),
                 ("D_800D3778", 0x800D3778, 64)],
    # map4_s05: D_800D7D74 (pre-existing) + the Floatstinger boss rodata
    # cluster. Sizes tile exactly: 780C+0x3C=7848, 7848+0x10=7858 (runtime
    # state vars 7858..799C stay zero-stubbed), 7A20+0x10=7A30, +8=7A38,
    # +0x20=7A58, +4=7A5C, +0x18=0x800D7A74=MAP_ROOM_IDXS. The dispatch
    # table D_800D7A04 (7A04..7A20) is function pointers and lives in
    # floatstinger_rodata.inc instead.
    "map4_s05": [("D_800D7D74", 0x800D7D74, 4),
                 ("D_800D780C", 0x800D780C, 0x3C),  # flight boundary boxes (3x s_func_800D4458)
                 ("D_800D7848", 0x800D7848, 0x10),  # bone index list
                 ("D_800D7A20", 0x800D7A20, 0x10),  # SVECTOR[2] acid-spit muzzle offsets
                 ("D_800D7A30", 0x800D7A30, 8),     # SVECTOR stinger offset
                 ("D_800D7A38", 0x800D7A38, 0x20),  # s16[16] wing motion amplitudes
                 ("D_800D7A58", 0x800D7A58, 4),     # bone index list
                 ("D_800D7A5C", 0x800D7A5C, 0x18)], # SVECTOR[3]
    "map5_s02": [("D_800D947C", 0x800D947C, 32)],
    "map5_s03": [("D_800D3BDC", 0x800D3BDC, 32)],
    # D_800F0084: DVECTOR[17][17] per-cell otherworld-transition thresholds
    # (map6_s00.c func_800EC4B4). Fills the gap to g_ParticleMapIdx0
    # (0x800F0508): 0x800F0508-0x800F0084 = 0x484 = 17*17*4. Zero-stubbed it
    # made every grid cell cross its (0,0) threshold at ramp=0 -> instant full
    # otherworld instead of the real-time sweep (otherworld.log).
    "map6_s00": [("D_800F0038", 0x800F0038, 8),
                 ("D_800F0084", 0x800F0084, 0x484)],
    "map6_s01": [("D_800D4108", 0x800D4108, 32)],
    "map6_s02": [("D_800D3C2C", 0x800D3C2C, 30),  # u16[15] threshold table (D_800D3C8C < D_800D3C2C[i]); zero -> compare always false
                 ("D_800D3B40", 0x800D3B40, 4),
                 ("D_800D3B6C", 0x800D3B6C, 272),
                 ("D_800CAB90", 0x800CAB90, 8),
                 ("D_800CAB98", 0x800CAB98, 8)],
    "map6_s04": [("D_800EBA34", 0x800EBA34, 48),
                 ("D_800EBA64", 0x800EBA64, 72),    # was 210 — over-grab swallowed the carousel-horse tables
                 ("D_800EBAAC", 0x800EBAAC, 40),    # s32[10] carousel horse X offsets
                 ("D_800EBAD4", 0x800EBAD4, 40),    # s32[10] carousel horse Z offsets
                 ("D_800EBAFC", 0x800EBAFC, 20),    # q3_12[10] horse angles (zero -> all horses stacked at center)
                 ("D_800CC7C0", 0x800CC7C0, 12),
                 ("D_800CC7CC", 0x800CC7CC, 12),
                 ("D_800CB728", 0x800CB728, 12),
                 ("D_800CC4A4", 0x800CC4A4, 0x20),
                 # MonsterCybil keyframe-trigger constants (s16). Zero-stubbed =>
                 # boss waits for keyframe 0 which never arrives => freezes on her
                 # first attack (still takes hits) -> blocks Good/Bad endings.
                 ("D_800EA776", 0x800EA776, 2),
                 ("D_800EA7D4", 0x800EA7D4, 2),
                 ("D_800EA7D6", 0x800EA7D6, 2),
                 ("D_800EA816", 0x800EA816, 2),
                 ("D_800EA836", 0x800EA836, 2),
                 ("D_800EA856", 0x800EA856, 2),
                 ("D_800EA894", 0x800EA894, 2),
                 ("D_800EA896", 0x800EA896, 2)],
    "map7_s00": [("D_800D31C4", 0x800D31C4, 12),
                 ("D_800CB61C", 0x800CB61C, 8)],
    "map7_s01": [("D_800CC984", 0x800CC984, 12),
                 ("D_800CC990", 0x800CC990, 8),
                 ("D_800CC998", 0x800CC998, 12),
                 ("D_800E14E8", 0x800E14E8, 24),
                 ("D_800E1500", 0x800E1500, 4),
                 ("D_800E1504", 0x800E1504, 12),
                 ("D_800E1510", 0x800E1510, 52),
                 ("D_800E1544", 0x800E1544, 8),
                 ("D_800E154C", 0x800E154C, 16),  # u16[8] msg-id table (idx D_800E168D, <8)
                 ("D_800E155C", 0x800E155C, 2),   # u16 astrology puzzle solution (was swallowed by 154C's old 36)
                 ("D_800E1560", 0x800E1560, 14),  # s16[7] astrology panel BG texture IDs
                 ("D_800E1680", 0x800E1680, 8),
                 ("D_800E1688", 0x800E1688, 5),
                 ("D_800E168D", 0x800E168D, 1),
                 ("D_800E168E", 0x800E168E, 2),
                 ("D_800E1690", 0x800E1690, 4)],
    "map7_s02": [("D_800E9CE4", 0x800E9CE4, 24),
                 ("D_800E9CFC", 0x800E9CFC, 4),
                 ("D_800E9D00", 0x800E9D00, 12),
                 ("D_800E9D0C", 0x800E9D0C, 16),
                 ("D_800E9D1C", 0x800E9D1C, 8),
                 ("D_800E9D24", 0x800E9D24, 8),
                 ("D_800E9D2C", 0x800E9D2C, 4),
                 ("D_800E9D30", 0x800E9D30, 32),
                 ("D_800E9D50", 0x800E9D50, 28),
                 ("D_800E9D6C", 0x800E9D6C, 16),
                 ("D_800E9D7C", 0x800E9D7C, 4),
                 ("D_800E9D80", 0x800E9D80, 14),
                 ("D_800E9D8E", 0x800E9D8E, 2),
                 ("D_800E9DE8", 0x800E9DE8, 52),
                 ("D_800E9E1C", 0x800E9E1C, 5)],  # u8[5] keypad puzzle solution (vs D_800EA4AC input -> EventFlag_488)
    "map7_s03": [("D_800EC770", 0x800EC770, 40),  # s_800EC770[5] boss hit-SFX descriptors (sfxId/vol/interval); field_4=0 -> /0 crash + grunt SFX spam
                 ("D_800ED768", 0x800ED768, 20),
                 ("D_800ED77C", 0x800ED77C, 56),
                 ("D_800ED7B4", 0x800ED7B4, 216),
                 ("D_800ED88C", 0x800ED88C, 12),
                 ("D_800ED898", 0x800ED898, 284),
                 ("D_800ED9B4", 0x800ED9B4, 68)],
}

# Hard size overrides (bytes). Used where the sym-file annotation or the
# next-symbol gap would give the wrong size (e.g. sharedData_800EB740_6_s04
# is annotated size:2 but Bgm_Update reads 8 limit bytes; the map7 limit
# tables have large gaps to the next listed symbol).
# Per-map hard size overrides (bytes) for symbol names that recur across maps
# with different real sizes. Checked before the global SIZE_OVERRIDE.
SIZE_OVERRIDE_PER_MAP = {
    # Cybil basement scene: the upstream sym file sizes this table at 0x52
    # (41 entries) but the on-disc data continues with 42 more valid XA cmds
    # up to where g_Cutscene_MapMsgAudioCmds0 starts (0x800D5A1C + 0xA8 ==
    # 0x800D5AC4). The truncation made page 42+ of the scene read the
    # neighbouring tables (voices from other scenes) and then non-voice
    # data (silence) — user log decoded 1:1 against the PC data layout.
    ("map4_s01", "g_Cutscene_MapMsgAudioCmds"): 0xA8,
}

SIZE_OVERRIDE = {
    "sharedData_800ECA4C_2_s02": 4,
    "sharedData_800ECACC_2_s02": 4,
    "sharedData_800ECBD0_2_s02": 2,
    "sharedData_800EB738_6_s04": 8,
    "sharedData_800EB740_6_s04": 8,
    "sharedData_800EB748_6_s04": 8,
    "sharedData_800D2F18_7_s00": 8,
    "sharedData_800D2F20_7_s00": 8,
    "sharedData_800D2F74_7_s00": 8,
    "sharedData_800D2F7C_7_s00": 8,
    "sharedData_800D2F84_7_s00": 8,
}

# Symbols whose size is determined by scanning for a null terminator in the binary
# rather than by the sym-file size annotation or next-symbol gap inference.
NULL_TERM_SYMBOLS = {
    "LOADABLE_INVENTORY_ITEMS",
}

# (map_name, symbol_name) pairs to skip — these maps define the symbol in their
# own source files and would get a duplicate-symbol link error if we also emit
# it from the extracted_data.c.
SKIP_SYMBOL_FOR_MAP = {
    # LOADABLE_INVENTORY_ITEMS — defined in _anim_info.c for these maps
    ("map0_s01", "LOADABLE_INVENTORY_ITEMS"),
    ("map1_s04", "LOADABLE_INVENTORY_ITEMS"),
    ("map2_s01", "LOADABLE_INVENTORY_ITEMS"),
    ("map2_s03", "LOADABLE_INVENTORY_ITEMS"),
    ("map2_s04", "LOADABLE_INVENTORY_ITEMS"),
    ("map5_s03", "LOADABLE_INVENTORY_ITEMS"),
    # g_CommonWorldObjectPoses — defined in map source files for these maps
    ("map1_s00", "g_CommonWorldObjectPoses"),  # map1_s00_events_data.c (real definition)
}

# Symbols that are scalars (single value, not arrays) regardless of inferred size.
SCALAR_SYMBOLS = {
    "sharedData_800ECA4C_2_s02",
    "sharedData_800ECACC_2_s02",
    "sharedData_800ECBD0_2_s02",
    "g_Cutscene_Timer",
    "g_Cutscene_Timer0",
    "g_Cutscene_Timer1",
    "g_Cutscene_Timer2",
    "g_Cutscene_Timer3",
    "g_Cutscene_MapMsgAudioIdx",
    "g_Cutscene_MapMsgAudioIdx0",
    "g_Cutscene_MapMsgAudioIdx1",
    "g_Cutscene_MapMsgAudioIdx2",
}

# C type name to printf-style format and packing helpers
TYPE_INFO = {
    "u8":             ("0x%02X",   "B", 1),
    "u16":            ("0x%04X",   "H", 2),
    "s16":            ("%d",       "h", 2),
    "s32":            ("%d",       "i", 4),
    "VECTOR3":        (None,       None, 12),
    "s_WorldObjectPose": (None,    None, 20),
}

# ---------- parsing ----------

PA_VA_RE = re.compile(r"PA:\s*0x([0-9A-Fa-f]+)\s+VA:\s*0x([0-9A-Fa-f]+)")
SYMBOL_RE = re.compile(
    r"^([a-zA-Z_]\w*)\s*=\s*0x([0-9A-Fa-f]+)\s*;?\s*"
    r"(?://\s*(?:type:(\w+))?\s*(?:size:0x([0-9A-Fa-f]+))?)?"
)

SUBSEG_RE = re.compile(r"subsegment:\s*([.\w]+)")

def parse_sym_file(path):
    """Return (load_base, [(name, va, declared_size_or_None)], {name: subsegment})."""
    load_base = None
    syms = []
    seg_by_name = {}
    cur_seg = "?"
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            sm = SUBSEG_RE.search(line)
            if sm:
                cur_seg = sm.group(1)
            m = PA_VA_RE.search(line)
            if m and load_base is None:
                pa = int(m.group(1), 16)
                va = int(m.group(2), 16)
                load_base = va - pa
                continue
            m = SYMBOL_RE.match(line)
            if m:
                name = m.group(1)
                va   = int(m.group(2), 16)
                size = int(m.group(4), 16) if m.group(4) else None
                syms.append((name, va, size))
                seg_by_name[name] = cur_seg
    return load_base, syms, seg_by_name

def map_name_from_sym(filename):
    # sym.map0_s00.txt -> map0_s00
    return filename[len("sym."):-len(".txt")]

def bin_path_for_map(map_name):
    # map0_s00 -> VIN/MAP0_S00.BIN
    upper = map_name.upper()
    return VIN_DIR / f"{upper}.BIN"

# ---------- extraction ----------

def infer_size(name, va, declared, all_syms):
    """If size not declared, infer from gap to next symbol. Cap at 256 bytes."""
    if declared:
        return declared
    next_va = None
    for n2, va2, _ in all_syms:
        if va2 > va and (next_va is None or va2 < next_va):
            next_va = va2
    if next_va is None:
        return 16  # last symbol fallback — small
    gap = next_va - va
    return min(gap, 256)

def fmt_array(c_type, data):
    """Format raw bytes as a C array initializer body."""
    fmt, pack, _ = TYPE_INFO[c_type]
    n_elem = len(data) // TYPE_INFO[c_type][2]
    if n_elem == 0:
        return None
    vals = struct.unpack(f"<{n_elem}{pack}", data[:n_elem * TYPE_INFO[c_type][2]])
    parts = [fmt % v for v in vals]
    # Word-wrap
    lines = []
    line = "    "
    for i, s in enumerate(parts):
        sep = ", " if i < len(parts) - 1 else ""
        if len(line) + len(s) + len(sep) > 100:
            lines.append(line.rstrip())
            line = "    "
        line += s + sep
    if line.strip():
        lines.append(line.rstrip())
    return "\n".join(lines)

def emit_vector3(data):
    if len(data) < 12:
        data = data + b"\x00" * (12 - len(data))
    vx, vy, vz = struct.unpack("<iii", data[:12])
    return f"{{ {vx}, {vy}, {vz} }}"

def emit_world_object_pose_array(data, n):
    """Format n s_WorldObjectPose elements as a C initializer body.
    Layout: VECTOR3 position (3×s32, 12 bytes) + SVECTOR3 rotation (3×s16, 6 bytes) + 2 pad = 20 bytes.
    """
    lines = []
    for i in range(n):
        ofs = i * 20
        if ofs + 18 > len(data):
            break
        px, py, pz = struct.unpack_from("<3i", data, ofs)
        rx, ry, rz = struct.unpack_from("<3h", data, ofs + 12)
        comma = "," if i < n - 1 else ""
        lines.append(f"    {{ {{ {px}, {py}, {pz} }}, {{ {rx}, {ry}, {rz} }} }}{comma}")
    return "\n".join(lines)

def emit_map7_d800ed274(binary, load_base, va):
    """map7_s03 D_800ED274[18] — s_func_800E1FE0 camera/path table (final boss).
    PSX element (68 B): { s32 field_0; s_800F4B40_1C field_4[2] } where
    s_800F4B40_1C (32 B) = { VECTOR3* vec_0; q19_12 f4; VECTOR3* vec_8;
    q19_12 fC; s16 x8 }. The embedded pointers target single VECTOR3s in the
    overlay; the engine both reads AND writes through them (func_800E1F44
    copies *src->vec into *dest->vec), so aliasing between elements must be
    preserved — emit one named static per unique PSX target address.
    Targets outside the file-backed extent are PSX .bss → zero-init static.
    Targets below the overlay base live in the MAIN EXE and must reference the
    real game global (the engine tracks live values through them)."""
    # 0x800BA3C8 = g_SysWork (0x800B9FC0) + 0x408 = npcs[0x1A0] + 2*296 + 0x18
    exe_ptrs = {0x800BA3C8: "&g_SysWork.npcs[2].position"}
    targets = {}   # psx_va -> static name
    elems = []
    for i in range(18):
        e_ofs = (va - load_base) + i * 68
        field_0 = struct.unpack_from("<i", binary, e_ofs)[0]
        lcs = []
        for j in range(2):
            lc = e_ofs + 4 + j * 32
            p0, f4, p8, fC = struct.unpack_from("<IiIi", binary, lc)
            s16s = struct.unpack_from("<8h", binary, lc + 16)
            for p in (p0, p8):
                if p and p not in exe_ptrs and p not in targets:
                    if p < 0x800C0000:
                        raise SystemExit(
                            f"D_800ED274: pointer 0x{p:08X} targets the main exe "
                            f"but has no mapping in exe_ptrs — resolve it first")
                    targets[p] = f"s_Vec_{p:08X}"
            lcs.append((p0, f4, p8, fC, s16s))
        elems.append((field_0, lcs))

    text = ("// 0x%08X  size 0x%X (1224 bytes) — s_func_800E1FE0 D_800ED274[18]\n"
            "// Local typedef mirrors include/maps/map7/map7_s03.h (not included here\n"
            "// to avoid its prerequisite chain); layout must stay in sync.\n"
            % (va, 18 * 68))
    text += ("typedef struct {\n"
             "    VECTOR3* vec_0;\n"
             "    s32      field_4;\n"
             "    VECTOR3* vec_8;\n"
             "    s32      field_C;\n"
             "    s16      pos_10, field_12, field_14, field_16;\n"
             "    s16      total_max_spd_18, field_1A, field_1C, field_1E;\n"
             "} s_800F4B40_1C_pc;\n"
             "typedef struct {\n"
             "    s32             field_0;\n"
             "    s_800F4B40_1C_pc field_4[2];\n"
             "} s_func_800E1FE0_pc;\n\n")

    for p in sorted(targets):
        name = targets[p]
        t_ofs = p - load_base
        if 0 <= t_ofs and t_ofs + 12 <= len(binary):
            x, y, z = struct.unpack_from("<3i", binary, t_ofs)
            text += f"static VECTOR3 {name} = {{ {x}, {y}, {z} }};  // 0x{p:08X}\n"
        else:
            text += f"static VECTOR3 {name} = {{ 0, 0, 0 }};  // 0x{p:08X} (.bss)\n"
    text += "\ns_func_800E1FE0_pc D_800ED274[18] = {\n"

    def ref(p):
        if not p:
            return "NULL"
        return exe_ptrs.get(p) or f"&{targets[p]}"

    for field_0, lcs in elems:
        parts = []
        for p0, f4, p8, fC, s16s in lcs:
            s16str = ", ".join(str(v) for v in s16s)
            parts.append(f"{{ {ref(p0)}, {f4}, {ref(p8)}, {fC}, {s16str} }}")
        text += f"    {{ {field_0}, {{ {parts[0]},\n           {parts[1]} }} }},\n"
    text += "};\n\n"
    return text

# Element widths for stub declarations in data_stubs.c. Some stubs were
# hand-ENLARGED beyond their PSX data extent because game code WRITES through
# them as pools (g_Particles: s_Particle[450]=14400; sharedData_800DFB7C_0_s00:
# s_MapHdr_field_4C[450]=9000 — see the comments in data_stubs.c). A DLL-local
# extracted array SMALLER than the stub shadows it and re-introduces the very
# overrun the enlarged stub fixed (crashed map3_s03 on hospital entry).
STUB_TYPE_WIDTHS = {
    "u8": 1, "s8": 1, "char": 1,
    "u16": 2, "s16": 2, "short": 2,
    "u32": 4, "s32": 4, "int": 4,
    "sh_pc_MapHdrField4C": 20,
}

# Mirror typedefs for the world-object emitter, injected once per generated
# file that needs them. Layout MUST match what the PC compiler produces for
# the real headers (s_ModelInfo: s32 + 2 ptrs + s32 = 32 B with x64 padding;
# metadata 12 B; model 48 B; placement 64 B).
WOBJ_TYPEDEFS = """\
/* PC-layout mirrors of s_WorldObjectModel/s_WorldObjectPlacement (the real
 * headers aren't included here to avoid their prerequisite chain; layouts
 * must stay in sync). coord/modelHdr are RUNTIME pointers the engine fills
 * at map load by matching `name` against loaded model files — the PSX ROM
 * values are meaningless on PC and emitted as NULL. */
typedef struct { s32 field_0; void* coord; void* modelHdr; s32 modelIdx; } sh_pc_ModelInfo;
/* u[2] first so a positional initializer fills the exact filename bytes. */
typedef struct { union { u32 u[2]; char str[8]; } name; s8 field_8; s8 lmIdx; } sh_pc_WObjMeta;
typedef struct { sh_pc_ModelInfo modelInfo; sh_pc_WObjMeta metadata; } sh_pc_WObjModel;
typedef struct { sh_pc_WObjModel object; VECTOR3 position; } sh_pc_WObjPlacement;
typedef struct { sh_pc_WObjModel object; VECTOR3 position; SVECTOR3 rotation; } sh_pc_WObjPose;

"""

WOBJ_KINDS = {
    # decl type           -> (kind tag, PSX bytes/elem, mirror C type)
    "s_WorldObjectModel":     ("model",     28, "sh_pc_WObjModel"),
    "s_WorldObjectPlacement": ("placement", 40, "sh_pc_WObjPlacement"),
    "s_WorldObjectPose":      ("pose",      48, "sh_pc_WObjPose"),
}

def emit_world_object(name, va, data, kind, decl_count):
    """Emit a world-object symbol as PC-layout initializers. All three kinds
    embed s_WorldObjectModel (which holds two RUNTIME pointers -> NULL);
    placement adds VECTOR3 position, pose adds position + SVECTOR3 rotation.
    Authored fields (flags, modelIdx, 8-char name, lmIdx, transforms) come
    from the ROM bytes."""
    tag, elem_psx, c_type = next(v for v in WOBJ_KINDS.values() if v[0] == kind)
    n = decl_count if decl_count else max(1, len(data) // elem_psx)

    text = (f"// 0x{va:08X}  {kind} x{n} (PSX {elem_psx} B/elem; PC layout, "
            f"runtime ptrs NULLed)\n")
    text += f"{c_type} {name}[{n}] = {{\n"
    for i in range(n):
        ofs = i * elem_psx
        if ofs + elem_psx > len(data):
            text += "    { 0 }, /* beyond ROM extent — zero */\n"
            continue
        f0, _coord, _hdr, mIdx = struct.unpack_from("<iIIi", data, ofs)
        nm0, nm1 = struct.unpack_from("<II", data, ofs + 16)
        f8, lmIdx = struct.unpack_from("<bb", data, ofs + 24)
        raw_name = data[ofs + 16:ofs + 24]
        readable = "".join(chr(b) if 32 <= b < 127 else "." for b in raw_name)
        model = (f"{{ {{ {f0}, NULL, NULL, {mIdx} }}, "
                 f"{{ {{ {{ 0x{nm0:08X}, 0x{nm1:08X} }} }}, {f8}, {lmIdx} }} }}")
        if kind == "placement":
            px, py, pz = struct.unpack_from("<3i", data, ofs + 28)
            text += f"    {{ {model}, {{ {px}, {py}, {pz} }} }}, /* \"{readable}\" */\n"
        elif kind == "pose":
            px, py, pz = struct.unpack_from("<3i", data, ofs + 28)
            rx, ry, rz = struct.unpack_from("<3h", data, ofs + 40)
            text += (f"    {{ {model}, {{ {px}, {py}, {pz} }}, "
                     f"{{ {rx}, {ry}, {rz} }} }}, /* \"{readable}\" */\n")
        else:
            text += f"    {{ {model} }}, /* \"{readable}\" */\n"
    text += "};\n\n"
    return text

def load_stub_sizes():
    """Map of still-zero-stubbed symbol name -> stub byte size (None if the
    stub's element type has unknown width — such symbols are not safe to
    auto-extract because we can't guarantee write capacity)."""
    stub_re = re.compile(r"^(\w+) (\w+)\[(\d+)\] = \{0\};", re.M)
    path = REPO_ROOT / "pc_port" / "src" / "stubs" / "data_stubs.c"
    sizes = {}
    for typ, name, count in stub_re.findall(path.read_text(encoding="utf-8", errors="ignore")):
        width = STUB_TYPE_WIDTHS.get(typ)
        sizes[name] = int(count) * width if width else None
    return sizes

STUB_SIZES = None

# Types whose PC (x86-64) layout is byte-identical to PSX, verified by hand.
# Raw byte extraction is ONLY valid for these. Structs containing pointers
# (s_WorldObjectModel/s_WorldObjectPlacement via s_ModelInfo, s_RayTrace,
# s_800E0930, s_800F4B40, GsCOORDINATE2, ...) are LARGER on PC: game writes
# at PC field offsets overrun a PSX-sized array and smash adjacent globals.
# That exact failure crashed map3_s03 (hospital) via the g_WorldObject_*
# placements — worse than the old roomy 256-byte exe stubs whose overruns
# landed in neighbouring stub padding.
SAFE_AUTO_TYPES = {
    "u8", "u16", "u32", "s8", "s16", "s32", "char", "short", "int", "bool",
    "q3_12", "q19_12", "q23_8", "q7_8", "q11_4", "q20_12", "q4_12", "q12",
    "VECTOR3", "SVECTOR3", "SVECTOR", "VECTOR", "DVECTOR", "CVECTOR",
    # NOTE: s_WorldObjectPose is NOT here despite the name — the real typedef
    # (maps/shared.h) embeds s_WorldObjectModel with two pointers (48 B PSX /
    # 72 B PC). It goes through the world-object emitter instead. s_Pose is
    # the plain {VECTOR3, SVECTOR3} transform and is safe.
    "s_BgmLayerLimits", "s_FsImageDesc", "s_Pose",
    "s_Keyframe", "MATRIX", "s_800E330C", "s_800E34FC",
    "s_Particle", "s_ParticleVectors", "s_func_800CB560", "s_CollisionResult",
    "s_MapHdr_field_4C", "s_MapHeader_field_5C",
    "s_MapOverlayHdr_7C", "s_MapOverlayHdr_94",
    "s_sharedData_800D5AB0_1_s05", "s_sharedData_800DFB10_0_s01",
    "s_sharedData_800E21D0_0_s01", "s_sharedData_800ED2D4_2_s02",
}

# Byte width of the safe fixed-layout types, used to size header-driven
# auto-extraction EXACTLY (count * width) instead of gap inference — gap
# inference over-grabs and swallows neighbouring symbols (e.g. D_800EBA64's
# 210 B extent hid the carousel-horse tables; D_800E154C's 36 B hid the
# astrology solution). Only types that are byte-identical PSX<->PC.
TYPE_SIZEOF = {
    "u8": 1, "s8": 1, "char": 1, "bool": 1,
    "u16": 2, "s16": 2, "short": 2, "q3_12": 2, "q4_12": 2, "q11_4": 2, "q7_8": 2, "q12": 2,
    "u32": 4, "s32": 4, "int": 4, "q19_12": 4, "q20_12": 4, "q23_8": 4,
    "DVECTOR": 4, "SVECTOR3": 6, "SVECTOR": 8, "CVECTOR": 4, "VECTOR3": 12, "VECTOR": 16,
}

DECL_RE = re.compile(r"extern\s+(?:const\s+)?(\w+\**)\s+(\w+)\s*((?:\[[^\]]*\])*)\s*;")

def _parse_decls(txt, types):
    for m in DECL_RE.finditer(txt):
        # Multi-dim arrays flatten to a total element count (row-major flat
        # layout is identical). Bounds may be macro names or empty — count
        # only when every dimension is numeric.
        dims = re.findall(r"\[\s*([^\]]*?)\s*\]", m.group(3) or "")
        count = None
        if dims and all(d.isdigit() for d in dims):
            count = 1
            for d in dims:
                count *= int(d)
        types.setdefault(m.group(2), (m.group(1), count))

def load_extern_types():
    """Map symbol name -> (declared extern type, array count or None), scanned
    from all headers and sources. Used to gate auto-extraction on
    SAFE_AUTO_TYPES. NOTE: global fallback only — the same g_WorldObject*
    name can have DIFFERENT types in different maps (g_WorldObject1 is
    s_WorldObjectPlacement[6] in map1_s00 but s_WorldObjectPose in
    map1_s01); always consult load_map_extern_types() first."""
    types = {}
    for sub in ("include", "src"):
        base = REPO_ROOT / sub
        for path in list(base.rglob("*.h")) + list(base.rglob("*.c")):
            try:
                _parse_decls(path.read_text(encoding="utf-8", errors="ignore"), types)
            except OSError:
                continue
    return types

MAP_EXTERN_TYPES = {}

def load_map_extern_types(map_name):
    """Per-map authoritative extern declarations: the map's own header plus
    its source directory. Resolves symbols whose type varies per map."""
    if map_name in MAP_EXTERN_TYPES:
        return MAP_EXTERN_TYPES[map_name]
    types = {}
    family = map_name.split("_")[0]  # map1_s01 -> map1
    candidates = [REPO_ROOT / "include" / "maps" / family / f"{map_name}.h"]
    src_dir = REPO_ROOT / "src" / "maps" / map_name
    if src_dir.is_dir():
        candidates += sorted(src_dir.glob("*.c")) + sorted(src_dir.glob("*.h"))
    for path in candidates:
        try:
            _parse_decls(path.read_text(encoding="utf-8", errors="ignore"), types)
        except OSError:
            continue
    MAP_EXTERN_TYPES[map_name] = types
    return types

EXTERN_TYPES = None

def extract_map(map_name, sym_path, bin_path):
    global STUB_SIZES, EXTERN_TYPES
    if STUB_SIZES is None:
        STUB_SIZES = load_stub_sizes()
    if EXTERN_TYPES is None:
        EXTERN_TYPES = load_extern_types()

    load_base, syms, seg_by_name = parse_sym_file(sym_path)
    if load_base is None:
        print(f"  [{map_name}] no PA/VA pair; skipping", file=sys.stderr)
        return None
    if not bin_path.exists():
        print(f"  [{map_name}] no binary at {bin_path}; skipping", file=sys.stderr)
        return None

    with open(bin_path, "rb") as f:
        binary = f.read()

    syms = syms + EXTRA_SYMBOLS.get(map_name, [])

    # Header-driven candidates: zero-stub D_ symbols absent from the splat sym
    # map (so they never appear in `syms` above) but declared in THIS map's
    # decomp header with a fixed-width type + numeric array size. Their VA is
    # encoded in the name (D_800EBAAC -> 0x800EBAAC). Scoping to the map's own
    # header keeps a symbol from being mis-extracted into another overlay that
    # happens to span the same address. The header_only gate downstream (safe
    # type + non-zero ROM + pointer scan + in-binary bounds) does the rest.
    # This auto-catches the carousel-horse / puzzle-solution / keyframe class
    # instead of hand-listing each.
    seg_names = set(seg_by_name)
    existing = seg_names | {t[0] for t in syms}
    for nm, (dty, dcnt) in load_map_extern_types(map_name).items():
        if (not nm.startswith("D_") or nm in existing or nm not in STUB_SIZES
                or not dcnt or (dty or "").rstrip("*") not in TYPE_SIZEOF):
            continue
        try:
            syms = syms + [(nm, int(nm[2:], 16), None)]
        except ValueError:
            pass

    found = []
    for name, va, decl_size in syms:
        # Special-case: pointer-bearing struct table, custom emitter.
        if name == "D_800ED274" and map_name == "map7_s03":
            text = emit_map7_d800ed274(binary, load_base, va)
            found.append((name, va, "CUSTOM", 18 * 68, text, None))
            continue
        # Auto-extract any still-zero-stubbed symbol that lives in a
        # file-backed section (.data/.rodata) of this overlay. Stubs whose
        # symbols sit in .bss/.sbss are runtime state — zero is correct.
        seg = seg_by_name.get(name)
        auto = (name not in TARGETS and name in STUB_SIZES and
                seg is not None and "bss" not in seg)
        # Header-driven auto: symbols absent from the splat sym map (so seg is
        # None) are never reached above, which is exactly how the carousel
        # horses / astrology solution / boss keyframes slipped through. Accept
        # them too — the in-binary bounds check below is the .data/.bss test
        # (file-backed => real ROM), and they're gated on a safe header type +
        # explicit array size + non-zero content + the PSX-pointer scan.
        # Explicit EXTRA_SYMBOLS entry (hand-given size) the auto/header-only
        # classifier can't size — e.g. a struct-typed scalar like
        # `s_FsImageDesc D_800DB91C` (no array count, type absent from
        # TYPE_SIZEOF). Extract raw bytes at exactly the listed size.
        explicit_raw = (decl_size is not None and name not in TARGETS
                        and seg is None)
        header_only = (not auto and not explicit_raw and name not in TARGETS
                       and name in STUB_SIZES and seg is None)
        if header_only:
            auto = True
        if name not in TARGETS and not auto and not explicit_raw:
            continue
        wobj_kind = None
        decl_count = None
        if auto:
            # Per-map header first: the same g_WorldObject* name can be a
            # different type in different maps.
            decl = load_map_extern_types(map_name).get(name) or EXTERN_TYPES.get(name)
            decl_type, decl_count = decl if decl else (None, None)
            if decl_type in WOBJ_KINDS:
                wobj_kind = WOBJ_KINDS[decl_type][0]
            elif decl_type not in SAFE_AUTO_TYPES:
                print(f"  [{map_name}] {name}: skipped — extern type "
                      f"'{decl_type or 'unknown'}' not verified 64-bit-safe; "
                      f"keeping exe zero stub", file=sys.stderr)
                continue
            if wobj_kind is None and STUB_SIZES[name] is None:
                print(f"  [{map_name}] {name}: skipped — stub element width "
                      f"unknown, cannot guarantee write capacity", file=sys.stderr)
                continue
            # Header-only symbols have no gap-inference fallback (they're not in
            # the sym map, so there's no "next symbol" to bound them). Require an
            # explicit array size + known fixed width so we extract EXACTLY the
            # declared extent — no over-grab. Scalars/[] without a size are left
            # stubbed (handled later or manually).
            if header_only and (decl_count is None or
                                (decl_type or "").rstrip("*") not in TYPE_SIZEOF):
                continue
        if (map_name, name) in SKIP_SYMBOL_FOR_MAP:
            print(f"  [{map_name}] {name}: skipped (local definition exists in source)")
            continue
        # Auto-discovered symbols default to raw u8 bytes — the exe stub is
        # u8[], and any consumer casts/indexes through its own extern type.
        c_type = "u8" if (auto or explicit_raw) else TARGETS[name][0]
        ofs = va - load_base
        if ofs < 0 or ofs >= len(binary):
            print(f"  [{map_name}] {name}: offset 0x{ofs:X} out of binary bounds",
                  file=sys.stderr)
            continue
        if name in NULL_TERM_SYMBOLS:
            # Find the null terminator and use that as the actual size.
            null_pos = binary.find(b'\x00', ofs)
            if null_pos == -1 or null_pos - ofs > 128:
                size = 128  # safety cap
            else:
                size = null_pos - ofs + 1  # include null terminator
        elif name == "MAP_ROOM_IDXS":
            size = MAP_ROOM_IDXS_SIZE.get(map_name)
            if size is None:
                print(f"  [{map_name}] MAP_ROOM_IDXS: no grid size known; skipping",
                      file=sys.stderr)
                continue
        elif (map_name, name) in SIZE_OVERRIDE_PER_MAP:
            size = SIZE_OVERRIDE_PER_MAP[(map_name, name)]
        elif name in SIZE_OVERRIDE:
            size = SIZE_OVERRIDE[name]
        elif explicit_raw:
            # Hand-given size from EXTRA_SYMBOLS — extract exactly that.
            size = decl_size
        elif header_only:
            # Exact header-declared extent (validated above to be a fixed type
            # with a numeric array size). No gap inference -> no over-grab.
            size = decl_count * TYPE_SIZEOF[(decl_type or "").rstrip("*")]
        else:
            size = infer_size(name, va, decl_size, syms)
            if wobj_kind and decl_count:
                # The declared array bound is authoritative; gap inference is
                # capped at 256 B and would clip larger arrays.
                elem_psx = next(v[1] for v in WOBJ_KINDS.values() if v[0] == wobj_kind)
                size = max(size, decl_count * elem_psx)
        if ofs + size > len(binary):
            print(f"  [{map_name}] {name}: offset 0x{ofs:X}+{size} out of binary bounds",
                  file=sys.stderr)
            continue
        data = binary[ofs:ofs + size]

        # Header-only symbols that are all-zero in ROM are runtime work vars
        # (the game inits them before reading) — the zero stub is already
        # correct, so don't bother emitting them. Only real ROM data (non-zero)
        # indicates a read-before-write table that the stub breaks.
        if header_only and not any(data):
            continue

        if wobj_kind:
            # Verified: ALL world-object symbols are zero in ROM (the engine
            # fills them at runtime), so the exe zero stub is PSX-equivalent.
            # Emit only if authored content appears — recognized by a fully
            # printable 8-char filename in some element (a lone nonzero blob
            # is tail-overlap junk from a neighbouring symbol, e.g. map2_s04
            # g_CommonWorldObjects' last 2 elements).
            elem_psx = next(v[1] for v in WOBJ_KINDS.values() if v[0] == wobj_kind)
            authored = False
            for eo in range(0, len(data) - elem_psx + 1, elem_psx):
                nm = data[eo + 16:eo + 24]
                if nm[0] != 0 and all(32 <= b < 127 for b in nm.rstrip(b"\x00")):
                    authored = True
                    break
            if not authored:
                continue
            text = emit_world_object(name, va, data, wobj_kind, decl_count)
            found.append((name, va, "CUSTOM_WOBJ", size, text, None))
            continue

        # PSX-pointer scan: a table of aligned 0x80xxxxxx dwords is a pointer
        # table (function or data). Raw extraction of those is WORSE than the
        # zero stub — PSX VAs are garbage on PC and crash when dereferenced.
        # Such tables need manual seed+Init porting (see *_anim_infos.c).
        n_dwords = size // 4
        n_ptrs = 0
        if n_dwords:
            for dw in struct.unpack_from(f"<{n_dwords}I", data):
                if 0x80000000 <= dw < 0x80200000:
                    n_ptrs += 1
        warn = None
        if n_ptrs:
            if auto and n_ptrs * 8 >= n_dwords:  # >= 1/8 of dwords are pointers
                print(f"  [{map_name}] {name}: SKIPPED — {n_ptrs}/{n_dwords} dwords are "
                      f"PSX pointers; needs manual seed+Init port", file=sys.stderr)
                continue
            warn = (f"WARNING: {n_ptrs}/{n_dwords} dwords look like PSX pointers "
                    f"(0x80xxxxxx) — verify these are data values, not addresses")
            print(f"  [{map_name}] {name}: {warn}", file=sys.stderr)
        # Never declare a DLL-local array smaller than its exe stub: the game
        # may WRITE up to the stub's (sometimes hand-enlarged) capacity. The
        # initializer keeps just the real ROM bytes; C zero-fills the tail.
        # EXCEPTION: header_only symbols have an authoritative exact size from
        # their declaration — bumping them to the default 256 B stub would
        # over-grab into the next symbol (the bug this whole change fixes), and
        # the consumer's own typed extern bounds its accesses anyway.
        if auto and not header_only and STUB_SIZES[name] > size:
            warn = (warn + " | " if warn else "") + \
                   f"declared {STUB_SIZES[name]} B (stub capacity), {size} B from ROM"
            size = STUB_SIZES[name]
        found.append((name, va, c_type, size, data, warn))

    if not found:
        return None
    return found

# ---------- C generation ----------

C_HEADER = """\
/* AUTO-GENERATED by pc_port/tools/extract_map_data.py — do not edit.
 * Source: disc_extract/VIN/{BINFILE}
 * Map: {MAPNAME}
 *
 * Per-map data extracted from the original PSX map overlay binary.
 * In the upstream decomp these are `extern` declared in map headers but their
 * data lives only in the PSX overlay binary, not in C source.
 * This file provides them as local definitions so the map DLL is self-contained.
 */

#include <assert.h>  /* C11 static_assert macro */
#include "common.h"
#include "game.h"  /* VECTOR3, SVECTOR3 */

/* Minimal typedef for s_WorldObjectPose — full definition is in maps/shared.h.
 * Guarded so it doesn't conflict if a map's header is pulled in transitively. */
#ifndef S_WORLD_OBJECT_POSE_FWDDECL
#define S_WORLD_OBJECT_POSE_FWDDECL
typedef struct {{ VECTOR3 position; SVECTOR3 rotation_C; }} s_WorldObjectPose;
#endif

"""

def generate_c(map_name, found, bin_filename):
    family = map_name.split("_")[0]  # map0_s00 -> map0
    text = C_HEADER.format(MAPNAME=map_name, BINFILE=bin_filename, family=family, map=map_name)

    if any(entry[2] == "CUSTOM_WOBJ" for entry in found):
        text += WOBJ_TYPEDEFS

    for name, va, c_type, size, data, warn in sorted(found, key=lambda x: x[1]):
        if c_type in ("CUSTOM", "CUSTOM_WOBJ"):
            text += data  # pre-rendered C text with its own header comment
            continue
        text += f"// 0x{va:08X}  size 0x{size:X} ({size} bytes)\n"
        if warn:
            text += f"// {warn}\n"

        if c_type == "VECTOR3":
            text += f"VECTOR3 {name} = {emit_vector3(data)};\n\n"
            continue

        if c_type == "s_WorldObjectPose":
            n = size // 20
            body = emit_world_object_pose_array(data, n)
            text += f"s_WorldObjectPose {name}[{n}] = {{\n{body}\n}};\n\n"
            continue

        if name in SCALAR_SYMBOLS:
            # Single value
            fmt, pack, w = TYPE_INFO[c_type]
            if size < w:
                data = data + b"\x00" * (w - size)
            val = struct.unpack(f"<{pack}", data[:w])[0]
            text += f"{c_type} {name} = {fmt % val};\n\n"
            continue

        # Array
        body = fmt_array(c_type, data)
        if body is None:
            text += f"// (empty extraction for {name}, skipped)\n\n"
            continue
        n = size // TYPE_INFO[c_type][2]
        text += f"{c_type} {name}[{n}] = {{\n{body}\n}};\n\n"

    return text

# ---------- main ----------

def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    sym_files = sorted(SYM_DIR.glob("sym.map*.txt"))
    print(f"Found {len(sym_files)} sym files in {SYM_DIR}")

    total_extracted = 0
    for sf in sym_files:
        map_name = map_name_from_sym(sf.name)
        bin_path = bin_path_for_map(map_name)
        bin_fname = bin_path.name

        found = extract_map(map_name, sf, bin_path)
        if not found:
            continue

        out_text = generate_c(map_name, found, bin_fname)
        out_path = OUT_DIR / f"{map_name}_extracted_data.c"

        # Safety: if the existing file is marked "MANUALLY MAINTAINED" in its
        # header, skip the rewrite. Some extracted files have been hand-edited
        # to add extra symbols not in TARGETS (e.g. map0_s00 has D_800DFAC4
        # for the alley camera warp flag) and a blind regen would drop them.
        if out_path.exists():
            try:
                existing = out_path.read_text(encoding="utf-8", errors="ignore")
                if "MANUALLY MAINTAINED" in existing[:500]:
                    print(f"  [{map_name}] manually maintained — skipping regen")
                    continue
            except OSError:
                pass

        out_path.write_text(out_text, encoding="utf-8")
        total_extracted += len(found)
        print(f"  [{map_name}] {len(found):2d} syms -> {out_path.name}")

    print(f"\nDone: extracted {total_extracted} symbols across all maps.")
    print(f"Output: {OUT_DIR}")

if __name__ == "__main__":
    main()
