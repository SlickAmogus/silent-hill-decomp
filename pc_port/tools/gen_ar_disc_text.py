#!/usr/bin/env python3
"""
gen_ar_disc_text.py -- where the Arabic fan patches keep each UI string.

The port draws its menus from COMPILED English literals, so an Arabic disc's
translated menus are only reachable if the port knows where on the disc each
literal's translation sits. Both patches edit in place:

  * a string that fit is overwritten at the same offset              (DIRECT)
  * one that did not is written elsewhere, and either the pointer-table
    slot that names it                                               (PTR)
  * or the lui/addiu pair in code that loads it is retargeted        (CODE)

This tool compares each retail file with its patched copy and records, for
every translated string, the retail ENGLISH text and the location to read on
the user's disc. No Arabic is written out: the port reads it from the mounted
disc at run time, so the translators' text is never redistributed.

Files: 1ST/BODYPROG.BIN (LCG-obfuscated, decrypted first), VIN/OPTION.BIN,
VIN/SAVELOAD.BIN.

Usage:
  python gen_ar_disc_text.py --usa RETAIL_USA.bin MANSOUR.bin \\
                             --pal RETAIL_PAL.bin AHMED.bin \\
                             -o ../src/lang_ar_disc.inc
"""
import argparse
import os
import re
import struct

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
SECT, DOFF = 2352, 24

FILES = [
    # (enum suffix in lang_ar.c, path, encrypted, PSX link base by region)
    ('AR_FILE_BODYPROG', '1ST/BODYPROG.BIN', True,  {'USA': 0x80024B60, 'EUR': 0x80025690}),
    ('AR_FILE_OPTION',   'VIN/OPTION.BIN',   False, {'USA': 0x801E2600, 'EUR': 0x801E2600}),
    ('AR_FILE_SAVELOAD', 'VIN/SAVELOAD.BIN', False, {'USA': 0x801E2600, 'EUR': 0x801E2600}),
]


def load_table(region):
    path = os.path.join(REPO, 'src', 'main', 'filetable.c.%s.inc' % region)
    t = {}
    for line in open(path, encoding='utf-8', errors='replace'):
        m = re.match(r'/\*\s*\d+\s*\*/\s*\{\s*0x([0-9a-fA-F]+),\s*(\d+),.*//\s*(\S+)', line)
        if m:
            t[m.group(3)] = (int(m.group(1), 16), int(m.group(2)) * 256)
    return t


def read_file(bin_path, table, name):
    lba, size = table[name]
    out = bytearray()
    with open(bin_path, 'rb') as f:
        for i in range((size + 2047) // 2048):
            f.seek((lba + i) * SECT + DOFF)
            out += f.read(2048)
    return bytes(out[:size])


def decrypt(data):
    n = len(data) // 4
    w = list(struct.unpack('<%dI' % n, data[:n * 4]))
    seed = 0
    for i in range(n):
        seed = ((seed + 0x01309125) * 0x03A452F7) & 0xFFFFFFFF
        w[i] ^= seed
    return struct.pack('<%dI' % n, *w)


def text_byte(c):
    # English keys only: the port's compiled literals are ASCII, and a byte
    # >= 0x80 here is a numeric table that happens to end in a zero.
    return c in (0x09, 0x0A) or 0x01 <= c <= 0x08 or 0x20 <= c <= 0x7E


def english_at(d, o):
    """A plausible English UI literal starting at o, else None."""
    if o < 0 or o >= len(d):
        return None
    e = d.find(b'\0', o)
    if e < 0 or e == o or e - o > 240:
        return None
    s = d[o:e]
    if not all(text_byte(c) for c in s):
        return None
    if len(s) < 2 or sum(1 for c in s if 0x41 <= c <= 0x5A or 0x61 <= c <= 0x7A) < 1:
        return None
    return s


def cstr(d, o):
    if o < 0 or o >= len(d):
        return None
    e = d.find(b'\0', o)
    return d[o:e] if e >= 0 else None


def refs(d, base):
    """{target offset: [(kind, loc, aux)]} for pointer words and lui/addiu pairs."""
    n = len(d) // 4
    w = struct.unpack('<%dI' % n, d[:n * 4])
    out = {}
    for i in range(n):
        t = w[i] - base
        if 0 <= t < len(d):
            out.setdefault(t, []).append(('AR_LOC_PTR', i * 4, 0))
    for i in range(n):
        x = w[i]
        if (x >> 26) != 0x0F:
            continue
        rt = (x >> 16) & 31
        hi = (x & 0xFFFF) << 16
        for j in range(i + 1, min(n, i + 12)):
            y = w[j]
            op = y >> 26
            if op == 0x09 and ((y >> 21) & 31) == rt:
                imm = y & 0xFFFF
                imm -= 0x10000 if imm & 0x8000 else 0
                t = ((hi + imm) & 0xFFFFFFFF) - base
                if 0 <= t < len(d):
                    out.setdefault(t, []).append(('AR_LOC_CODE', j * 4, i * 4))
                break
            if ((y >> 16) & 31) == rt and op not in (0x2B, 0x28, 0x29):
                break  # rt overwritten before any addiu
    return out


def resolve(d, base, kind, loc, aux):
    n = len(d) // 4
    if loc + 4 > len(d) or aux + 4 > len(d):
        return None
    if kind == 'AR_LOC_PTR':
        return struct.unpack_from('<I', d, loc)[0] - base
    lo = struct.unpack_from('<I', d, loc)[0] & 0xFFFF
    hi = (struct.unpack_from('<I', d, aux)[0] & 0xFFFF) << 16
    lo -= 0x10000 if lo & 0x8000 else 0
    return ((hi + lo) & 0xFFFFFFFF) - base


def entries_for(region, disc_enum, retail_bin, ar_bin):
    table = load_table(region)
    rows = []
    for fenum, name, enc, bases in FILES:
        base = bases[region]
        a = read_file(retail_bin, table, name)
        b = read_file(ar_bin, table, name)
        if enc:
            a, b = decrypt(a), decrypt(b)
        ra = refs(a, base)

        starts = set()
        o = 0
        while o < len(a):
            if (o == 0 or a[o - 1] == 0) and english_at(a, o):
                starts.add(o)
                o = a.find(b'\0', o) + 1
            else:
                o += 1
        starts |= {t for t in ra if english_at(a, t)}

        for t in sorted(starts):
            eng = english_at(a, t)
            moved = None
            for kind, loc, aux in ra.get(t, []):
                nt = resolve(b, base, kind, loc, aux)
                if nt is not None and nt != t:
                    ar = cstr(b, nt)
                    if ar:
                        moved = (kind, loc, aux)
                        break
            if moved:
                rows.append((disc_enum, fenum, moved[0], moved[1], moved[2], eng))
            elif cstr(b, t) not in (None, b'', eng):
                rows.append((disc_enum, fenum, 'AR_LOC_DIRECT', t, 0, eng))
    return rows


def c_escape(s):
    out = []
    for c in s:
        if c == 0x22:
            out.append('\\"')
        elif c == 0x5C:
            out.append('\\\\')
        elif 0x20 <= c <= 0x7E:
            out.append(chr(c))
        else:
            out.append('\\x%02X""' % c)
    return '"' + ''.join(out) + '"'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--usa', nargs=2, metavar=('RETAIL', 'ARABIC'))
    ap.add_argument('--pal', nargs=2, metavar=('RETAIL', 'ARABIC'))
    ap.add_argument('-o', '--out', required=True)
    args = ap.parse_args()

    rows = []
    if args.usa:
        rows += entries_for('USA', 'AR_DISC_USA_MANSOUR', *args.usa)
    if args.pal:
        rows += entries_for('EUR', 'AR_DISC_PAL_AHMED', *args.pal)

    with open(args.out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('/* Generated by pc_port/tools/gen_ar_disc_text.py from the retail and\n'
                ' * Arabic discs. English keys and disc locations only; the Arabic is read\n'
                ' * from the mounted disc at run time (lang_ar.c). Do not edit. */\n')
        for disc, fenum, kind, loc, aux, eng in rows:
            f.write('{ %s, %s, %s, 0x%06X, 0x%06X, %s },\n'
                    % (disc, fenum, kind, loc, aux, c_escape(eng)))
    counts = {}
    for r in rows:
        counts[(r[0], r[1], r[2])] = counts.get((r[0], r[1], r[2]), 0) + 1
    for k in sorted(counts):
        print('%-22s %-18s %-14s %d' % (k + (counts[k],)))
    print('%d entries -> %s' % (len(rows), args.out))


if __name__ == '__main__':
    main()
