# SPDX-License-Identifier: GPL-3.0-or-later
"""Bridge C and C++ global variables across the MSVC ABI.

The port shares plain globals between C and C++ translation units without
extern "C". The Itanium ABI every other target uses does not mangle a
namespace-scope variable, so `int g_x;` in a .cpp is the same symbol as
`extern int g_x;` in a .c. The MSVC ABI encodes the type (?g_x@@3HA), and the
two no longer meet.

This writes one /ALTERNATENAME per such variable, in both directions, so the
linker falls back from the spelling one language asks for to the spelling the
other defined. An alternate name only applies while the first name is
undefined, so listing a variable nobody needs bridged costs nothing.

The output is a response file for the clang driver, which expands @file
itself, hence the -Xlinker in front of each option.

usage: gen_alternatenames.py --nm <llvm-nm> --out <file.rsp> <objects and libs>...
"""
import argparse
import glob
import re
import subprocess

# A global-namespace data symbol: ?name@@3<type><storage>
MANGLED_VAR = re.compile(r"^\?([A-Za-z_]\w*)@@3\S+$")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nm", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("inputs", nargs="+")
    args = ap.parse_args()

    files = []
    for pattern in args.inputs:
        files += glob.glob(pattern, recursive=True) or [pattern]

    aliases = set()
    for i in range(0, len(files), 64):
        out = subprocess.run([args.nm, "--format=posix", *files[i:i + 64]],
                             capture_output=True, text=True, errors="replace").stdout
        for line in out.splitlines():
            parts = line.split()
            if len(parts) < 2:
                continue
            m = MANGLED_VAR.match(parts[0])
            if not m:
                continue
            if parts[1] == "U":
                aliases.add(f"-Xlinker /ALTERNATENAME:{parts[0]}={m.group(1)}")
            else:
                aliases.add(f"-Xlinker /ALTERNATENAME:{m.group(1)}={parts[0]}")

    with open(args.out, "w", newline="\n") as f:
        f.write("\n".join(sorted(aliases)) + "\n")


if __name__ == "__main__":
    main()
