#!/usr/bin/env sh
# gen_map_syms.sh <mapname> <output.syms> <obj>...
#
# Emits an llvm-objcopy --redefine-syms rename list that prefixes every
# DEFINED extern symbol of a map overlay's objects with the map's name:
#   _g_MapOverlayHeader_map0_s01 _map0_s01_g_MapOverlayHeader_map0_s01
# (raw COFF names; i386 cdecl prepends '_', so the C-level rename is
#  g_MapOverlayHeader_map0_s01 -> map0_s01_g_MapOverlayHeader_map0_s01).
#
# Applying the SAME list to all of the map's objs renames definitions and
# intra-map references consistently; undefined (shared game) symbols are left
# alone. Only '_'-prefixed symbols are renamed: MSVC-mangled COMDATs such as
# '??_C@...' string literals and '@feat.00' are content-keyed/internal and are
# deliberately left for the linker to dedup.
set -e

map="$1"
out="$2"
shift 2

llvm-nm --extern-only --defined-only "$@" \
    | awk -v pfx="_${map}" '$3 ~ /^_/ { print $3, pfx $3 }' \
    | sort -u > "$out"
