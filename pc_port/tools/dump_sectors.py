import sys
path = sys.argv[1]
with open(path, "rb") as f:
    for s in [16, 17, 18, 100, 1000, 10000, 100000]:
        f.seek(s * 2352)
        d = f.read(64)
        print(f"sector {s}:")
        print(f"  bytes[0:16]:  {d[:16].hex(' ')}")
        print(f"  bytes[16:32]: {d[16:32].hex(' ')}")
        print(f"  bytes[32:48]: {d[32:48].hex(' ')}")
        print(f"  mode byte (offset 15): 0x{d[15]:02x}")
        print(f"  subheader [16:24]: {d[16:24].hex(' ')}  submode=0x{d[18]:02x}")
