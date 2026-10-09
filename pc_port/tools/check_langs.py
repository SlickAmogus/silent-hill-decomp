#!/usr/bin/env python3
"""Check every shipped language pack before it goes out.

These are the failures that are invisible until someone plays in that language
for an hour, and every one of them has actually happened: a pack with no !font
line whose built glyphs were therefore never painted, a Cyrillic pack with
Latin letters in entries drawn through an atlas that has no Latin capitals, a
menu label too wide for the row, a key containing the '=' the loader splits on.

Static checks only -- nothing here needs the game. For the runtime half (did
the story text, the item text and the font actually install?) use the LANGCHECK
console command, which reports each layer separately.

Usage:  python check_langs.py            # the shipped packs
        python check_langs.py <dir>      # a mod's lang folder
"""
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
LANG_DIR = os.path.join(HERE, '..', 'assets', 'gamedata', 'lang')

BS = bytes([92])
FONTS = ('latin', 'polish', 'cyrillic', 'sjis', 'chinese', 'portuguese')

# The row right-aligns a label that does not fit, but only to here (lang_text.c
# Pc_LangSlotNameX): past this it would collide with the row's own caption.
ROW_RIGHT = 264
ROW_LEFT_FLOOR = 130


def glyph_widths():
    s = io.open(os.path.join(REPO, 'src', 'bodyprog', 'text', 'text_draw.c'),
                encoding='utf-8', errors='surrogateescape').read()
    m = re.search(r'FONT_12X16_GLYPH_WIDTHS\[FONT_12X16_GLYPH_COUNT\] = \{(.*?)\};',
                  s, re.S)
    return [int(x, 0) for x in re.findall(r'(0x[0-9a-fA-F]+|\d+)', m.group(1))]


W = glyph_widths()


def drawn_width(text):
    """Width in pixels as Pc_LangMenuTextWidth measures it."""
    total = 0
    for ch in text:
        if ch == '_':
            total += 6
            continue
        i = ord(ch) - 0x27
        total += W[i] if 0 <= i < len(W) else 12
    return total


def strip_codes(v):
    """Drop the engine's ~X control codes. They are spelled with ASCII letters
    (~N, ~E, ~C2, ~J0(2.5)), so a Latin-letter search would flag every line."""
    return re.sub(rb'~[A-Z][0-9]?(\([0-9.]*\))?', b'', v)


def unescape(v):
    out, i = bytearray(), 0
    while i < len(v):
        if v[i:i + 1] == BS and i + 1 < len(v):
            c = v[i + 1:i + 2]
            out += b'\n' if c == b'n' else b'\t' if c == b't' else c
            i += 2
            continue
        out.append(v[i])
        i += 1
    return bytes(out)


def check(path):
    name = os.path.basename(path)
    problems = []
    notes = []
    header = {}
    entries = []

    raw = io.open(path, 'rb').read()
    lines = raw.split(b'\n')
    for n, ln in enumerate(lines, 1):
        if not ln or ln.startswith(b'#'):
            continue
        if ln.startswith(b'!'):
            k, _, v = ln[1:].partition(b'=')
            header[k.decode('ascii', 'replace')] = v
            continue
        if b'=' not in ln:
            problems.append('line %d: no "=" and not a comment' % n)
            continue
        k, v = ln.split(b'=', 1)
        entries.append((n, k, v))

    for need in ('code', 'name', 'menu', 'font'):
        if need not in header:
            problems.append('missing !%s' % need)
    font = header.get('font', b'').decode('ascii', 'replace')
    if 'font' in header and font not in FONTS:
        problems.append('!font=%s is not one of %s' % (font, '/'.join(FONTS)))

    # The first header line must be !font: the loader needs it before it
    # transcodes any value.
    first = next((l for l in lines if l.startswith(b'!')), b'')
    if not first.startswith(b'!font='):
        problems.append('!font must be the FIRST header line (loader transcodes '
                        'values as it reads them)')

    menu = header.get('menu', b'').decode('utf-8', 'replace')
    if ' ' in menu:
        problems.append('!menu=%r contains a literal space; the game draws "_" '
                        'as a space' % menu)
    if menu and font != 'cyrillic' and font != 'sjis':
        w = drawn_width(menu)
        x = max(min(198, ROW_RIGHT - w), ROW_LEFT_FLOOR)
        if x + w > ROW_RIGHT + 2:
            problems.append('!menu=%r is %dpx: too wide for the Language row '
                            '(would reach x=%d, limit %d)' % (menu, w, x + w, ROW_RIGHT))

    for n, k, v in entries:
        if b'=' in k:
            problems.append('line %d: key contains "=", which the loader splits on' % n)
        try:
            k.decode('ascii')
        except UnicodeDecodeError:
            problems.append('line %d: key is not ASCII' % n)
        if font in ('sjis', 'chinese'):
            try:
                unescape(v).decode('cp932')
            except UnicodeDecodeError:
                problems.append('line %d (%s): value is not a valid %s byte sequence'
                                % (n, k.decode('ascii', 'replace'), font))

    # A Cyrillic pack draws through an atlas with no Latin capitals, so a Latin
    # letter in a game-font entry renders as the wrong glyph or nothing. QUICK.*
    # is the TrueType overlay and may hold anything.
    if font == 'cyrillic':
        leaked = [k.decode('ascii', 'replace') for n, k, v in entries
                  if not k.startswith(b'QUICK.')
                  and re.search(b'[A-Za-z]', strip_codes(unescape(v)))]
        if leaked:
            notes.append('%d entries contain Latin letters (%s%s) -- fine only if '
                         'they are meant to be unreadable' %
                         (len(leaked), ', '.join(leaked[:4]),
                          ', ...' if len(leaked) > 4 else ''))

    kinds = {}
    for _, k, _v in entries:
        kinds[k.split(b'.')[0].decode('ascii', 'replace')] = \
            kinds.get(k.split(b'.')[0].decode('ascii', 'replace'), 0) + 1
    story = sum(c for p, c in kinds.items() if p.startswith(('COMMON', 'MAP')))
    shape = 'story %d, menu %d, item %d' % (
        story, kinds.get('MENU', 0),
        kinds.get('ITEM_NAME', 0) + kinds.get('ITEM_DESC', 0))

    return name, header, len(entries), shape, problems, notes


def main():
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    d = sys.argv[1] if len(sys.argv) > 1 else LANG_DIR
    files = sorted(f for f in os.listdir(d) if f.endswith('.lang'))
    if not files:
        sys.exit('no .lang files in %s' % d)

    bad = 0
    for f in files:
        name, header, count, shape, problems, notes = check(os.path.join(d, f))
        label = header.get('menu', b'?').decode('utf-8', 'replace')
        print('%-12s %-16s %4d entries  (%s)'
              % (name, label, count, shape))
        for p in problems:
            print('   FAIL  %s' % p)
            bad += 1
        for n in notes:
            print('   note  %s' % n)
    print()
    print('%d pack(s), %d problem(s)' % (len(files), bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
