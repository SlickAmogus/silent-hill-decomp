import io, re

SRC = r'C:\Claude\silenthill\silent-hill-decomp\src\bodyprog\items\item_screens_3.c'
OUT = r'C:\Claude\silenthill\silent-hill-decomp\pc_port\src\lang_jpn_items.inc'

lines = io.open(SRC, encoding='utf-8').read().split('\n')


def extract(start_idx, name_new):
    """Walk an array initializer, keeping the NTSCJ branch of every
    #if VERSION_REGION_IS(NTSCJ) and dropping the #else branch."""
    out = []
    i = start_idx
    # Stack of states, one per open conditional:
    #   'take'  = we are in the branch we want
    #   'drop'  = we are in the branch we discard
    #   'keep'  = an unrelated conditional, emitted verbatim
    st = []
    RESOLVED = ('#if VERSION_REGION_IS(NTSCJ)', '#if VERSION_EQUAL_OR_NEWER(JAP1)')
    while i < len(lines):
        L = lines[i]
        s = L.strip()
        live = all(x != 'drop' for x in st)
        if any(s.startswith(r) for r in RESOLVED):
            st.append('take'); i += 1; continue
        if s.startswith('#if') :
            st.append('keep')
            if live: out.append(L)
            i += 1; continue
        if s == '#else' and st:
            top = st[-1]
            if top == 'take':   st[-1] = 'drop'
            elif top == 'drop': st[-1] = 'take'
            else:
                if live: out.append(L)
            i += 1; continue
        if s == '#endif' and st:
            top = st.pop()
            if top == 'keep' and all(x != 'drop' for x in st):
                out.append(L)
            i += 1; continue
        if s.startswith('};'):
            break
        if live:
            out.append(L)
        i += 1
    body = '\n'.join(out)
    n = body.count('"') // 2 + body.count('NULL')
    return body, i



HEXDIGITS = '0123456789abcdefABCDEF'


def to_sjis(body):
    r"""Rewrite every string literal's non-ASCII text as Shift-JIS \xNN escapes.

    The decomp keeps these sources in UTF-8 and the PSX build transcodes the
    preprocessed file (Makefile: iconv_sjis_wrapper.py -f UTF-8 -t SHIFT-JIS)
    before assembling, so the shipped bytes are Shift-JIS. The PC port compiles
    one binary straight from UTF-8 sources, so without this the literals stay
    UTF-8 -- and the game's text drawer reads them as Shift-JIS, where 0xE0..0xEF
    is a legal lead byte. Every 3-byte UTF-8 kanji was consumed as a 2-byte SJIS
    pair plus a stray, drawing a wrong glyph and drifting out of phase: the
    garbled inventory text. Escapes keep the .inc itself encoding-independent.
    """
    out = []
    i = 0
    in_str = False
    prev_was_hex = False
    while i < len(body):
        c = body[i]
        if not in_str:
            out.append(c)
            if c == '"':
                in_str = True
                prev_was_hex = False
            i += 1
            continue
        if c == '\\':                      # existing escape: copy verbatim
            out.append(body[i:i + 2])
            prev_was_hex = False
            i += 2
            continue
        if c == '"':
            out.append(c)
            in_str = False
            i += 1
            continue
        if ord(c) < 128:
            # "~N" is the MAP-MESSAGE newline, parsed by the message drawer.
            # Item descriptions go through Gfx_StringDraw, which knows only a
            # literal \\n -- and drops '~' outright, because 0x7E is above the
            # 'z' its glyph range ends at, leaving the 'N' to draw as a letter.
            # That is the stray N in the Japanese inventory descriptions.
            if c == '~' and i + 1 < len(body) and body[i + 1] == 'N':
                out.append('\\n')
                prev_was_hex = False
                i += 2
                continue
            # A hex escape swallows every hex digit that follows it, so break
            # the literal in two when plain text would extend one.
            if prev_was_hex and c in HEXDIGITS:
                out.append('" "')
            out.append(c)
            prev_was_hex = False
            i += 1
            continue
        for b in bytearray(c.encode('shift_jis')):
            out.append('\\x%02X' % b)
        prev_was_hex = True
        i += 1
    return ''.join(out)


# find the two arrays
ni = next(k for k, L in enumerate(lines) if L.startswith('const char* INVENTORY_ITEM_NAMES[]'))
di = next(k for k, L in enumerate(lines) if L.startswith('const char* g_ItemDescriptions[]'))

nbody, _ = extract(ni + 1, 'names')
dbody, _ = extract(di + 1, 'descs')


def count_entries(body):
    # count top-level comma-separated initializers
    c = 0
    for L in body.split('\n'):
        s = L.strip()
        if not s or s.startswith('//') or s.startswith('/*') or s.startswith('*') or s.startswith('#'):
            continue
        c += s.count(',')
        if s and not s.endswith(',') and (s.startswith('"') or s.startswith('NULL')):
            c += 1
    return c


w = io.open(OUT, 'w', encoding='utf-8', newline='\n')
w.write('/* SPDX-License-Identifier: GPL-3.0-or-later */\n')
w.write('/* GENERATED from src/bodyprog/items/item_screens_3.c -- the NTSC-J branch of\n')
w.write(' * INVENTORY_ITEM_NAMES / g_ItemDescriptions. The decomp selects those at COMPILE\n')
w.write(' * time via VERSION_REGION_IS(NTSCJ); the PC port builds one binary and picks the\n')
w.write(' * region at RUNTIME, so the Japanese tables have to be compiled alongside the\n')
w.write(' * English ones and installed when g_GameRegion == Region_JPN.\n')
w.write(" * Regenerate with pc_port/tools/gen_jpn_items.py if the decomp tables change.\n"
        " *\n"
        " * Text is SHIFT-JIS, written as escapes. The decomp sources are UTF-8 and the\n"
        " * PSX build transcodes them (Makefile: iconv_sjis_wrapper.py); the PC port has\n"
        " * no such step, and the drawer reads Shift-JIS -- where 0xE0..0xEF is a legal\n"
        " * lead byte, so UTF-8 kanji rendered as garbage. */\n\n")
w.write('static const char* const INVENTORY_ITEM_NAMES_JPN[] = {\n')
w.write(to_sjis(nbody).rstrip() + '\n};\n\n')
w.write('static const char* const ITEM_DESCRIPTIONS_JPN[] = {\n')
w.write(to_sjis(dbody).rstrip() + '\n};\n')
w.close()
print('names entries  ~%d' % count_entries(nbody))
print('descs entries  ~%d' % count_entries(dbody))
print('wrote', OUT)
