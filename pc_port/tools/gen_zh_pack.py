#!/usr/bin/env python3
"""Build gamedata/lang/zh.pack — the Chinese text for NTSC-J, lifted off a
Chinese-patched disc so an UNPATCHED Japanese disc can show it.

Why a pack at all
-----------------
The Chinese translation (goro / 十三月, 2006) has two halves. The GLYPHS ship as
a patched PSX BIOS, because the disc has nowhere to store Chinese characters --
the port already stands in for that with its own embedded font
(pc_port/src/kanji_font_cn.inc). The WORDS ship as a PPF applied to the disc
image, and that half is what this extracts.

The translation never writes Chinese: it writes ordinary JIS kuten codes whose
glyphs its BIOS redefines. So the bytes here are the disc's own bytes, engine
control codes (~N, ~E, ~J0(2.0), tabs) and all -- nothing is transcoded, and the
port draws them with the CN glyph set exactly as the disc intended.

Redistribution of the extracted text is with goro's permission.

Sources on the disc
-------------------
  item names/descriptions  1ST/BODYPROG.BIN, XOR-obfuscated (Fs_DecryptOverlay),
                           pointer arrays at 0x800B0044 / 0x800B0350, link base
                           0x80024B60
  map messages             VIN/MAP*.BIN, pointer table at overlay offset 0x34,
                           link base 0x800CBBD0 (JPN_OVL_BASE)
  menu strings             VIN/OPTION.BIN and VIN/SAVELOAD.BIN at the fixed
                           offsets pc_port/src/lang_jpn_menu.inc already lists

The map tables are emitted in the disc's own (JP) order. The port's existing
US->JP index map (lang_jpn_msgmap.inc) is applied at install time, so this file
must not reorder anything.

Usage:
    python gen_zh_pack.py "Silent Hill (Japan) (patched).bin" [-o zh.pack]
"""

import argparse
import io
import os
import re
import struct
import sys

RAW_SECTOR, SECTOR_DATA_OFF, SECTOR_DATA_LEN = 2352, 24, 2048

JPN_OVL_BASE  = 0x800CBBD0
JPN_BODY_VRAM = 0x80024B60
JPN_ITEM_NAME = 0x800B0044
JPN_ITEM_DESC = 0x800B0350
ITEM_COUNT    = 195
MSG_COUNT_MAX = 176

MAGIC   = b'SHZH'
VERSION = 2
NO_STR  = 0xFFFFFFFF

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..'))


def read_disc_file(path, lba, size):
    n = (size + SECTOR_DATA_LEN - 1) // SECTOR_DATA_LEN
    buf = bytearray()
    with open(path, 'rb') as f:
        for i in range(n):
            f.seek((lba + i) * RAW_SECTOR + SECTOR_DATA_OFF)
            d = f.read(SECTOR_DATA_LEN)
            if len(d) != SECTOR_DATA_LEN:
                return None
            buf += d
    return bytes(buf[:size])


def fs_decrypt_overlay(data):
    """The game's own Fs_DecryptOverlay: one continuous LCG over the file."""
    n = len(data) // 4
    words = struct.unpack('<%dI' % n, data[:n * 4])
    out, seed = [], 0
    for w in words:
        seed = ((seed + 0x01309125) * 0x03A452F7) & 0xFFFFFFFF
        out.append(w ^ seed)
    return struct.pack('<%dI' % n, *out)


def load_filetable(inc_name):
    """VIN map overlays + BODYPROG from a filetable .inc."""
    path = os.path.join(REPO, 'src', 'main', inc_name)
    text = io.open(path, encoding='utf-8', errors='surrogateescape').read()
    maps, body = {}, None
    for m in re.finditer(r'\{\s*(0x[0-9a-fA-F]+),\s*(\d+),.*?//\s*VIN/(MAP\d+_S\d+)\.BIN', text):
        maps[m.group(3).lower()] = (int(m.group(1), 16), int(m.group(2)))
    m = re.search(r'\{\s*(0x[0-9a-fA-F]+),\s*(\d+),.*?//\s*1ST/BODYPROG\.BIN', text)
    if m:
        body = (int(m.group(1), 16), int(m.group(2)))
    return maps, body


def map_order():
    """mapIdx -> 'mapN_sXX', indexed by the e_MapIdx enum itself.

    Taken from the enum rather than any table, because the pack is indexed by
    mapIdx at runtime: read it from something that could be reordered and every
    map would silently show another map's text. MAPT_S00/MAPX_S00 (43/44) are
    unused test maps with no VIN file and come out as gaps.
    """
    path = os.path.join(REPO, 'include', 'bodyprog', 'map', 'map.h')
    text = io.open(path, encoding='utf-8', errors='surrogateescape').read()
    pairs = re.findall(r'MapIdx_(MAP\w+?_S\d+)\s*=\s*(\d+)', text)
    if not pairs:
        sys.exit('error: could not read e_MapIdx from map.h')
    top = max(int(v) for _, v in pairs)
    out = [None] * (top + 1)
    for name, val in pairs:
        out[int(val)] = name.lower()
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
        e = ovl.find(b'\x00', s)
        if e < 0:
            break
        out.append(ovl[s:e])
    return out


def item_arrays(body):
    size = len(body)
    names, descs = [], []
    for arr, addr in ((names, JPN_ITEM_NAME), (descs, JPN_ITEM_DESC)):
        for i in range(ITEM_COUNT):
            off = (addr - JPN_BODY_VRAM) + i * 4
            if off + 4 > size:
                arr.append(None)
                continue
            p = struct.unpack_from('<I', body, off)[0]
            if p < JPN_BODY_VRAM or p - JPN_BODY_VRAM >= size:
                arr.append(None)
                continue
            s = p - JPN_BODY_VRAM
            e = body.find(b'\x00', s)
            arr.append(body[s:e] if e >= 0 else None)
    return names, descs


def menu_entries():
    """(file, off, len) per menu string, read from the port's own table.

    lang_jpn_menu.inc is where those offsets are already maintained for reading
    Japanese off a retail disc; the Chinese patch edits the same overlays in
    place, so the same offsets hold the Chinese. Parsing that file rather than
    restating the offsets means the two can never drift apart.
    """
    path = os.path.join(REPO, 'pc_port', 'src', 'lang_jpn_menu.inc')
    text = io.open(path, encoding='utf-8', errors='surrogateescape').read()
    out = []
    # A key may be SEVERAL adjacent literals: the Walk/Run row carries the
    # menu's kerning escapes and C splits it across quotes. Matching only one
    # literal dropped that row and shifted every index after it by one, which
    # the loader cannot catch -- it reads the pack BY INDEX, so Chinese would
    # have drawn the wrong string for every entry past it.
    for m in re.finditer(r'\{\s*(?:"(?:[^"\\]|\\.)*"\s*)+,\s*(\d+)\s*,\s*'
                         r'(0x[0-9A-Fa-f]+|\d+)\s*,\s*(\d+)\s*\}', text):
        out.append((int(m.group(1)), int(m.group(2), 0), int(m.group(3))))
    rows = len(re.findall(r'^\s*\{', text, re.M))
    if rows != len(out):
        sys.exit('error: parsed %d of %d rows in lang_jpn_menu.inc -- the pack '
                 'would be index-shifted against the game' % (len(out), rows))
    return out


def menu_slice(ovl, off, length):
    """The raw bytes the port's CopyEntry would read, so it can run unchanged."""
    if ovl is None or off >= len(ovl):
        return None
    if length:
        end = off + length
        if end > len(ovl):
            return None
        return ovl[off:end]
    end = ovl.find(b'\x00', off)
    if end < 0:
        return None
    return ovl[off:end]


class Blob(object):
    def __init__(self):
        self.buf = bytearray()
        self.seen = {}

    def add(self, s):
        if s is None:
            return NO_STR
        if s in self.seen:
            return self.seen[s]
        off = len(self.buf)
        self.buf += s + b'\x00'
        self.seen[s] = off
        return off


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('disc', help='Chinese-patched NTSC-J disc image (raw 2352-byte sectors)')
    ap.add_argument('-o', '--out',
                    default=os.path.join(REPO, 'pc_port', 'assets', 'gamedata', 'lang', 'zh.pack'))
    ap.add_argument('--filetable', default='filetable.c.JAP1.inc')
    args = ap.parse_args()

    maps_ft, body_ft = load_filetable(args.filetable)
    if body_ft is None:
        sys.exit('error: no BODYPROG entry in %s' % args.filetable)

    names = descs = None
    disc_path = args.disc
    raw = read_disc_file(disc_path, body_ft[0], body_ft[1] * 256)
    if raw is None:
        sys.exit('error: could not read BODYPROG (is this a raw 2352-byte image?)')
    names, descs = item_arrays(fs_decrypt_overlay(raw))
    print('items      : %d names, %d descriptions'
          % (sum(1 for x in names if x), sum(1 for x in descs if x)))

    order = map_order()
    blob = Blob()
    item_name_off = [blob.add(s) for s in names]
    item_desc_off = [blob.add(s) for s in descs]

    map_msgs = []
    for low in order:
        ent = maps_ft.get(low) if low else None
        if ent is None:
            map_msgs.append(None)
            continue
        ovl = read_disc_file(args.disc, ent[0], ent[1] * 256)
        if ovl is None:
            map_msgs.append(None)
            continue
        msgs = overlay_messages(ovl, ent[1] * 256)
        map_msgs.append(msgs)

    menu_ft = {}
    text_ft = io.open(os.path.join(REPO, 'src', 'main', args.filetable),
                      encoding='utf-8', errors='surrogateescape').read()
    for key, tag in ((0, 'OPTION'), (1, 'SAVELOAD')):
        mm = re.search(r'\{\s*(0x[0-9a-fA-F]+),\s*(\d+),.*?//\s*VIN/%s\.BIN' % tag, text_ft)
        if mm:
            menu_ft[key] = (int(mm.group(1), 16), int(mm.group(2)))

    menu_ovl = {}
    for key, ent in menu_ft.items():
        menu_ovl[key] = read_disc_file(disc_path, ent[0], ent[1] * 256)

    menu = []
    for fileIdx, off, length in menu_entries():
        menu.append(menu_slice(menu_ovl.get(fileIdx), off, length))
    print('menu       : %d of %d strings read from OPTION/SAVELOAD'
          % (sum(1 for m in menu if m), len(menu)))

    got = sum(1 for m in map_msgs if m)
    total = sum(len(m) for m in map_msgs if m)
    print('maps       : %d of %d carry a message table (%d messages)'
          % (got, len(order), total))

    map_tables = []
    for msgs in map_msgs:
        if not msgs:
            map_tables.append(None)
        else:
            map_tables.append([blob.add(s) for s in msgs])

    # ---- serialise -------------------------------------------------------
    map_count  = len(order)
    menu_count = len(menu)
    head = struct.calcsize('<4sIIIIII')
    dir_size = ITEM_COUNT * 4 * 2 + map_count * 4 + menu_count * 8
    cursor = head + dir_size

    map_dir, tables = [], bytearray()
    for t in map_tables:
        if t is None:
            map_dir.append(0)
            continue
        map_dir.append(cursor + len(tables))
        tables += struct.pack('<I', len(t))
        tables += struct.pack('<%dI' % len(t), *t)

    # Menu slices carry an explicit length: they are fixed-width fields, not
    # NUL-terminated strings, and CopyEntry needs the same span it would have
    # read out of the overlay.
    menu_dir = []
    for sl in menu:
        if sl is None:
            menu_dir.append((NO_STR, 0))
        else:
            menu_dir.append((blob.add(sl), len(sl)))

    blob_off = cursor + len(tables)
    out = bytearray()
    out += struct.pack('<4sIIIIII', MAGIC, VERSION, ITEM_COUNT, map_count,
                       blob_off, len(blob.buf), menu_count)
    out += struct.pack('<%dI' % ITEM_COUNT, *item_name_off)
    out += struct.pack('<%dI' % ITEM_COUNT, *item_desc_off)
    out += struct.pack('<%dI' % map_count, *map_dir)
    for off, ln in menu_dir:
        out += struct.pack('<II', off, ln)
    out += tables
    out += blob.buf

    d = os.path.dirname(args.out)
    if d and not os.path.isdir(d):
        os.makedirs(d)
    with open(args.out, 'wb') as f:
        f.write(out)
    print('wrote %s (%d bytes; %d in strings)' % (args.out, len(out), len(blob.buf)))


if __name__ == '__main__':
    main()
