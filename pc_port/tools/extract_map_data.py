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
    "map1_s06": [("D_800D71E8", 0x800D71E8, 8)],
    "map5_s00": [("D_800DA570", 0x800DA570, 8), ("D_800DA578", 0x800DA578, 44)],
    "map5_s01": [("D_800EFC74", 0x800EFC74, 8)],
    "map6_s03": [("D_800DBCDC", 0x800DBCDC, 8)],
}

# Hard size overrides (bytes). Used where the sym-file annotation or the
# next-symbol gap would give the wrong size (e.g. sharedData_800EB740_6_s04
# is annotated size:2 but Bgm_Update reads 8 limit bytes; the map7 limit
# tables have large gaps to the next listed symbol).
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

def load_stub_names():
    """Symbol names still zero-stubbed in data_stubs.c — candidates for
    auto-extraction when they appear in a map's .data/.rodata."""
    stub_re = re.compile(r"^\w+ (\w+)\[\d*\] = \{0\};", re.M)
    path = REPO_ROOT / "pc_port" / "src" / "stubs" / "data_stubs.c"
    return set(stub_re.findall(path.read_text(encoding="utf-8", errors="ignore")))

STUB_NAMES = None

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
    "s_BgmLayerLimits", "s_FsImageDesc", "s_Pose", "s_WorldObjectPose",
    "s_Particle", "s_ParticleVectors", "s_func_800CB560", "s_CollisionResult",
    "s_MapHdr_field_4C", "s_MapHeader_field_5C",
    "s_MapOverlayHdr_7C", "s_MapOverlayHdr_94",
    "s_sharedData_800D5AB0_1_s05", "s_sharedData_800DFB10_0_s01",
    "s_sharedData_800E21D0_0_s01", "s_sharedData_800ED2D4_2_s02",
}

def load_extern_types():
    """Map symbol name -> declared extern type, scanned from all headers and
    sources. Used to gate auto-extraction on SAFE_AUTO_TYPES."""
    types = {}
    decl_re = re.compile(r"extern\s+(?:const\s+)?(\w+\**)\s+(\w+)\s*(?:\[[^\]]*\])?\s*;")
    for sub in ("include", "src"):
        base = REPO_ROOT / sub
        for path in list(base.rglob("*.h")) + list(base.rglob("*.c")):
            try:
                txt = path.read_text(encoding="utf-8", errors="ignore")
            except OSError:
                continue
            for m in decl_re.finditer(txt):
                types.setdefault(m.group(2), m.group(1))
    return types

EXTERN_TYPES = None

def extract_map(map_name, sym_path, bin_path):
    global STUB_NAMES, EXTERN_TYPES
    if STUB_NAMES is None:
        STUB_NAMES = load_stub_names()
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
        auto = (name not in TARGETS and name in STUB_NAMES and
                "bss" not in seg_by_name.get(name, "bss"))
        if name not in TARGETS and not auto:
            continue
        if auto:
            decl_type = EXTERN_TYPES.get(name)
            if decl_type not in SAFE_AUTO_TYPES:
                print(f"  [{map_name}] {name}: skipped — extern type "
                      f"'{decl_type or 'unknown'}' not verified 64-bit-safe; "
                      f"keeping exe zero stub", file=sys.stderr)
                continue
        if (map_name, name) in SKIP_SYMBOL_FOR_MAP:
            print(f"  [{map_name}] {name}: skipped (local definition exists in source)")
            continue
        # Auto-discovered symbols default to raw u8 bytes — the exe stub is
        # u8[], and any consumer casts/indexes through its own extern type.
        c_type = "u8" if auto else TARGETS[name][0]
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
        elif name in SIZE_OVERRIDE:
            size = SIZE_OVERRIDE[name]
        else:
            size = infer_size(name, va, decl_size, syms)
        if ofs + size > len(binary):
            print(f"  [{map_name}] {name}: offset 0x{ofs:X}+{size} out of binary bounds",
                  file=sys.stderr)
            continue
        data = binary[ofs:ofs + size]

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

    for name, va, c_type, size, data, warn in sorted(found, key=lambda x: x[1]):
        if c_type == "CUSTOM":
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
