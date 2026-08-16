#!/usr/bin/env sh
# gen_map_syms.sh <mapname> <output.syms> <obj>...
#
# Emits a ppu-objcopy --redefine-syms rename list that prefixes every DEFINED
# extern symbol of a map overlay's objects with the map's name:
#   g_MapOverlayHeader_map0_s01 -> map0_s01_g_MapOverlayHeader_map0_s01
#
# Applying the SAME list to all of a map's objects renames definitions and
# intra-map references consistently, while undefined (shared game) symbols are
# left alone and still bind to bodyprog/pc_port -- the same binding a PC map DLL
# gets from the exe's import lib. That is what kills the 500+ cross-map
# duplicate symbols: the shared player/particle/chara sources are #included into
# every map, so each map defines its own copy of all of them.
#
# PPC64 ELFv1 WRINKLE, which the 32-bit ports do not have: every function has
# TWO symbols -- `foo`, the descriptor in .opd, and `.foo`, the actual code. The
# dot form has to be renamed as `.map_foo`, NOT `map_.foo`, or the descriptor
# and its code end up with unrelated names and the link fails on one of them.
set -e

map="$1"
out="$2"
shift 2

ppu-nm --extern-only --defined-only "$@" \
    | awk -v map="$map" '
        NF >= 3 {
            s = $3
            if (substr(s, 1, 1) == ".")
                print s, "." map "_" substr(s, 2)
            else
                print s, map "_" s
        }' \
    | sort -u > "$out"
