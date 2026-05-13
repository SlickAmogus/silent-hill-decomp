"""Examine sectors at start of each FMV-named file from g_FileTable."""
import sys, struct
path = sys.argv[1]

fmv_starts = [
    (0x20807, "C1_20670"),
    (0x258c5, "C2_20670"),
    (0x2a983, "M1_03500"),
    (0x340e8, "Z1_16180"),
    (0x38f76, "ZC_14392"),
]

with open(path, "rb") as f:
    for start_sector, name in fmv_starts:
        print(f"=== {name} @ 0x{start_sector:05x} ({start_sector}) ===")
        for off in range(3):
            s = start_sector + off
            f.seek(s * 2352)
            d = f.read(64)
            print(f"  sector {s}:")
            print(f"    sync_hdr: {d[:16].hex(' ')}")
            print(f"    subhdr:   {d[16:24].hex(' ')}  submode=0x{d[18]:02x}")
            print(f"    data[24:48]: {d[24:48].hex(' ')}")
            # Check for 0x3800 at various positions
            for ofs in [0x18, 0x20, 0x28, 0x30, 0x38]:
                if d[ofs] == 0x00 and d[ofs+1] == 0x38:
                    print(f"    3800h at offset 0x{ofs:02x}!")
