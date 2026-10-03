#!/usr/bin/env python3
"""Lift the Japanese story text off an NTSC-J disc into a language pack.

Only the story needs extracting. The Japanese item names and descriptions are
already compiled into the port (lang_jpn_items.inc), and so are its menus
(lang_jpn_menu.inc reads them off the disc, lang_jpn_pcopt/ui.inc are written),
so the pack carries the one thing that is neither: the per-map message tables.

The pack is written in the engine's own bytes rather than UTF-8 -- the values
are Shift-JIS, which is what the drawer wants and what the embedded kanji font
is indexed by -- so it declares `!font=sjis` and the loader passes the bytes
through untouched. Keys stay ASCII, and no SJIS trail byte can be a newline or
'=', so the line format is unambiguous.

Indices are US indices, because that is what the game asks a pack for. 14 maps
number their messages differently on the two discs; lang_jpn_msgmap.inc already
holds that US->JP mapping and is parsed here rather than restated.

Usage:
    python make_jp_pack.py "<gamedata>/Silent Hill (Japan).bin"
"""
import io
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gen_zh_pack as disc  # noqa: E402

REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
OUT = os.path.join(HERE, '..', 'assets', 'gamedata', 'lang', 'ja.lang')

JPN_OVL_BASE = 0x800CBBD0
MSG_COUNT_MAX = 176
COMMON_MSG_COUNT = 15

FT = io.open(os.path.join(REPO, 'src', 'main', 'filetable.c.JAP1.inc'),
             encoding='utf-8', errors='surrogateescape').read()


def ft_find(tag):
    m = re.search(r'\{\s*(0x[0-9a-fA-F]+),\s*(\d+),.*?//\s*%s\b' % re.escape(tag), FT)
    return (int(m.group(1), 16), int(m.group(2))) if m else None


def map_order():
    text = io.open(os.path.join(REPO, 'include', 'bodyprog', 'map', 'map.h'),
                   encoding='utf-8', errors='surrogateescape').read()
    pairs = re.findall(r'MapIdx_(MAP\w+?_S\d+)\s*=\s*(\d+)', text)
    out = [None] * (max(int(v) for _, v in pairs) + 1)
    for name, val in pairs:
        out[int(val)] = name.upper()
    return out


def msg_maps():
    """US index -> JP index, per map, from the port's own table."""
    text = io.open(os.path.join(REPO, 'pc_port', 'src', 'lang_jpn_msgmap.inc'),
                   encoding='utf-8', errors='surrogateescape').read()
    out = {}
    for m in re.finditer(r'JPNMSG_(MAP\w+?)\[\d+\]\s*=\s*\{(.*?)\};', text, re.S):
        out[m.group(1)] = [int(x) for x in re.findall(r'-?\d+', m.group(2))]
    return out


def overlay_messages(ovl, size):
    tp = struct.unpack_from('<I', ovl, 0x34)[0]
    if tp < JPN_OVL_BASE or tp - JPN_OVL_BASE >= size:
        return None
    off, out = tp - JPN_OVL_BASE, []
    while off + (len(out) + 1) * 4 <= size and len(out) < MSG_COUNT_MAX:
        p = struct.unpack_from('<I', ovl, off + len(out) * 4)[0]
        if p <= JPN_OVL_BASE or p - JPN_OVL_BASE >= size:
            break
        s = p - JPN_OVL_BASE
        e = ovl.find(b'\0', s)
        if e < 0:
            break
        out.append(ovl[s:e])
    return out


def esc(b):
    """One entry per line. A SJIS trail byte can be 0x5C, so escape the
    backslash too or the loader's unescape would eat the pair."""
    return b.replace(b'\\', b'\\\\').replace(b'\n', b'\\n').replace(b'\t', b'\\t')


MENU_INC = os.path.join(REPO, 'pc_port', 'src', 'lang_jpn_menu.inc')


def menu_table():
    """(us, file, off, len) per row, from the same table the loader reads."""
    rows = []
    for m in re.finditer(r'\{\s*"([^"]*)"\s*,\s*(\d+)\s*,\s*'
                         r'(0x[0-9a-fA-F]+)\s*,\s*(\d+)\s*\}',
                         io.open(MENU_INC, encoding='utf-8').read()):
        rows.append((m.group(1), int(m.group(2)),
                     int(m.group(3), 16), int(m.group(4))))
    return rows


def is_lead(b):
    return 0x81 <= b <= 0x9F or 0xE0 <= b <= 0xEF


def copy_entry(src, off, length):
    """Mirror of CopyEntry in lang_jpn.c: drop the fixed field's trailing
    padding and fold the overlay's ~Cn colour markup to the one byte the
    port's string drawer wants. The walk has to be SJIS-pair-aware -- a trail
    byte may legally be 0x7E or 0x81, so scanning one byte at a time would let
    half a kanji pass for markup or for padding."""
    end = min(off + length, len(src)) if length else src.index(bytes([0]), off)
    out = bytearray()
    keep = 0
    i = off
    while i < end:
        if is_lead(src[i]) and i + 1 < end:
            pad = src[i] == 0x81 and src[i + 1] == 0x40
            out += src[i:i + 2]
            i += 2
            if not pad:
                keep = len(out)
            continue
        if src[i] == 0x7E and i + 2 < end and src[i + 1:i + 2] == b'C'                 and 0x30 <= src[i + 2] <= 0x37:
            out.append(src[i + 2] - 0x30)
            i += 3
            keep = len(out)
            continue
        out.append(src[i])
        i += 1
        if src[i - 1] != 0x20:
            keep = len(out)
    return bytes(out[:keep])


def menu_entries(path):
    """The Japanese menu strings, keyed by the US literal the port draws.

    These are the one piece a pack could not supply before: the loader reads
    them out of the Japanese disc's own VIN overlays, so on any other disc they
    stayed English however the language was set. Extracting them is what lets
    Japanese menus work on a PAL or US disc."""
    ovl = []
    for name in ('VIN/OPTION.BIN', 'VIN/SAVELOAD.BIN'):
        ent = ft_find(name)
        data = disc.read_disc_file(path, ent[0], ent[1] * 256)
        if data is None:
            sys.exit('error: could not read %s' % name)
        ovl.append(data)

    out = []
    for us, fileIdx, off, length in menu_table():
        v = copy_entry(ovl[fileIdx], off, length)
        if not v:
            continue
        # The loader builds its lookup key the same way (lang_pack.c MenuKey).
        out.append(('MENU.' + us.replace(' ', '_').replace('=', '-'), v))
    return out


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    path = sys.argv[1]
    order = map_order()
    maps = msg_maps()

    entries = []
    for idx, mapname in enumerate(order):
        if mapname is None:
            continue
        ent = ft_find('VIN/%s.BIN' % mapname)
        if ent is None or ent[1] == 0:
            continue
        ovl = disc.read_disc_file(path, ent[0], ent[1] * 256)
        if ovl is None or len(ovl) < 0x40:
            continue
        msgs = overlay_messages(ovl, ent[1] * 256)
        if not msgs:
            continue

        idxmap = maps.get(mapname)
        count = len(idxmap) if idxmap else len(msgs)
        for us in range(count):
            jp = idxmap[us] if idxmap else us
            if jp < 0 or jp >= len(msgs) or not msgs[jp]:
                continue           # no JP counterpart: the English stands
            key = ('COMMON.%d' % us) if us < COMMON_MSG_COUNT else ('%s.%d' % (mapname, us))
            entries.append((key, msgs[jp]))

    menus = menu_entries(path)
    entries.extend(menus)

    seen = set()
    with io.open(OUT, 'wb') as f:
        f.write(b'# Silent Hill PC -- language pack (generated by make_jp_pack.py)\n')
        f.write(b'# Japanese story text, lifted from the NTSC-J disc so any disc can\n')
        f.write(b'# offer it. Values are Shift-JIS, the engine\'s own bytes.\n')
        f.write(b'!font=sjis\n!code=ja\n!name=Japanese\n!menu=Japanese\n')
        n = 0
        for key, v in entries:
            if key in seen:
                continue
            seen.add(key)
            f.write(key.encode('ascii') + b'=' + esc(v) + b'\n')
            n += 1
    print('%d entries (%d menu), %s KB'
          % (n, len(menus), '{:,}'.format(os.path.getsize(OUT) // 1024)))
    print('WROTE:', os.path.normpath(OUT))


if __name__ == '__main__':
    main()
