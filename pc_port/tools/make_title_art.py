#!/usr/bin/env python3
"""Extract the title art each disc is missing, so every style works everywhere.

There are three title screens and no disc carries all three:

  us        TIM/TITLE_E.TIM, a full 320x480 picture (background + logo baked).
            On the USA and Japanese discs; the PAL disc's TITLE_E is something
            else entirely.
  pal       a 4bpp 320x96 logo block the game composes a title from (black,
            logo, fog). Only on the PAL disc, where it is called TITLE_E.
  japanese  TIM/TITLE.TIM, black with the stylized logo. Byte-identical on all
            three discs, so it never needs shipping.

So the port ships the two a given disc lacks, as plain TIMs under
gamedata/title/. They are read only when the chosen style is not the one the
mounted disc provides.

Usage:
    python make_title_art.py "<gamedata>/Silent Hill (USA).bin" \\
                             "<gamedata>/Silent Hill (Europe) (En,Fr,De,Es,It).bin"
"""
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gen_zh_pack as disc  # noqa: E402

REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
OUT_DIR = os.path.join(HERE, '..', 'assets', 'gamedata', 'title')


def ft_find(table, tag):
    ft = io.open(os.path.join(REPO, 'src', 'main', table),
                 encoding='utf-8', errors='surrogateescape').read()
    m = re.search(r'\{\s*(0x[0-9a-fA-F]+),\s*(\d+),.*?//\s*%s\b' % re.escape(tag), ft)
    return (int(m.group(1), 16), int(m.group(2))) if m else None


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    usa, eur = sys.argv[1], sys.argv[2]
    os.makedirs(OUT_DIR, exist_ok=True)

    for name, table, path, out in (
            ('TIM/TITLE_E.TIM', 'filetable.c.USA.inc', usa, 'us.tim'),
            ('TIM/TITLE_E.TIM', 'filetable.c.EUR.inc', eur, 'pal.tim')):
        ent = ft_find(table, name)
        data = disc.read_disc_file(path, ent[0], ent[1] * 256)
        if data is None:
            sys.exit('error: could not read %s' % name)
        dst = os.path.join(OUT_DIR, out)
        io.open(dst, 'wb').write(data)
        print('%-8s %s bytes  <- %s' % (out, '{:,}'.format(len(data)), os.path.basename(path)))
    print('WROTE:', os.path.normpath(OUT_DIR))


if __name__ == '__main__':
    main()
