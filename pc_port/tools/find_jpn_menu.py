#!/usr/bin/env python3
"""Find more of the Japanese menu strings on the disc, instead of writing them.

lang_jpn_menu.inc maps 56 menu literals to offsets in VIN/OPTION.BIN and
VIN/SAVELOAD.BIN. About a hundred more of the port's menu literals are retail
strings that exist in Japanese on that disc and simply have no offset recorded:
LOAD, CONTINUE, the save/load messages, the inventory verbs.

The two discs share the layout -- "Sewer" sits at 0x0028 in SAVELOAD.BIN on the
PAL disc and lang_jpn_menu.inc lists 0x0028 for it on the Japanese one -- so the
ENGLISH disc can be used as an index: find a literal there, and the Japanese
string is at the same offset on the Japanese disc. That yields retail Japanese
rather than a translation of mine, and the Chinese PPF patches the same bytes,
so it answers for Chinese too.

Reports candidates for review; it does not write the .inc.

Usage:
    python find_jpn_menu.py "<gamedata>/Silent Hill (Japan).bin" \
                            "<gamedata>/Silent Hill (Europe) ....bin"
"""
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gen_zh_pack as disc  # noqa: E402
import lang_coverage as lc  # noqa: E402

REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
FILES = ('VIN/OPTION.BIN', 'VIN/SAVELOAD.BIN')


def ft(table, tag):
    s = io.open(os.path.join(REPO, 'src', 'main', table),
                encoding='utf-8', errors='surrogateescape').read()
    m = re.search(r'\{\s*(0x[0-9a-fA-F]+),\s*(\d+),.*?//\s*%s\b' % re.escape(tag), s)
    return (int(m.group(1), 16), int(m.group(2))) if m else None


def load(path, table):
    out = []
    for name in FILES:
        e = ft(table, name)
        out.append(disc.read_disc_file(path, e[0], e[1] * 256) if e else b'')
    return out


def known_offsets():
    """What lang_jpn_menu.inc already records, so this only reports new ones."""
    s = io.open(os.path.join(HERE, '..', 'src', 'lang_jpn_menu.inc'),
                encoding='utf-8').read()
    return {(int(m.group(2)), int(m.group(3), 16))
            for m in re.finditer(r'\{\s*"([^"]*)"\s*,\s*(\d+)\s*,\s*(0x[0-9a-fA-F]+)', s)}, \
           {m.group(1) for m in re.finditer(r'\{\s*"([^"]*)"\s*,\s*\d+\s*,\s*0x', s)}


def sjis_text(b):
    try:
        return b.decode('cp932')
    except UnicodeDecodeError:
        return None


def main():
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    if len(sys.argv) != 3:
        sys.exit(__doc__)

    jp = load(sys.argv[1], 'filetable.c.JAP1.inc')
    en = load(sys.argv[2], 'filetable.c.EUR.inc')
    _known_off, known_lit = known_offsets()

    # Every menu literal the port draws that Japanese has no text for.
    keys = [(k, e) for k, e in lc.template_keys() if lc.needs_translation(k, e)]
    want = {}
    for k, e in keys:
        if lc.kind(k) != 'MENU':
            continue
        lit = k[len('MENU.'):]
        if lit in known_lit:
            continue
        want[lit] = e or ''

    found, missing = [], []
    for lit, english in sorted(want.items()):
        # The disc stores the drawn form, with '_' for a space.
        probe = (english or lit).replace(' ', '_').encode('latin-1', 'replace')
        probe = probe.lstrip(b'\x07')
        hit = None
        for fi, data in enumerate(en):
            if not data or len(probe) < 3:
                continue
            at = data.find(probe)
            if at >= 0:
                hit = (fi, at)
                break
        if hit is None:
            missing.append(lit)
            continue

        fi, at = hit
        blob = jp[fi]
        end = blob.find(b'\0', at) if at < len(blob) else -1
        text = sjis_text(blob[at:end]) if end > at else None
        found.append((lit, fi, at, text))

    print('menu literals with no Japanese: %d' % len(want))
    print('  located in the English disc  : %d' % len(found))
    print('  not found as a literal       : %d' % len(missing))
    print()
    ok = [f for f in found if f[3]]
    print('%d of those decode as Shift-JIS at the same offset:' % len(ok))
    for lit, fi, at, text in ok[:60]:
        print('    { "%-30s %d, 0x%04X, 0 },  /* %s */'
              % (lit + '",', fi, at, text))
    bad = [f for f in found if not f[3]]
    if bad:
        print()
        print('%d located but NOT valid Shift-JIS there (offset is not parallel):'
              % len(bad))
        for lit, fi, at, _t in bad[:20]:
            print('    %-32s file %d, 0x%04X' % (lit, fi, at))
    if missing:
        print()
        print('not present as a literal in the English menu files (%d):' % len(missing))
        print('    ' + ', '.join(missing[:30]))


if __name__ == '__main__':
    main()
