"""Check cafe health drink placement symbols use the map-declared pose layout."""
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "include" / "maps" / "map0" / "map0_s01.h"
DATA = ROOT / "pc_port" / "build_gen" / "extracted_data" / "map0_s01_extracted_data.c"
SYMBOLS = ("D_800DE12C", "D_800DE140")


def find_type(pattern, text, symbol):
    match = re.search(pattern.format(symbol=re.escape(symbol)), text, re.MULTILINE)
    return match.group(1) if match else None


def main() -> int:
    header_text = HEADER.read_text(encoding="utf-8")
    data_text = DATA.read_text(encoding="utf-8")
    errors = []

    for symbol in SYMBOLS:
        declared_type = find_type(r"extern\s+(\w+)\s+{symbol}\s*;", header_text, symbol)
        defined_type = find_type(r"^\s*(\w+)\s+{symbol}\s*=", data_text, symbol)

        if declared_type is None:
            errors.append(f"{symbol}: no extern declaration found in {HEADER}")
            continue
        if defined_type is None:
            errors.append(f"{symbol}: no definition found in {DATA}")
            continue
        if defined_type != declared_type:
            errors.append(
                f"{symbol}: extracted data defines {defined_type}, "
                f"but the map declares {declared_type}"
            )
        if defined_type != "s_Pose":
            errors.append(f"{symbol}: cafe item placement must be s_Pose, not {defined_type}")

    if errors:
        print("map0_s01 pose layout check failed:")
        for error in errors:
            print(f"  - {error}")
        return 1

    print("map0_s01 cafe health drink pose layout check passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
