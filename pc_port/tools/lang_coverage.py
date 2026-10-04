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


def c_rows(path, start=None, cols=4):
    """Rows of a { "us", { a, b, c, d } } table, us -> [4 values or None]."""
    s = io.open(path, encoding='utf-8', errors='surrogateescape').read()
    if start:
        s = s[s.index(start):]
        s = s[:s.index('\n};')]
    out = {}
    for m in re.finditer(ROW, s, re.S):
        us = ''.join(re.finditer and re.findall(STR, m.group(1)))
        vals = re.findall(STR + r'|\bNULL\b', m.group(3))
        if len(vals) != cols:
            continue
        out[us] = [v if v else None for v in vals]
    return out


def template_keys():
    s = io.open(TEMPLATE, encoding='utf-8').read()
    keys, cur = [], None
    for ln in s.split('\n'):
        m = re.match(r'^\[([^\]]+)\]', ln)
        if m:
            cur = m.group(1)
            keys.append([cur, None])
        elif cur and ln.startswith('EN: '):
            keys[-1][1] = ln[4:]
            cur = None
    return keys


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


MENU = c_rows(os.path.join(HERE, '..', 'src', 'lang_menu.c'), 's_MenuTr[] = {')
QUICK = c_rows(os.path.join(HERE, '..', 'src', 'lang_quick_pal.inc'))

# ja/zh draw their own UI text from compiled tables the port writes, not from
# the pack, so credit those the same way.
JPN_UI = os.path.join(HERE, '..', 'src', 'lang_jpn_pcopt.inc')


def jpn_pcopt_keys():
    s = io.open(JPN_UI, encoding='utf-8', errors='surrogateescape').read()
    out = set()
    for m in re.finditer(r'\{\s*' + STR + r'\s*,\s*' + STR, s):
        out.add(menu_key(m.group(1)))
    return out


def main():
    keys = template_keys()
    allk = [k for k, _en in keys]
    kinds = ('story', 'MENU', 'QUICK', 'ITEM_NAME', 'ITEM_DESC')
    by = {g: [k for k in allk if kind(k) == g] for g in kinds}

    if len(sys.argv) > 2 and sys.argv[1] == 'missing':
        code = sys.argv[2]
        got = covered(code, allk)
        if code == 'ja':
            got |= jpn_pcopt_keys()
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

    print('%-7s %s' % ('', '  '.join('%-11s' % g for g in kinds)))
    for code in ('de', 'fr', 'es', 'it', 'pl', 'ru', 'ja', 'zh', 'en_pal'):
        got = covered(code, allk)
        if code == 'ja':
            got |= jpn_pcopt_keys()
        cells = []
        for g in kinds:
            tot = len(by[g])
            have = sum(1 for k in by[g] if k in got)
            cells.append('%d/%d' % (have, tot))
        total = sum(1 for k in allk if k in got)
        print('%-7s %s  = %d/%d' % (code, '  '.join('%-11s' % c for c in cells),
                                    total, len(allk)))


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    main()
