import struct, re, os, sys

ROOT  = r"C:\Claude\silenthill\silent-hill-decomp"
DISC  = r"C:\Claude\silenthill\disc_extract\VIN"
STUBS = os.path.join(ROOT, "pc_port", "src", "stubs", "data_stubs.c")
BASE  = 0x800C9578  # overlay vram base (shared by all map overlays)

# enemy -> (csrc, bin, symfile, out_inc, title)
CFG = {
 "larval_stalker": ("src/maps/characters/larval_stalker.c", "MAP1_S00.BIN",
    "configs/USA/maps/sym.map1_s00.txt", "src/maps/characters/larval_stalker_rodata.inc", "Chara_LarvalStalker"),
 "creeper": ("src/maps/characters/creeper.c", "MAP1_S02.BIN",
    "configs/USA/maps/sym.map1_s02.txt", "src/maps/characters/creeper_rodata.inc", "Chara_Creeper"),
 "hanged_scratcher": ("src/maps/characters/hanged_scratcher.c", "MAP5_S00.BIN",
    "configs/USA/maps/sym.map5_s00.txt", "src/maps/characters/hanged_scratcher_rodata.inc", "Chara_HangedScratcher"),
}

def run(enemy):
    csrc, binname, symf, outinc, title = CFG[enemy]
    src     = open(os.path.join(ROOT, csrc), encoding="utf-8", errors="replace").read()
    data    = open(os.path.join(DISC, binname), "rb").read()
    stubsrc = open(STUBS, encoding="utf-8").read()

    # Authoritative table list = the enemy's own `extern s_Keyframe` declarations.
    externs = {}  # name -> is_array
    for m in re.finditer(r'extern\s+s_Keyframe\s+(sharedData_[0-9A-Fa-f]+_\d+_s\d+)\s*(\[\s*\])?\s*;', src):
        externs[m.group(1)] = bool(m.group(2))

    stubbed = set(re.findall(r'u8 (sharedData_[0-9A-Fa-f]+_\d+_s\d+)\[256\] = \{0\};', stubsrc))

    addrs, sizes = {}, {}
    all_addrs = []   # every symbol address in the file, for gap-based sizing
    for line in open(os.path.join(ROOT, symf)):
        m = re.match(r'\s*(\w+)\s*=\s*(0x[0-9A-Fa-f]+);(?:\s*//\s*size:(0x[0-9A-Fa-f]+))?', line)
        if not m: continue
        a = int(m.group(2), 16)
        all_addrs.append(a)
        if m.group(1).startswith("sharedData_"):
            addrs[m.group(1)] = a
            if m.group(3): sizes[m.group(1)] = int(m.group(3), 16)
    # Fill missing sizes from the gap to the next symbol address (tables are
    # a contiguous data block; verified this equals the // size: comments on
    # the maps that have them).
    srt = sorted(set(all_addrs))
    for name, a in addrs.items():
        if name in sizes: continue
        nxt = next((x for x in srt if x > a), None)
        if nxt: sizes[name] = nxt - a

    out = ["/* %s collision-keyframe data, extracted from %s at the PSX VRAM" % (title, binname),
           " * addresses in %s (overlay base 0x800C9578). Replaces the zero-stubs" % os.path.basename(symf),
           " * in pc_port data_stubs.c that gave func_80070400 all-zero field_C8/radius",
           " * (no melee hit window, no push collision). s_Keyframe is 20 bytes PSX/PC.",
           " * Included at the bottom of %s. */" % os.path.basename(csrc), ""]
    done, skipped = [], []
    for name in externs:                       # only this enemy's tables
        if name not in stubbed:  skipped.append((name, "not zero-stubbed")); continue
        if name not in addrs:    skipped.append((name, "no sym addr")); continue
        if name not in sizes:    skipped.append((name, "no sym size")); continue
        n   = sizes[name] // 20
        off = addrs[name] - BASE
        if off < 0 or off + n*20 > len(data):
            skipped.append((name, "offset 0x%x out of BIN(0x%x)" % (off, len(data)))); continue
        rows = ["    { %s }," % ", ".join(str(x) for x in struct.unpack_from("<10h", data, off+i*20)) for i in range(n)]
        if externs[name] or n > 1:
            out.append("s_Keyframe %s[%d] = {" % (name, n)); out += rows; out.append("};")
        else:
            v = struct.unpack_from("<10h", data, off)
            out.append("s_Keyframe %s = { %s };" % (name, ", ".join(str(x) for x in v)))
        out.append("")
        done.append(name)

    open(os.path.join(ROOT, outinc), "w").write("\n".join(out))

    lines, removed = stubsrc.split("\n"), 0
    nl = []
    for ln in lines:
        m = re.match(r'\s*u8 (sharedData_[0-9A-Fa-f]+_\d+_s\d+)\[256\] = \{0\};\s*$', ln)
        if m and m.group(1) in done:
            nl.append("/* %s now provided with real data by %s */" % (m.group(1), os.path.basename(outinc))); removed += 1
        else:
            nl.append(ln)
    open(STUBS, "w", encoding="utf-8").write("\n".join(nl))
    print("%s: externs=%d defined=%d stubsCommented=%d" % (enemy, len(externs), len(done), removed))
    if skipped: print("   skipped:", skipped)
    # sanity sample
    if done:
        s = done[0]; off = addrs[s]-BASE
        print("   sample %s[0] = %s" % (s, struct.unpack_from("<10h", data, off)))

for e in sys.argv[1:]:
    run(e)
