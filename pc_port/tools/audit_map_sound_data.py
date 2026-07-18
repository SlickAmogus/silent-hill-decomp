#!/usr/bin/env python3
"""Census of map-overlay SOUND data on the PC port vs the PSX disc.

For every map, finds D_8* symbols referenced near sound code (Bgm_*, SD_Call,
Sd_*, Sfx, mapRoomIdx-indexed BGM tables, distance-modulated ambience), then
classifies each symbol's PC-side definition:
    extracted   defined in pc_port/build_gen/extracted_data/<map>_extracted_data.c
    map-source  defined with an initializer inside src/maps/<map>/
    zero-stub   defined in pc_port/src/stubs/data_stubs.c
    missing     no definition found (INCLUDE_RODATA remnant / implicit zero)
and dumps the real disc bytes behind the VA from disc_extract/VIN/<MAP>.BIN.

Verdict: zero-stub/missing + nonzero disc bytes = the disc data is being
dropped on PC (the recurring "silent BGM layer / missing ambience" bug class).

Usage:
    python audit_map_sound_data.py [map ...]      # default: all maps
    python audit_map_sound_data.py -v map5_s00    # also hex-dump disc bytes
"""
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SRC_MAPS = REPO / "src" / "maps"
EXTRACTED = REPO / "pc_port" / "build_gen" / "extracted_data"
DATA_STUBS = REPO / "pc_port" / "src" / "stubs" / "data_stubs.c"
SYM_DIR = REPO / "configs" / "USA" / "maps"
VIN_DIR = REPO.parent / "disc_extract" / "VIN"

SOUND_KEYWORDS = re.compile(
    r"Bgm_|SD_Call|Sd_[A-Z]|Sfx|bgmIdx|BgmFlag|mapRoomIdx|"
    r"Math_Distance2dGet|AmbientSfx|layerLimit|LayerLimit")
DSYM = re.compile(r"\b(D_800[C-F][0-9A-Fa-f]{4})\b")
CONTEXT_LINES = 12

PA_VA_RE = re.compile(r"PA:\s*0x([0-9A-Fa-f]+)\s+VA:\s*0x([0-9A-Fa-f]+)")


SYM_LINE = re.compile(r"^\w+\s*=\s*0x([0-9A-Fa-f]{8})\s*;")


def sym_load_base(map_name):
    path = SYM_DIR / f"sym.{map_name}.txt"
    if not path.exists():
        return None, []
    load_base = None
    vas = []
    for line in path.read_text(errors="ignore").splitlines():
        m = PA_VA_RE.search(line)
        if m and load_base is None:
            load_base = int(m.group(2), 16) - int(m.group(1), 16)
        m = SYM_LINE.match(line)
        if m:
            vas.append(int(m.group(1), 16))
    return load_base, sorted(set(vas))


def probe_size(va, all_vas, cap=64):
    """Clamp to the gap before the next known symbol so the probe doesn't
    read the neighbour's bytes and misattribute them."""
    for v in all_vas:
        if v > va:
            return max(1, min(cap, v - va))
    return cap


def disc_bytes(map_name, va, size, load_base):
    bin_path = VIN_DIR / f"{map_name.upper()}.BIN"
    if not bin_path.exists() or load_base is None:
        return None
    off = va - load_base
    data = bin_path.read_bytes()
    if off < 0 or off >= len(data):
        return None
    return data[off:off + size]


WRITE_USE = re.compile(r"^\s*(?:\w[\w.\[\]>-]*\s*=\s*)?(D_800[C-F][0-9A-Fa-f]{4})(\.[\w]+|\[[^\]]*\])?\s*(=[^=]|\+=|-=|\|=|&=|\+\+|--)")


def find_sound_symbols(map_name):
    """D_8* symbols on or near sound-keyword lines in the map's sources.
    Returns sym -> (evidence, usage) where usage is read/write/read+write."""
    hits = {}
    map_dir = SRC_MAPS / map_name
    if not map_dir.is_dir():
        return hits
    for c in sorted(map_dir.glob("*.c")):
        lines = c.read_text(errors="ignore").splitlines()
        keyword_idx = [i for i, l in enumerate(lines) if SOUND_KEYWORDS.search(l)]
        near = set()
        for i in keyword_idx:
            near.update(range(max(0, i - CONTEXT_LINES), min(len(lines), i + CONTEXT_LINES + 1)))
        for i in sorted(near):
            wm = WRITE_USE.match(lines[i])
            for m in DSYM.finditer(lines[i]):
                sym = m.group(1)
                is_write = bool(wm and wm.group(1) == sym)
                ev, usage = hits.get(sym, (None, set()))
                usage.add("write" if is_write else "read")
                if ev is None or (not is_write and "read-ev" not in usage):
                    ev = f"{c.name}:{i+1}: {lines[i].strip()[:100]}"
                    if not is_write:
                        usage.add("read-ev")
                hits[sym] = (ev, usage)
    return {s: (ev, "+".join(sorted(u - {"read-ev"}))) for s, (ev, u) in hits.items()}


def def_status(map_name, sym):
    ext = EXTRACTED / f"{map_name}_extracted_data.c"
    if ext.exists() and re.search(rf"\b{sym}\b\s*(\[|=)", ext.read_text(errors="ignore")):
        return "extracted"
    map_dir = SRC_MAPS / map_name
    for c in map_dir.glob("*.[ch]"):
        for line in c.read_text(errors="ignore").splitlines():
            if re.search(rf"^\s*(static\s+)?(const\s+)?\w[\w\s\*]*\b{sym}\b\s*(\[[^\]]*\])?\s*=", line):
                return "map-source"
    if DATA_STUBS.exists() and re.search(rf"\b{sym}\b\s*(\[|=)", DATA_STUBS.read_text(errors="ignore")):
        return "zero-stub"
    return "missing"


def audit_map(map_name, verbose=False):
    rows = []
    load_base, all_vas = sym_load_base(map_name)
    for sym, (evidence, usage) in sorted(find_sound_symbols(map_name).items()):
        va = int(sym[2:], 16)
        status = def_status(map_name, sym)
        size = probe_size(va, all_vas)
        raw = disc_bytes(map_name, va, size, load_base)
        nz = sum(1 for b in raw if b) if raw is not None else None
        if status in ("zero-stub", "missing") and nz and "read" in usage:
            verdict = "BUG?"
        elif status in ("zero-stub", "missing") and nz:
            verdict = "workvar?"  # write-only in sound context; likely runtime var
        elif nz is None:
            verdict = "no-disc-data"
        else:
            verdict = "ok"
        rows.append((sym, status, nz, size, usage, verdict, evidence, raw))
    return rows


def main():
    verbose = "-v" in sys.argv
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    maps = args or sorted(p.name for p in SRC_MAPS.iterdir()
                          if p.is_dir() and p.name.startswith("map"))
    total_bugs = 0
    for mp in maps:
        rows = audit_map(mp, verbose)
        flagged = [r for r in rows if r[5] == "BUG?"]
        total_bugs += len(flagged)
        interesting = flagged if not verbose else rows
        if not rows:
            continue
        print(f"\n== {mp}: {len(rows)} sound-adjacent symbols, {len(flagged)} flagged ==")
        for sym, status, nz, size, usage, verdict, evidence, raw in (interesting or rows):
            mark = " <-- DISC DATA DROPPED" if verdict == "BUG?" else ""
            print(f"  {sym}  {status:10s} {usage:10s} discNZ={nz!s:>4}/{size}  {verdict}{mark}")
            print(f"      {evidence}")
            if (verbose or verdict == "BUG?") and raw is not None and nz:
                print("      disc: " + raw[:32].hex(" "))
    print(f"\nTOTAL flagged (zero-stub/missing with real disc data): {total_bugs}")


if __name__ == "__main__":
    main()
