import re, os
F = r"C:\Claude\silenthill\silent-hill-decomp\pc_port\src\stubs\data_stubs.c"
names = ["800DDBA8","800DDBBC","800DDC70","800DDCFC","800DDD88","800DDEC8","800DDF2C",
"800DE008","800DE0E4","800DE1E8","800DE2C4","800DE2D8","800DE2EC","800DE300","800DE440",
"800DE580","800DE8C8","800DEB0C","800DEC74","800DECB0","800DEDA0","800DEE04","800DEE40",
"800DEE68","800DEF1C"]
lines = open(F, encoding="utf-8").read().split("\n")
nameset = set(names)
out = []
removed = 0
for ln in lines:
    m = re.match(r'\s*u8 sharedData_(800D[DE][0-9A-F]+)_0_s00\[256\] = \{0\};\s*$', ln)
    if m and m.group(1) in nameset:
        out.append("/* sharedData_%s_0_s00 now provided with real data by stalker_rodata.inc */" % m.group(1))
        removed += 1
    else:
        out.append(ln)
open(F, "w", encoding="utf-8").write("\n".join(out))
print("removed stubs:", removed)
