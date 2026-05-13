"""Dump subheader + CDSECTOR header of first 15 sectors of C1 FMV."""
import sys, struct
path = sys.argv[1]
base = 0x20807
with open(path, "rb") as f:
    for off in range(15):
        s = base + off
        f.seek(s * 2352 + 16)  # skip sync
        d = f.read(40)  # subheader(8) + cdsector(32)
        sub = d[0:8]
        h   = d[8:40]
        submode = sub[2]
        secCount, nSectors = struct.unpack_from("<HH", h, 4)
        frame_no, frame_sz = struct.unpack_from("<II", h, 8)
        w, hh = struct.unpack_from("<HH", h, 16)
        print(f"sec {off:2d} (abs {s}): submode=0x{submode:02x}  "
              f"secCount={secCount} of {nSectors}  frame={frame_no}  "
              f"size={frame_sz}  {w}x{hh}")
