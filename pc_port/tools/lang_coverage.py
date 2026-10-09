#!/usr/bin/env python3
r"""What each language actually shows in game, and what is still English.

Counting entries in a .lang file understates coverage badly, because the menus
and the F10 overlay for German, French, Spanish and Italian are not in the
packs at all -- they are compiled tables (s_MenuTr in lang_menu.c, the rows in
lang_quick_pal.inc) that a pack of the same name reads through its column. So
this reports the union, which is what a player sees, and lists what is left.

  python lang_coverage.py              # the summary table
  python lang_coverage.py missing de   # every untranslated key for one language
"""
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
LANG_DIR = os.path.join(HERE, '..', 'assets', 'gamedata', 'lang')
TEMPLATE = os.path.join(HERE, '..', 'localization',
                        'SilentHill_EN_for_translation.txt')

COLS = ['de', 'fr', 'es', 'it']
STR = r'"((?:[^"\\]|\\.)*)"'
ROW = r'\{\s*((?:' + STR + r'\s*)+),\s*\{(.*?)\}\s*\}'


def c_unescape(s):
    """C source escapes -> the text the compiler produces.

    Needed to compare against the template at all: a colour byte is written
    "\x07" in the source, four characters, while the template key holds the
    real 0x07 -- so 17 menu literals never matched and read as untranslated."""
    out, i = bytearray(), 0
    while i < len(s):
        if s[i] != chr(92) or i + 1 >= len(s):
            out += s[i].encode('latin-1', 'replace')
            i += 1
            continue
        c = s[i + 1]
        if c == 'x':
            j = i + 2
            h = ''
            while j < len(s) and len(h) < 2 and s[j] in '0123456789abcdefABCDEF':
                h += s[j]
                j += 1
            out.append(int(h, 16))
            i = j
        else:
            out.append({'n': 10, 't': 9, 'r': 13, '0': 0}.get(c, ord(c)))
            i += 2
    return out.decode('latin-1')


def c_rows(path, start=None, cols=4):
    """Rows of a { "us", { a, b, c, d } } table, us -> [4 values or None].

    A column may be several adjacent literals rather than one: the table splits
    after a \\xNN escape whose next character is a hex digit ("Fran\\xE7" "ais"),
    because the escape would otherwise swallow it. So columns are split on the
    commas and each column's literals are joined -- counting literals instead
    would see five in a four-column row and skip it, which silently hid every
    accented translation in the table."""
    s = io.open(path, encoding='utf-8', errors='surrogateescape').read()
    if start:
        s = s[s.index(start):]
        s = s[:s.index('\n};')]
    out = {}
    for m in re.finditer(ROW, s, re.S):
        us = ''.join(re.findall(STR, m.group(1)))
        parts = [p.strip() for p in re.split(r',(?![^"]*"(?:[^"]*"[^"]*")*[^"]*$)',
                                             m.group(3))]
        parts = [p for p in parts if p]
        if len(parts) != cols:
            continue
        vals = []
        for p in parts:
            lits = re.findall(STR, p)
            vals.append(c_unescape(''.join(lits)) if lits else None)
        out[c_unescape(us)] = vals
    return out


def template_keys():
    s = io.open(TEMPLATE, encoding='utf-8').read()
    keys, cur = [], None
    for ln in s.split('\n'):
        m = re.match(r'^\[(.+)\]\s*$', ln)  # keys contain ']', e.g. [MENU.[R] Reset]
        if m:
            cur = m.group(1)
            keys.append([cur, None])
        elif cur and ln.startswith('EN: '):
            keys[-1][1] = ln[4:]
            cur = None
    return keys


CODE = re.compile(r'~[A-Z][0-9]?(\([0-9.]*\))?')


def needs_translation(key, en):
    """False for an entry a translation cannot differ in.

    58 story entries carry no words at all -- a timed pause and an end marker
    ("~J0(13.2) ~E"), a select prompt ("~S3"), or a "=======" separator. A pack
    rightly has no entry for them and the English fallback is identical, so
    counting them as untranslated overstated every language by 58 lines."""
    if not key.startswith(('COMMON', 'MAP')):
        return True
    return bool(re.search(r'[^\W_]', CODE.sub('', en or ''), re.UNICODE))


def kind(k):
    p = k.split('.')[0]
    return 'story' if p.startswith(('COMMON', 'MAP')) else p


def pack_keys(code):
    p = os.path.join(LANG_DIR, code + '.lang')
    ks = set()
    if not os.path.exists(p):
        return ks
    for ln in io.open(p, 'rb').read().split(b'\n'):
        if ln and not ln.startswith((b'#', b'!')) and b'=' in ln:
            ks.add(ln.split(b'=', 1)[0].decode('ascii', 'replace'))
    return ks


def menu_key(us):
    return 'MENU.' + us.replace(' ', '_').replace('=', '-')


def quick_key(en):
    return 'QUICK.' + en.replace(' ', '_').replace('=', '-')


def covered(code, keys):
    """Every template key this language shows translated, from any source."""
    got = set(k for k in keys if k in pack_keys(code))

    if code in COLS:
        col = COLS.index(code)
        for us, vals in MENU.items():
            if vals[col]:
                got.add(menu_key(us))
        for en, vals in QUICK.items():
            if vals[col]:
                got.add(quick_key(en))
    return got


def supplement_same(code):
    """A language's own record of entries where English IS the translation.

    Expressed as a SAME set in localization/<code>_supplement.py, which is the
    pack equivalent of an explicit NULL column in the compiled tables: a name
    that does not change (Harry, Katana), a technical term Polish keeps (ACES,
    Aniso 8x), the engine's own "NO STAGE!" placeholder. Nothing is written for
    them -- an identical value is dropped on import and the fallback yields the
    same text -- so without this they read as untranslated forever."""
    path = os.path.join(HERE, '..', 'localization', '%s_supplement.py' % code)
    if not os.path.exists(path):
        return set()
    import importlib.util
    spec = importlib.util.spec_from_file_location('sup_' + code, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return set(getattr(mod, 'SAME', ()))


def decided(code):
    """Keys deliberately left English: the row exists and its column is NULL.

    VSync, PGXP, ACES, Aniso 8x and the like are the same word in every one of
    these languages, so NULL is the answer rather than a gap. Separating the two
    is the difference between 'nobody looked at this' and 'we looked'."""
    out = supplement_same(code)
    if code not in COLS:
        return out
    col = COLS.index(code)
    for us, vals in MENU.items():
        if vals[col] is None:
            out.add(menu_key(us))
    for en, vals in QUICK.items():
        if vals[col] is None:
            out.add(quick_key(en))
    return out


MENU = c_rows(os.path.join(HERE, '..', 'src', 'lang_menu.c'), 's_MenuTr[] = {')
QUICK = c_rows(os.path.join(HERE, '..', 'src', 'lang_quick_pal.inc'))

# ja/zh draw their own UI text from compiled tables the port writes, not from
# the pack, so credit those the same way.
JPN_UI = os.path.join(HERE, '..', 'src', 'lang_jpn_pcopt.inc')


ALL_ITEM_KEYS = set()  # filled in main() once the template is read


def jpn_item_keys():
    """Japanese item text is compiled in (INVENTORY_ITEM_NAMES_JPN and
    ITEM_DESCRIPTIONS_JPN), installed by Pc_LangInit rather than carried in the
    pack, so ja.lang holds none of it and it is not a gap."""
    path = os.path.join(HERE, '..', 'src', 'lang_jpn_items.inc')
    if not os.path.exists(path):
        return set()
    s = io.open(path, encoding='utf-8', errors='surrogateescape').read()
    out = set()
    # Two flat arrays, one entry per item index in order.
    for array, prefix in (('INVENTORY_ITEM_NAMES_JPN', 'ITEM_NAME'),
                          ('ITEM_DESCRIPTIONS_JPN', 'ITEM_DESC')):
        m = re.search(re.escape(array) + r'\[\]\s*=\s*\{(.*?)\n\};', s, re.S)
        if not m:
            continue
        # The array's own index is not the template's item id (the template's
        # run past 131, the arrays hold 78), and matching them up would prove
        # nothing: these tables ARE the decomp's NTSC-J branch, so they are
        # complete by construction and the runtime installs them wholesale.
        # Credit the set rather than guess at a mapping.
        if re.search(STR, m.group(1)):
            out |= set(k for k in ALL_ITEM_KEYS if k.startswith(prefix))
    return out


def jpn_pcopt_keys():
    s = io.open(JPN_UI, encoding='utf-8', errors='surrogateescape').read()
    out = set()
    for m in re.finditer(r'\{\s*' + STR + r'\s*,\s*' + STR, s):
        out.add(menu_key(m.group(1)))
    return out


def main():
    keys = [(k, e) for k, e in template_keys() if needs_translation(k, e)]
    ALL_ITEM_KEYS.update(k for k, _e in keys if kind(k).startswith('ITEM_'))
    allk = [k for k, _en in keys]
    kinds = ('story', 'MENU', 'QUICK', 'ITEM_NAME', 'ITEM_DESC')
    by = {g: [k for k in allk if kind(k) == g] for g in kinds}

    if len(sys.argv) > 2 and sys.argv[1] == 'missing':
        code = sys.argv[2]
        got = covered(code, allk)
        if code == 'ja':
            got |= jpn_pcopt_keys() | jpn_item_keys()
        got |= decided(code)
        en = dict(keys)
        n = 0
        for g in kinds:
            miss = [k for k in by[g] if k not in got]
            if not miss:
                continue
            print('--- %s: %d missing' % (g, len(miss)))
            for k in miss:
                print('%-26s %s' % (k, (en.get(k) or '')[:90]))
                n += 1
        print('\n%s: %d untranslated of %d' % (code, n, len(allk)))
        return

    print('%-7s %s %11s %11s' % ('', '  '.join('%-11s' % g for g in kinds),
                                  'translated', 'English'))
    for code in ('de', 'fr', 'es', 'it', 'pl', 'pt', 'ru', 'ja', 'zh', 'en_pal'):
        got = covered(code, allk)
        if code == 'ja':
            got |= jpn_pcopt_keys() | jpn_item_keys()
        dec = decided(code) - got
        cells = []
        for g in kinds:
            tot = len(by[g])
            have = sum(1 for k in by[g] if k in got)
            cells.append('%d/%d' % (have, tot))
        total = sum(1 for k in allk if k in got)
        left = len(allk) - total - len(dec)
        print('%-7s %s  %5d/%d  %4d chosen  %d LEFT'
              % (code, '  '.join('%-11s' % c for c in cells), total, len(allk),
                 len(dec), left))


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    main()
