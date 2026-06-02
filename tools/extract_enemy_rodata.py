import struct, re, os, sys

ROOT = r"C:\Claude\silenthill\silent-hill-decomp"
DISC = r"C:\Claude\silenthill\disc_extract\VIN"
STUBS = os.path.join(ROOT, "pc_port", "src", "stubs", "data_stubs.c")
base = 0x800C9578  # overlay vram base (same for all maps)

# config per enemy: (csrc, bin, symfile, sym_regex_prefix, out_inc, title)
CFG = {
 "larval_stalker": ("src/maps/characters/larval_stalker.c", "MAP1_S00.BIN",
    "configs/USA/maps/sym.map1_s00.txt", r"sharedData_800D[AB][0-9A-F]+_1_s00",
    "src/maps/characters/larval_stalker_rodata.inc", "Chara_LarvalStalker"),
}

def run(enemy):
    csrc, binname, symf, symre, outinc, title = CFG[enemy]
    src = open(os.path.join(ROOT, csrc), encoding="utf-8", errors="replace").read()
    data = open(os.path.join(DISC, binname), "rb").read()
    # which symbols are zero-stubbed?
    stubsrc = open(STUBS, encoding="utf-8").read()
    stubbed = set(re.findall(r'u8 (' + symre + r')\[256\] = \{0\};', stubsrc))
    # addr+size from sym
    addrs, sizes = {}, {}
    for line in open(os.path.join(ROOT, symf)):
        m = re.match(r'\s*(' + symre + r')\s*=\s*(0x[0-9A-Fa-f]+);(?:\s*//\s*size:(0x[0-9A-Fa-f]+))?', line)
        if m:
            addrs[m.group(1)] = int(m.group(2), 16)
            if m.group(3): sizes[m.group(1)] = int(m.group(3), 16)
    # array vs single from C externs
    isarray = {}
    for m in re.finditer(r'extern\s+s_Keyframe\s+(' + symre + r')\s*(\[\s*\])?\s*;', src):
        isarray[m.group(1)] = bool(m.group(2))
    out = ["/* %s collision-keyframe data, extracted from %s at the PSX VRAM" % (title, binname),
           " * addresses in %s (overlay base 0x800C9578)." % os.path.basename(symf),
           " * Replaces the zero-stubs in pc_port data_stubs.c that gave func_80070400" ,
           " * all-zero field_C8/radius (no melee hit window, no push collision)." ,
           " * s_Keyframe is 20 bytes on PSX and PC. Included at the bottom of %s." % os.path.basename(csrc),
           " */", ""]
    done = []
    skipped = []
    for name in sorted(stubbed):
        if name not in addrs:
            skipped.append((name, "no sym addr")); continue
        if name not in sizes:
            skipped.append((name, "no sym size")); continue
        n = sizes[name] // 20
        off = addrs[name] - base
        if off < 0 or off + n*20 > len(data):
            skipped.append((name, "offset out of BIN range")); continue
        arr = isarray.get(name, n > 1)
        rows = []
        for i in range(n):
            v = struct.unpack_from("<10h", data, off + i*20)
            rows.append("    { %s }," % ", ".join(str(x) for x in v))
        if arr:
            out.append("s_Keyframe %s[%d] = {" % (name, n)); out += rows; out.append("};")
        else:
            v = struct.unpack_from("<10h", data, off)
            out.append("s_Keyframe %s = { %s };" % (name, ", ".join(str(x) for x in v)))
        out.append("")
        done.append(name)
    open(os.path.join(ROOT, outinc), "w").write("\n".join(out))
    # comment out the stubs we now define
    lines = stubsrc.split("\n")
    nl = []
    removed = 0
    for ln in lines:
        m = re.match(r'\s*u8 (' + symre + r')\[256\] = \{0\};\s*$', ln)
        if m and m.group(1) in done:
            nl.append("/* %s now provided with real data by %s */" % (m.group(1), os.path.basename(outinc)))
            removed += 1
        else:
            nl.append(ln)
    open(STUBS, "w", encoding="utf-8").write("\n".join(nl))
    print("enemy:", enemy)
    print("  defined:", len(done), "stubs commented:", removed)
    print("  skipped:", skipped)
    print("  wrote:", outinc)

run(sys.argv[1] if len(sys.argv) > 1 else "larval_stalker")
