"""Scan ENTIRE BIN for sectors that look like STR chunk headers.
We test multiple possible offsets where the 3800h magic could appear, since
this disc has no submode-tagged video sectors (FMVs may be stored as data)."""
import sys, struct

path = sys.argv[1]
N = 2352
# Plausible offsets for the 0x3800 marker within a sector
# (relative to start of 2352-byte sector):
#   16 (subheader) + 8 (subheader copy) + 0x10 = 0x20 — Mode 2 video standard
#   16 (skip sync) + 0x10 = 0x20 (no subheader, Mode 1 data) — also 0x20
#   24 + 0x10 = 0x28 — chunk header inside subheader-as-data
candidate_offsets = [0x20, 0x28, 0x10, 0x18, 0x40, 0x30]

with open(path, "rb") as f:
    f.seek(0, 2); total = f.tell() // N; f.seek(0)
    print(f"# scanning {total} sectors for 3800h", file=sys.stderr)
    hits = []
    for s in range(total):
        d = f.read(N)
        if len(d) < N: break
        for ofs in candidate_offsets:
            if d[ofs] == 0x00 and d[ofs+1] == 0x38:
                # Also expect chunk_num=0 at ofs-0x10 if this is a frame start.
                chunk_num = struct.unpack_from("<H", d, ofs - 0x10)[0]
                frame_num = struct.unpack_from("<I", d, ofs - 0x0c)[0]
                w = struct.unpack_from("<H", d, ofs - 0x04)[0]
                h = struct.unpack_from("<H", d, ofs - 0x02)[0]
                if 0 <= chunk_num < 20 and frame_num < 10000 and 0 < w <= 320 and 0 < h <= 240:
                    hits.append((s, ofs, chunk_num, frame_num, w, h))
                    if len(hits) < 50:
                        print(f"MATCH sector={s:8d} ofs=0x{ofs:02x} "
                              f"chunk={chunk_num} frame={frame_num} {w}x{h}")
    print(f"# {len(hits)} matches", file=sys.stderr)
