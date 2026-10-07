#!/usr/bin/env python3
"""Pre-fill the translation template from an existing language pack.

For a language that already ships (gamedata/lang/<code>.lang): writes
SilentHill_<CODE>_for_update.txt, the current template with every entry the
pack already has filled in, so the translator only has to find the empty
lines -- text added to the game since their last pass (Quick Options, the
Controls panel, new PC Options rows) -- and can re-check the rest.

The file imports back with import_translation.py like any other.

Usage:
  python make_update_template.py pl
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import extract_text as tmpl   # noqa: E402


def load_pack(path):
    out = {}
    for ln in open(path, encoding='utf-8').read().split('\n'):
        if not ln or ln.startswith('#') or ln.startswith('!') or '=' not in ln:
            continue
        k, v = ln.split('=', 1)
        unesc = lambda s: s.replace('\\n', '\n').replace('\\t', '\t')
        out[unesc(k)] = unesc(v)
    return out


def main():
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except AttributeError:
        pass
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    code = sys.argv[1].lower()
    pack = load_pack(os.path.join(HERE, '..', 'assets', 'gamedata', 'lang', code + '.lang'))

    tr = {}
    for _sec, key, raw, _rd, _note in tmpl.records:
        v = pack.get(key)
        if v is None:
            continue
        # QUICK. text is stored as drawn; everything else is engine format.
        tr[key] = v.strip() if key.startswith('QUICK.') else tmpl.readable(v)

    legend = tmpl.LEGEND.replace(
        ' HOW TO USE\n',
        ' UPDATING AN EXISTING TRANSLATION\n'
        ' --------------------------------\n'
        ' The %s: lines already hold the current %s translation. Fill in the\n'
        ' EMPTY ones (text added to the game since) and correct anything else.\n\n'
        ' HOW TO USE\n' % (code.upper(), code.upper())).replace('TR:', code.upper() + ':')

    out = os.path.join(HERE, 'SilentHill_%s_for_update.txt' % code.upper())
    tmpl.write_template(out, tr_label=code.upper(), tr_text=tr, legend=legend)
    blank = sum(1 for r in tmpl.records if r[1] not in tr)
    print('filled %d of %d, %d left for the translator' % (len(tr), len(tmpl.records), blank))
    print('WROTE:', out)


if __name__ == '__main__':
    main()
