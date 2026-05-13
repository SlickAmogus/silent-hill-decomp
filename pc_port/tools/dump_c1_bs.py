"""Reassemble C1 frame 1 bitstream and print first 32 halfwords."""
import struct
path = "C:/Claude/silenthill/Silent Hill (USA).bin"
base = 0x20807
bs = bytearray()
n_video = 0
with open(path, "rb") as f:
    for off in range(10):
        s = base + off
        f.seek(s * 2352 + 16)
        d = f.read(2336)
        sub = d[:8]
        submode = sub[2]
        if submode & 0x04:
            continue
        # Video sector: CDSECTOR header at [8:40], bitstream at [40:2332]
        sec_count = struct.unpack_from("<H", d, 8 + 4)[0]
        frame_no  = struct.unpack_from("<I", d, 8 + 8)[0]
        chunk = d[40:40 + 2292]
        bs += chunk
        n_video += 1
        print(f"  sector +{off}: secCount={sec_count} frame={frame_no} (taking 2292 bytes)")
        if n_video >= 9: break

print(f"\nReassembled {len(bs)} bytes")
print(f"Trimmed to frame_size=2888: bs[:2888]")
print("\nFirst 32 halfwords (LE):")
for i in range(32):
    v = struct.unpack_from("<H", bs, i*2)[0]
    print(f"  [{i:2d}] 0x{v:04x}")

# Save to file for the decoder
with open("C:/Claude/silenthill/silent-hill-decomp/pc_port/build/c1_frame1_bs.bin", "wb") as f:
    f.write(bs[:2888])
print("\nWrote build/c1_frame1_bs.bin (2888 bytes)")
