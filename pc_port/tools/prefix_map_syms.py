#!/usr/bin/env python3
"""Rewrite every symbol a map overlay DEFINES to <mapname>_<symbol>.

Why this exists: on PSX only one map overlay is resident at a time, so every map
carries its own copy of the shared AI/particle/player code that it #includes.
Statically linking all 43 into one binary therefore collides on 500+ globals
(see pc_port/CMakeLists.txt, MAP_SOURCES). The DLL build sidesteps this by
keeping each map in its own module, but iOS refuses to load dylibs that are not
part of the signed bundle, so the static path needs the collisions resolved at
link time instead. Prefixing per map reproduces PSX overlay semantics exactly:
each overlay keeps a private copy of the shared code and data.

Only DEFINED, EXTERNAL symbols are renamed. Undefined references are left alone
so map code still binds to the one real engine copy, and local symbols are
already per-object.

Mach-O prefixes C symbols with an underscore and ELF/COFF-x86_64 do not, so the
new name has to be built around that prefix rather than pasted in front of the
raw symbol: _Foo must become _map0_s01_Foo, not map0_s01__Foo.
"""
import argparse
import subprocess
import sys
from pathlib import Path


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("%s failed (%d):\n%s" % (Path(cmd[0]).name, r.returncode, r.stderr.strip()))
    return r.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nm", required=True, help="llvm-nm executable")
    ap.add_argument("--objcopy", required=True, help="llvm-objcopy executable")
    ap.add_argument("--archive", required=True, help="static library to rewrite in place")
    ap.add_argument("--map-name", required=True, help="overlay name, e.g. map0_s01")
    ap.add_argument("--sym-prefix", default="", help="platform C symbol prefix ('_' on Mach-O)")
    ap.add_argument("--stamp", help="file to touch on success, for the build graph")
    args = ap.parse_args()

    archive = Path(args.archive)
    if not archive.is_file():
        sys.exit("archive not found: %s" % archive)

    listing = run([args.nm, "--defined-only", "--extern-only",
                   "--format=just-symbols", str(archive)])

    p = args.sym_prefix
    renames = {}
    for line in listing.splitlines():
        sym = line.strip()
        # llvm-nm interleaves "member.o:" headers and blank lines when reading an
        # archive; neither is a symbol.
        if not sym or sym.endswith(":"):
            continue
        if p and not sym.startswith(p):
            # Not an ordinary C symbol under this ABI (assembler-local, etc.).
            continue
        bare = sym[len(p):] if p else sym
        # Skip compiler-generated symbols, which are not the overlay's own
        # globals. MinGW emits a `.refptr.<name>` COMDAT thunk holding the
        # address of each symbol it imports from the exe; renaming those
        # desyncs the thunk from the COMDAT the linker merges it by, and every
        # engine reference (g_SysWork, g_DeltaTime, ...) comes back undefined.
        # Each overlay keeping its own copy is correct and already how the DLL
        # build works. No ABI names a real C global starting with '.'.
        if not (bare[:1].isalpha() or bare[:1] == "_"):
            continue
        # Idempotent: re-running over an already-prefixed archive must be a no-op,
        # otherwise an incremental rebuild double-prefixes and the registry's
        # extern stops resolving.
        if bare.startswith(args.map_name + "_"):
            continue
        renames[sym] = "%s%s_%s" % (p, args.map_name, bare)

    if not renames:
        print("[prefix_map_syms] %s: nothing to rename (already prefixed?)" % args.map_name)
    else:
        listfile = archive.with_suffix(archive.suffix + ".renames")
        listfile.write_text(
            "".join("%s %s\n" % kv for kv in sorted(renames.items())), encoding="utf-8")
        run([args.objcopy, "--redefine-syms=%s" % listfile, str(archive)])
        print("[prefix_map_syms] %s: renamed %d symbols" % (args.map_name, len(renames)))

    if args.stamp:
        Path(args.stamp).write_text("ok\n", encoding="utf-8")


if __name__ == "__main__":
    main()
