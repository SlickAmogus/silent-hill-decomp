#!/usr/bin/env python3
"""Dump the original PSX bytes behind a symbol from a map overlay BIN (ground
truth for "is this zero-stub actually needed?").

Usage:
    python dump_disc_sym.py <map> <0xVA|symbolName> [size] [type]

  <map>   e.g. map3_s02   (picks configs/USA/maps/sym.<map>.txt + VIN/<MAP>.BIN)
  <0xVA|symbolName>  PSX virtual address (0x800.....) OR a symbol name present
                     in that map's sym file.
  size    bytes to dump (default 32). If a symbolName is given and size omitted,
          uses the sym-file size annotation when present, else 32.
  type    one of u8/u16/s16/s32 to also print decoded values (default u8).

Prints: file offset, raw hex, decoded values, and NONZERO byte count (0 == the
data really is zero on disc, so a zero-stub is CORRECT; >0 == real data the
stub is silently dropping).
"""
import re, sys, struct
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SYM_DIR = REPO / "configs" / "USA" / "maps"
VIN_DIR = REPO.parent / "disc_extract" / "VIN"

PA_VA_RE = re.compile(r"PA:\s*0x([0-9A-Fa-f]+)\s+VA:\s*0x([0-9A-Fa-f]+)")
SYMBOL_RE = re.compile(
    r"^([a-zA-Z_]\w*)\s*=\s*0x([0-9A-Fa-f]+)\s*;?\s*"
    r"(?://\s*(?:type:(\w+))?\s*(?:size:0x([0-9A-Fa-f]+))?)?")

def parse_sym(map_name):
    path = SYM_DIR / f"sym.{map_name}.txt"
    load_base = None
    syms = {}
    if path.exists():
        for line in path.read_text(errors="ignore").splitlines():
            m = PA_VA_RE.search(line)
            if m and load_base is None:
                load_base = int(m.group(2), 16) - int(m.group(1), 16)
            m = SYMBOL_RE.match(line)
            if m:
                syms[m.group(1)] = (int(m.group(2), 16),
                                    int(m.group(4), 16) if m.group(4) else None)
    return load_base, syms

def main():
    if len(sys.argv) < 3:
        print(__doc__); sys.exit(1)
    map_name = sys.argv[1]
    target = sys.argv[2]
    load_base, syms = parse_sym(map_name)
    size = None
    if target.lower().startswith("0x"):
        va = int(target, 16)
    elif target in syms:
        va, size = syms[target]
    else:
        print(f"symbol {target} not in sym.{map_name}.txt and not a 0x VA"); sys.exit(2)
    if len(sys.argv) >= 4:
        size = int(sys.argv[3], 0)
    if size is None:
        size = 32
    ctype = sys.argv[4] if len(sys.argv) >= 5 else "u8"

    if load_base is None:
        # No PA/VA line: overlays load at 0x800C0000 in this game; fall back so a
        # bare VA still resolves. Report it so the caller can sanity-check.
        load_base = 0x800C0000 - 0
        print(f"[warn] no PA/VA in sym.{map_name}.txt; assuming load_base=0x{load_base:08X}")

    binp = VIN_DIR / f"{map_name.upper()}.BIN"
    if not binp.exists():
        print(f"missing BIN: {binp}"); sys.exit(3)
    data = binp.read_bytes()
    off = va - load_base
    print(f"map={map_name} va=0x{va:08X} load_base=0x{load_base:08X} "
          f"file_off=0x{off:X} size={size} bin_len=0x{len(data):X}")
    if off < 0 or off >= len(data):
        print(f"  -> offset outside file extent => PSX .bss (zero-init is CORRECT)")
        return
    chunk = data[off:off+size]
    nz = sum(1 for b in chunk if b)
    print("  hex:", chunk.hex(" "))
    fmt = {"u8":("B",1,"0x%02X"),"u16":("H",2,"0x%04X"),
           "s16":("h",2,"%d"),"s32":("i",4,"%d")}.get(ctype,("B",1,"0x%02X"))
    n = len(chunk)//fmt[1]
    if n:
        vals = struct.unpack(f"<{n}{fmt[0]}", chunk[:n*fmt[1]])
        print(f"  {ctype}:", ", ".join(fmt[2] % v for v in vals))
    print(f"  NONZERO bytes: {nz}/{len(chunk)}  ==> "
          + ("REAL DATA (stub would drop it)" if nz else "all zero (stub is correct)"))

if __name__ == "__main__":
    main()
