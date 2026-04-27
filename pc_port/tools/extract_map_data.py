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
}

# Symbols that are scalars (single value, not arrays) regardless of inferred size.
SCALAR_SYMBOLS = {
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
    "u8":      ("0x%02X",   "B", 1),
    "u16":     ("0x%04X",   "H", 2),
    "s32":     ("%d",       "i", 4),
    "VECTOR3": (None,       None, 12),
}

# ---------- parsing ----------

PA_VA_RE = re.compile(r"PA:\s*0x([0-9A-Fa-f]+)\s+VA:\s*0x([0-9A-Fa-f]+)")
SYMBOL_RE = re.compile(
    r"^([a-zA-Z_]\w*)\s*=\s*0x([0-9A-Fa-f]+)\s*;?\s*"
    r"(?://\s*(?:type:(\w+))?\s*(?:size:0x([0-9A-Fa-f]+))?)?"
)

def parse_sym_file(path):
    """Return (load_base, [(name, va, declared_size_or_None)])."""
    load_base = None
    syms = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
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
    return load_base, syms

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

def extract_map(map_name, sym_path, bin_path):
    load_base, syms = parse_sym_file(sym_path)
    if load_base is None:
        print(f"  [{map_name}] no PA/VA pair; skipping", file=sys.stderr)
        return None
    if not bin_path.exists():
        print(f"  [{map_name}] no binary at {bin_path}; skipping", file=sys.stderr)
        return None

    with open(bin_path, "rb") as f:
        binary = f.read()

    found = []
    for name, va, decl_size in syms:
        if name not in TARGETS:
            continue
        c_type, _ = TARGETS[name]
        size = infer_size(name, va, decl_size, syms)
        ofs = va - load_base
        if ofs < 0 or ofs + size > len(binary):
            print(f"  [{map_name}] {name}: offset 0x{ofs:X}+{size} out of binary bounds",
                  file=sys.stderr)
            continue
        data = binary[ofs:ofs + size]
        found.append((name, va, c_type, size, data))

    if not found:
        return None
    return found

# ---------- C generation ----------

C_HEADER = """\
/* AUTO-GENERATED by pc_port/tools/extract_map_data.py — do not edit.
 * Source: disc_extract/VIN/{BINFILE}
 * Map: {MAPNAME}
 *
 * Per-map cutscene data extracted from the original PSX map overlay binary.
 * In the upstream decomp these are `extern` declared in include/maps/{family}/{map}.h
 * but their data lives only in the PSX overlay binary, not in C source.
 * This file provides them as local definitions so the map DLL is self-contained.
 */

#include <assert.h>  /* C11 static_assert macro */
#include "common.h"
#include "game.h"  /* VECTOR3 */

"""

def generate_c(map_name, found, bin_filename):
    family = map_name.split("_")[0]  # map0_s00 -> map0
    text = C_HEADER.format(MAPNAME=map_name, BINFILE=bin_filename, family=family, map=map_name)

    for name, va, c_type, size, data in sorted(found, key=lambda x: x[1]):
        text += f"// 0x{va:08X}  size 0x{size:X} ({size} bytes)\n"

        if c_type == "VECTOR3":
            text += f"VECTOR3 {name} = {emit_vector3(data)};\n\n"
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
        out_path.write_text(out_text, encoding="utf-8")
        total_extracted += len(found)
        print(f"  [{map_name}] {len(found):2d} syms -> {out_path.name}")

    print(f"\nDone: extracted {total_extracted} symbols across all maps.")
    print(f"Output: {OUT_DIR}")

if __name__ == "__main__":
    main()
