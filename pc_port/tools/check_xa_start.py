"""Dump first few sectors of each XA file start to confirm subheader format."""
import sys, struct

xa_starts = [0x099BF, 0x0A227, 0x0B377, 0x0D0BF, 0x0EA57, 0x0F997, 0x1096F, 0x16F07, 0x19797]

path = sys.argv[1]
with open(path, "rb") as f:
    for s in xa_starts:
        f.seek(s * 2352)
        d = f.read(64)
        sub = d[16:24]
        print(f"sector 0x{s:05x} ({s}):")
        print(f"  subheader: {sub.hex(' ')}")
        print(f"  submode=0x{d[18]:02x}  coding=0x{d[19]:02x}")
        # check if XA — audio (bit 2) and Form 2 (bit 5)
        is_audio = (d[18] & 0x04) != 0
        is_form2 = (d[18] & 0x20) != 0
        print(f"  audio={is_audio} form2={is_form2}")
