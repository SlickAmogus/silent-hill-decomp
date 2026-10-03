#!/usr/bin/env python3
"""Build the shipped Russian master file from the disc text plus our own.

SilentHill_RU_for_review.txt is the Team Raccoon script as make_ru_template.py
read it off the disc. ru_supplement.py holds the Russian for everything the
disc could not supply (the PC port's own menus). This merges the two into
SilentHill_RU_translation.txt, the file ru.lang is generated from:

    python make_ru_lang.py
    python import_translation.py --in SilentHill_RU_translation.txt \\
           --code ru --name Russian --menu Русский --font cyrillic

Usage:
  python make_ru_lang.py [--check]     (--check only reports, writes nothing)
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import extract_text as tmpl          # noqa: E402
import import_translation as imp     # noqa: E402
import ru_supplement as sup          # noqa: E402

BASE = os.path.join(HERE, 'SilentHill_RU_for_review.txt')
OUT = os.path.join(HERE, 'SilentHill_RU_translation.txt')

LEGEND_HEAD = """\
================================================================================
 SILENT HILL (1999)  -  RUSSIAN
================================================================================
 The shipped Russian text, and the file gamedata/lang/ru.lang is built from.

 Story, items and the retail menus are the Team Raccoon / ViT Co / Metallist
 fan translation, read off that disc (make_ru_template.py). Subtitles for
 voiced lines come from the same team's text-only release, since the Team
 Raccoon disc is a dub and blanks them. The PC port's own menus -- PC Options,
 the F10 Quick Options menu, the Controls panel and the confirm boxes -- are
 ours (ru_supplement.py); no 1999 disc had them.

 Regenerate with make_ru_lang.py; edit ru_supplement.py, not this file, for
 anything in the PC-port sections.
"""


def main():
    check = '--check' in sys.argv
    src = imp.parse_translation(BASE)
    tr = {k: v for k, (v, _ln) in src.items() if v and not v.startswith('(no text')}
    keys = {r[1] for r in tmpl.records}

    # MENU.<literal> keys: the supplement is keyed by the literal itself.
    added = unknown = 0
    for lit, ru in sup.MENU.items():
        k = 'MENU.' + lit.replace('=', '-')
        if k not in keys:
            print('  ! MENU key not in the template: %s' % lit)
            unknown += 1
            continue
        tr[k] = ru
        added += 1
    for en, ru in sup.QUICK.items():
        k = 'QUICK.' + en.replace(' ', '_').replace('=', '-')
        if k not in keys:
            print('  ! QUICK key not in the template: %s' % en)
            unknown += 1
            continue
        # A placeholder the game fills in has to survive verbatim.
        if sorted(re.findall(r'\{[a-z]+\}', en)) != sorted(re.findall(r'\{[a-z]+\}', ru)):
            print('  ! placeholder mismatch: %s -> %s' % (en, ru))
            unknown += 1
            continue
        tr[k] = ru
        added += 1

    for k, ru in getattr(sup, 'OTHER', {}).items():
        if k not in keys:
            print('  ! key not in the template: %s' % k)
            unknown += 1
            continue
        tr[k] = ru
        added += 1

    blank = [r[1] for r in tmpl.records if r[1] not in tr]
    print('from the disc : %d' % len(src))
    print('ours          : %d added, %d rejected' % (added, unknown))
    print('total filled  : %d of %d   (%d left, mostly timing-only lines)'
          % (len(tr), len(tmpl.records), len(blank)))

    # Latin letters in a game-font row would draw as Cyrillic noise: on this
    # atlas the Latin capitals ARE the Cyrillic ones.
    bad = [k for k in tr if k.startswith(('MENU.', 'ITEM_', 'COMMON.', 'MAP'))
           and re.search(r'[A-Za-z]', re.sub(r'~[A-Z][0-9]?(\([0-9.]*\))?', '', tr[k]))]
    if bad:
        print('\nWARNING: %d game-font entries still contain Latin letters:' % len(bad))
        for k in bad[:12]:
            print('   %-22s %s' % (k, tr[k][:60]))

    if check:
        return 0
    tmpl.write_template(OUT, tr_label='RU', tr_text=tr,
                        legend=LEGEND_HEAD + '=' * 80 + '\n\n')
    print('\nWROTE:', OUT)
    return 0


if __name__ == '__main__':
    sys.exit(main())
