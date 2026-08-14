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
w.write(' * Regenerate with pc_port/tools/gen_jpn_items.py if the decomp tables change. */\n\n')
w.write('static const char* const INVENTORY_ITEM_NAMES_JPN[] = {\n')
w.write(nbody.rstrip() + '\n};\n\n')
w.write('static const char* const ITEM_DESCRIPTIONS_JPN[] = {\n')
w.write(dbody.rstrip() + '\n};\n')
w.close()
print('names entries  ~%d' % count_entries(nbody))
print('descs entries  ~%d' % count_entries(dbody))
print('wrote', OUT)
