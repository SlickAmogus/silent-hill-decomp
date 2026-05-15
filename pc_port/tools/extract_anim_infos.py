"""Extract enemy *_ANIM_INFOS tables from disc BIN files.

Usage: python3 extract_anim_infos.py <SYMBOL> <map_id>
  e.g.  python3 extract_anim_infos.py BLOODSUCKER_ANIM_INFOS map3_s03

Walks PSX s_AnimInfo[N] (16 B per entry) and emits a GroanerAnimInfos_Init-
style seed array + Init function. PSX function pointers are mapped to:
  0x800449F0 -> 1 (Anim_PlaybackOnce)
  0x80044B38 -> 2 (Anim_PlaybackLoop)
  0x80044CA4 -> 0 (Anim_BlendLinear)
  anything else (incl. 0) -> 3 (NULL)

The output is dropped to stdout — paste into a per-enemy file in
pc_port/src/.
"""
import struct
import sys
import re
import os

PLAYBACK_FUNCS = {
    0x800449F0: 1,  # Anim_PlaybackOnce
    0x80044B38: 2,  # Anim_PlaybackLoop
    0x80044CA4: 0,  # Anim_BlendLinear
}

DECOMP = "C:/Claude/silenthill/silent-hill-decomp"

def vram_base(map_id):
    path = f"{DECOMP}/configs/USA/maps/{map_id}.yaml"
    with open(path) as f:
        for line in f:
            m = re.search(r"vram:\s*(0x[0-9A-Fa-f]+)", line)
            if m:
                return int(m.group(1), 16)
    raise RuntimeError(f"No vram in {path}")

def find_symbol(map_id, symbol):
    path = f"{DECOMP}/configs/USA/maps/sym.{map_id}.txt"
    syms = []
    with open(path) as f:
        for line in f:
            m = re.match(r"^(\w+)\s*=\s*(0x[0-9A-Fa-f]+);", line)
            if m:
                syms.append((m.group(1), int(m.group(2), 16)))
    syms.sort(key=lambda x: x[1])
    for i, (n, a) in enumerate(syms):
        if n == symbol:
            next_a = syms[i+1][1] if i+1 < len(syms) else None
            return a, next_a
    return None, None

def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    symbol, map_id = sys.argv[1], sys.argv[2]

    psx_addr, next_addr = find_symbol(map_id, symbol)
    if psx_addr is None:
        print(f"ERROR: {symbol} not in sym.{map_id}.txt", file=sys.stderr)
        sys.exit(1)

    base = vram_base(map_id)
    bin_path = f"C:/Claude/silenthill/disc_extract/VIN/{map_id.upper()}.BIN"
    with open(bin_path, "rb") as f:
        blob = f.read()

    ofs = psx_addr - base
    if next_addr is not None:
        n_entries = (next_addr - psx_addr) // 16
        size_hint = f" (next sym at 0x{next_addr:08X})"
    else:
        # No next symbol — assume up to 64 entries.
        n_entries = 64
        size_hint = " (last symbol in map; capped at 64)"

    print(f"// {symbol} from {map_id.upper()}.BIN at PSX 0x{psx_addr:08X}, "
          f"BIN offset 0x{ofs:X}, {n_entries} entries{size_hint}", file=sys.stderr)

    seeds = []
    for i in range(n_entries):
        if ofs + i*16 + 16 > len(blob):
            print(f"WARN: out of range at entry {i}", file=sys.stderr)
            break
        e = blob[ofs + i*16 : ofs + i*16 + 16]
        playback_psx = struct.unpack("<I", e[0:4])[0]
        status      = e[4]
        hasVar      = e[5]
        linkStatus  = e[6]
        # e[7] = pad
        duration    = struct.unpack("<i", e[8:12])[0]
        startKf     = struct.unpack("<h", e[12:14])[0]
        endKf       = struct.unpack("<h", e[14:16])[0]

        kind = PLAYBACK_FUNCS.get(playback_psx, 3)
        seeds.append((kind, status, hasVar, linkStatus, duration, startKf, endKf, playback_psx))

    # Emit seed table
    name = symbol  # e.g. BLOODSUCKER_ANIM_INFOS
    print(f"s_AnimInfo {name}[{n_entries}];")
    print()
    print("typedef struct {")
    print("    u8  kind;       /* 0=BlendLinear, 1=PlaybackOnce, 2=PlaybackLoop, 3=NULL */")
    print("    u8  status;")
    print("    s8  hasVariableDuration;")
    print("    u8  linkStatus;")
    print("    s32 durationConstantQ12;")
    print("    s16 startKeyframeIdx;")
    print("    s16 endKeyframeIdx;")
    print(f"}} {name}_Seed;")
    print()
    print(f"static const {name}_Seed s_seeds[{n_entries}] = {{")
    for k, st, hv, ls, du, skf, ekf, psx in seeds:
        # Comment showing the PSX playback addr if unmapped
        cmt = ""
        if k == 3 and psx != 0:
            cmt = f"  /* PSX playback 0x{psx:08X} not mapped */"
        print(f"    {{ {k}, 0x{st:02X}, {hv}, 0x{ls:02X}, {du}, {skf}, {ekf} }},{cmt}")
    print("};")
    print()
    print(f"void {name.title().replace('_','')}_Init(void)")
    print("{")
    print("    void (*fns[3])(struct _Model*, struct _AnmHeader*, GsCOORDINATE2*, struct _AnimInfo*) = {")
    print("        Anim_BlendLinear, Anim_PlaybackOnce, Anim_PlaybackLoop };")
    print(f"    for (int i = 0; i < {n_entries}; i++) {{")
    print(f"        const {name}_Seed* s = &s_seeds[i];")
    print(f"        {name}[i].playbackFunc        = (s->kind < 3) ? fns[s->kind] : NULL;")
    print(f"        {name}[i].status              = s->status;")
    print(f"        {name}[i].hasVariableDuration = s->hasVariableDuration;")
    print(f"        {name}[i].linkStatus          = s->linkStatus;")
    print(f"        {name}[i].duration.constant   = s->durationConstantQ12;")
    print(f"        {name}[i].startKeyframeIdx    = s->startKeyframeIdx;")
    print(f"        {name}[i].endKeyframeIdx      = s->endKeyframeIdx;")
    print("    }")
    print("}")

if __name__ == "__main__":
    main()
