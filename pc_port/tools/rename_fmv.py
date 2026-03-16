#!/usr/bin/env python3
"""Rename jpsxdec-extracted HILL[nnn].avi files to their actual FMV names.

Usage: python rename_fmv.py <input_dir> <output_dir>
   or: python rename_fmv.py <input_dir>          (renames in place)

HILL[726] through HILL[746] map to the 21 FMV files on the Silent Hill (USA) disc.
"""

import sys
import os
import shutil

# HILL[726]..HILL[746] → FMV names (in disc order)
FMV_NAMES = [
    "C1_20670",  # HILL[726]
    "C2_20670",  # HILL[727]
    "M1_03500",  # HILL[728]
    "M2_01190",  # HILL[729]
    "M3_02570",  # HILL[730]
    "M4_02490",  # HILL[731]
    "M5_03140",  # HILL[732]
    "M6_02112",  # HILL[733]
    "M7_01536",  # HILL[734]
    "M8_03039",  # HILL[735]
    "M9_01730",  # HILL[736]
    "MA_03590",  # HILL[737]
    "MB_04850",  # HILL[738]
    "MC_01930",  # HILL[739]
    "MD_03780",  # HILL[740]
    "ME_03300",  # HILL[741]
    "Z1_16180",  # HILL[742]
    "Z3_02340",  # HILL[743]
    "Z4_01590",  # HILL[744]
    "ZC_14392",  # HILL[745]
    "ZZ_14239",  # HILL[746]
]

FIRST_INDEX = 726

def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <input_dir> [output_dir]")
        sys.exit(1)

    input_dir = sys.argv[1]
    output_dir = sys.argv[2] if len(sys.argv) > 2 else input_dir

    if not os.path.isdir(input_dir):
        print(f"Error: {input_dir} is not a directory")
        sys.exit(1)

    os.makedirs(output_dir, exist_ok=True)

    renamed = 0
    for i, name in enumerate(FMV_NAMES):
        idx = FIRST_INDEX + i
        src = os.path.join(input_dir, f"HILL[{idx}].avi")
        dst = os.path.join(output_dir, f"{name}.avi")

        if not os.path.exists(src):
            print(f"  SKIP: HILL[{idx}].avi not found")
            continue

        if input_dir == output_dir:
            os.rename(src, dst)
        else:
            shutil.copy2(src, dst)

        print(f"  HILL[{idx}].avi -> {name}.avi")
        renamed += 1

    print(f"\nRenamed {renamed}/{len(FMV_NAMES)} files.")

if __name__ == "__main__":
    main()
