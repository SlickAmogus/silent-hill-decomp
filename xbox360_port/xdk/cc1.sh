#!/usr/bin/env bash
# One compile for gate.sh / build_xex.sh, run under xargs -P.
#   cc1.sh "<src>|<obj>|<extra flags>"
#
# A C source is first compiled as-is. If cl rejects it, the TU goes through
# the C99->C89 transpiler (tools/c99to89.py) and is compiled again from the
# generated file; most of the tree needs that, since MSVC 16 is C89.
#
# Leaves <obj>.err on failure, <obj>.warn when a successful compile said
# anything (including "needed transpile"). FLAGS / DEFS / INCS arrive in the
# environment, as does XDK_TOOLS (the tools dir) and CLANG.
IFS='|' read -r src obj extra <<< "$1"
case "$src" in
    *.cpp) lang="-TP" ;;
    *)     lang="-TC" ;;
esac
rm -f "$obj" "$obj.err" "$obj.warn"

compile() {   # $1 = source to compile
    local out rc
    out=$(cl $FLAGS $lang $DEFS $INCS $extra -Fo"$(cygpath -w "$obj")" "$(cygpath -w "$1")" 2>&1)
    rc=$?
    # cl echoes the bare source filename as its first line; drop it.
    printf '%s\n' "$out" | grep -v "^$(basename "$1")\r\?$"
    return $rc
}

direct=$(compile "$src")
if [ $? -eq 0 ] && [ -f "$obj" ]; then
    [ -n "$direct" ] && printf '%s\n' "$direct" > "$obj.warn"
    exit 0
fi
if [ "$lang" = "-TP" ]; then
    printf '%s\n' "$direct" > "$obj.err"
    exit 0
fi

# Transpile. The clang -I/-D set is the cl one: cl accepts -I<path> and
# -D<x> in the same spelling clang does. Python and clang are Windows
# programs and MSYS path conversion is off (xdk_env.sh), so every path they
# receive is converted here explicitly.
gen="$obj.c89.c"
tools="$(cygpath -w "$XDK_TOOLS")"
tp=$(python "$tools\\c99to89.py" --clang "$(cygpath -w "$CLANG")" \
        --fake "$tools\\fake_libc_include" --fake-extra "$tools\\fake_extra" \
        --compat "$(cygpath -w "$XDK_COMPAT")" \
        $INCS \
        $(for d in $DEFS $extra; do case "$d" in -D*) echo "$d";; esac; done) \
        -o "$(cygpath -w "$gen")" "$(cygpath -w "$src")" 2>&1)
if [ $? -ne 0 ]; then
    { echo "[direct compile failed, then transpile failed]"; printf '%s\n' "$tp"; echo "--- direct:"; printf '%s\n' "$direct"; } > "$obj.err"
    exit 0
fi
second=$(compile "$gen")
if [ $? -eq 0 ] && [ -f "$obj" ]; then
    { echo "[transpiled]"; [ -n "$second" ] && printf '%s\n' "$second"; } > "$obj.warn"
    exit 0
fi
{ echo "[transpiled, then compile failed] $gen"; printf '%s\n' "$second"; } > "$obj.err"
exit 0
