import struct, re, os

ROOT = r"C:\Claude\silenthill\silent-hill-decomp"
BIN  = r"C:\Claude\silenthill\disc_extract\VIN\MAP0_S00.BIN"
SYM  = os.path.join(ROOT, "configs", "USA", "maps", "sym.map0_s00.txt")
OUT  = os.path.join(ROOT, "src", "maps", "characters", "stalker_rodata.inc")
base = 0x800C9578

data = open(BIN, "rb").read()
syms = {}
for line in open(SYM):
    m = re.match(r'\s*(sharedData_800D[DE][0-9A-F]+_0_s00)\s*=\s*(0x[0-9A-Fa-f]+)', line)
    if m:
        syms[m.group(1)] = int(m.group(2), 16)

# (name, count); count 1 = single (declared without [])
tables = [
    ("sharedData_800DDBA8_0_s00", 1), ("sharedData_800DDBBC_0_s00", 9), ("sharedData_800DDC70_0_s00", 7),
    ("sharedData_800DDCFC_0_s00", 7), ("sharedData_800DDD88_0_s00", 16), ("sharedData_800DDEC8_0_s00", 5),
    ("sharedData_800DDF2C_0_s00", 11), ("sharedData_800DE008_0_s00", 11), ("sharedData_800DE0E4_0_s00", 13),
    ("sharedData_800DE1E8_0_s00", 11), ("sharedData_800DE2C4_0_s00", 1), ("sharedData_800DE2D8_0_s00", 1),
    ("sharedData_800DE2EC_0_s00", 1), ("sharedData_800DE300_0_s00", 16), ("sharedData_800DE440_0_s00", 16),
    ("sharedData_800DE580_0_s00", 42), ("sharedData_800DE8C8_0_s00", 29), ("sharedData_800DEB0C_0_s00", 18),
    ("sharedData_800DEC74_0_s00", 3), ("sharedData_800DECB0_0_s00", 12), ("sharedData_800DEDA0_0_s00", 5),
    ("sharedData_800DEE04_0_s00", 3), ("sharedData_800DEE40_0_s00", 2), ("sharedData_800DEE68_0_s00", 9),
    ("sharedData_800DEF1C_0_s00", 30),
]

out = []
out.append("/* stalker_rodata.inc -- Chara_Stalker / Chara_GreyChild / Chara_Mumbler collision")
out.append(" * keyframe data, extracted from disc_extract/VIN/MAP0_S00.BIN at the PSX VRAM")
out.append(" * addresses in configs/USA/maps/sym.map0_s00.txt (overlay base 0x800C9578).")
out.append(" *")
out.append(" * Feeds func_80070400, which fills field_C8 (vertical collision extent),")
out.append(" * field_D4.radius_0 (push radius) and field_D8 (offsets) per anim frame. The")
out.append(" * prior zero-stubs in pc_port data_stubs.c collapsed the melee vertical hit")
out.append(" * window so short enemies (grey children) could never be struck, and gave the")
out.append(" * family no push radius. s_Keyframe is 20 bytes on PSX and PC so rows drop in")
out.append(" * directly. Map-independent (the enemy's intrinsic body profile); included at")
out.append(" * the bottom of stalker.c so every map DLL hosting a stalker-family enemy gets")
out.append(" * its own copy. */")
out.append("")

missing = []
for name, n in tables:
    if name not in syms:
        missing.append(name); continue
    off = syms[name] - base
    if n == 1:
        v = struct.unpack_from("<10h", data, off)
        out.append("s_Keyframe %s = { %s };" % (name, ", ".join(str(x) for x in v)))
    else:
        out.append("s_Keyframe %s[%d] = {" % (name, n))
        for i in range(n):
            v = struct.unpack_from("<10h", data, off + i * 20)
            out.append("    { %s }," % ", ".join(str(x) for x in v))
        out.append("};")
    out.append("")

open(OUT, "w").write("\n".join(out))
print("WROTE", OUT)
print("tables:", len(tables), "missing:", missing)
print("inc lines:", len(out))
