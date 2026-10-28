#!/usr/bin/env python3
"""coff_rename.py - prefix every defined external symbol of a map overlay's
objects, in place. The XDK build's answer to `xenon-objcopy --redefine-syms`.

    python3 coff_rename.py <prefix> <obj>...

Pass 1 collects the external symbols DEFINED in any of the objects. Pass 2
renames every external symbol in those objects whose name is in that set --
definitions and references alike, so intra-map calls keep binding to the
map's own copy, while references to shared game code (undefined everywhere
in the map) are untouched. That is what lets 43 maps that each #include the
same chara/particle sources link into one image.

PPC COFF (machine 0x1F2) symbols carry no underscore. Names longer than 8
bytes live in the string table; a renamed symbol is always longer than 8, so
its name is appended there and the entry switched to the long form.
"""
import struct
import sys

SYM = 18
CLS_EXTERNAL = 2
CLS_WEAK_EXTERNAL = 105


def read_syms(d):
    machine, nsec, _, symoff, nsym, optsz, chars = struct.unpack('<HHIIIHH', d[:20])
    if machine != 0x1F2:
        raise SystemExit('not a PPCBE COFF object (machine %04x)' % machine)
    stroff = symoff + nsym * SYM
    strsize = struct.unpack('<I', d[stroff:stroff + 4])[0]
    strtab = bytearray(d[stroff:stroff + strsize])
    entries = []
    i = 0
    while i < nsym:
        e = d[symoff + i * SYM: symoff + (i + 1) * SYM]
        name8, value, sec, typ, cls, naux = struct.unpack('<8sIhHBB', e)
        if name8[:4] == b'\0\0\0\0':
            off = struct.unpack('<I', name8[4:])[0]
            name = bytes(strtab[off:strtab.index(b'\0', off)])
        else:
            name = name8.rstrip(b'\0')
        entries.append((i, name, sec, cls, naux))
        i += 1 + naux
    return symoff, nsym, stroff, strtab, entries


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    prefix = sys.argv[1].encode()
    paths = sys.argv[2:]
    blobs = {p: open(p, 'rb').read() for p in paths}

    defined = set()
    for p, d in blobs.items():
        for _, name, sec, cls, _ in read_syms(d)[4]:
            if cls in (CLS_EXTERNAL, CLS_WEAK_EXTERNAL) and sec > 0:
                defined.add(name)

    renamed = 0
    for p, d in blobs.items():
        symoff, nsym, stroff, strtab, entries = read_syms(d)
        d = bytearray(d)
        changed = False
        for idx, name, sec, cls, naux in entries:
            if cls not in (CLS_EXTERNAL, CLS_WEAK_EXTERNAL) or name not in defined:
                continue
            new = prefix + name
            off = len(strtab)
            strtab += new + b'\0'
            pos = symoff + idx * SYM
            d[pos:pos + 8] = b'\0\0\0\0' + struct.pack('<I', off)
            changed = True
            renamed += 1
        if changed:
            struct.pack_into('<I', strtab, 0, len(strtab))
            d = d[:stroff] + bytes(strtab)
            open(p, 'wb').write(bytes(d))
    print('%s: %d defined symbols, %d entries renamed in %d objects' % (prefix.decode(), len(defined), renamed, len(paths)))


if __name__ == '__main__':
    main()
