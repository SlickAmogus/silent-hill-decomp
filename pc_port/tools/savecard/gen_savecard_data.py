#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generates sh_savecard_data.js (names for the save editor) from the decomp.

Run from anywhere:  python pc_port/tools/savecard/gen_savecard_data.py
Re-run when event_flags.h, items.h or map descriptions change.
"""
import json
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..', '..'))
OUT = os.path.join(HERE, 'sh_savecard_data.js')


def read(rel):
    with open(os.path.join(ROOT, rel), encoding='utf-8', errors='replace') as f:
        return f.read()


def enum_body(src, enum_name):
    m = re.search(r'typedef enum _' + enum_name + r'\s*\{(.*?)\}\s*e_' + enum_name + ';', src, re.S)
    if not m:
        raise SystemExit('enum not found: ' + enum_name)
    return m.group(1)


def enum_values(src, enum_name, prefix):
    out = []
    for m in re.finditer(r'\b' + prefix + r'(\w+)\s*=\s*([^,\n]+)', enum_body(src, enum_name)):
        expr = m.group(2).strip().split('//')[0].strip()
        expr = expr.replace('NO_VALUE', '-1')
        try:
            val = eval(expr, {})
        except Exception:
            continue
        out.append((m.group(1), val))
    return out


def spaced(name):
    s = re.sub(r'(?<=[a-z0-9])(?=[A-Z])', ' ', name)
    s = re.sub(r'(?<=[A-Z])(?=[A-Z][a-z])', ' ', s)
    return re.sub(r'\b(Of|The|To|In)\b', lambda m: m.group(1).lower(), s)


def main():
    items_h = read('include/bodyprog/items.h')
    screens_h = read('include/bodyprog/item_screens.h')
    game_h = read('include/game.h')
    map_h = read('include/bodyprog/map/map.h')
    memcard_h = read('include/bodyprog/memcard.h')
    options_h = read('include/screens/options.h')
    registry = read('pc_port/src/map_registry.c')

    data = {}
    data['items'] = {v: spaced(k) for k, v in enum_values(items_h, 'InvItemId', 'InvItemId_') if v > 0}
    data['invCommands'] = {v: spaced(k) for k, v in enum_values(items_h, 'InvCmdId', 'InvCmdId_')}
    data['endings'] = [[spaced(k), v] for k, v in enum_values(screens_h, 'GameEndingFlags', 'GameEndingFlag_')]
    data['inventoryItemFlags'] = [[spaced(k), v] for k, v in enum_values(screens_h, 'InventoryItemFlags', 'InventoryItemFlag_')]
    data['itemToggles'] = [[spaced(k), v] for k, v in enum_values(items_h, 'ItemToggleFlags', 'ItemToggleFlag_')]
    data['paperMaps'] = {v: spaced(k) for k, v in enum_values(game_h, 'PaperMapIdx', 'PaperMapIdx_')}
    data['difficulty'] = {v: k for k, v in enum_values(game_h, 'GameDifficulty', 'GameDifficulty_')}
    data['locations'] = {v: spaced(k) for k, v in enum_values(memcard_h, 'SaveLocationId', 'SaveLocationId_')}
    data['bloodColors'] = {v: k for k, v in enum_values(options_h, 'BloodColor', 'BloodColor_')}

    descs = dict(re.findall(r'\[MapIdx_(\w+)\]\s*=\s*"([^"]*[ -][^"]*)"', registry))
    maps = []
    for k, v in enum_values(map_h, 'MapIdx', 'MapIdx_'):
        maps.append({'idx': v, 'id': k, 'desc': descs.get(k, 'Unused test map' if k.startswith(('MAPT', 'MAPX')) else '')})
    data['maps'] = maps

    # Event flags: name, the doc comment of the group it sits in, its own comment,
    # and where the code uses it.
    flags = {}
    group = ''
    group_used = False
    for line in read('include/event_flags.h').splitlines():
        s = line.strip()
        gm = re.match(r'/\*\*\s*(.*?)\s*\*/', s)
        if gm and 'Indices for game event flags' not in gm.group(1):
            group = gm.group(1).replace('`', '')
            group_used = False
            continue
        fm = re.match(r'EventFlag_(\w+)\s*=\s*(\d+)\s*,?\s*(?://\s*(.*))?', s)
        if fm:
            name, idx, note = fm.group(1), int(fm.group(2)), (fm.group(3) or '').replace('`', '')
            if idx == 0:
                continue
            group_used = True
            flags[idx] = {'name': name, 'group': group, 'note': note.strip('} ').strip(), 'maps': set(), 'funcs': set()}
        elif s == '' and group_used:
            # A group comment covers the block of flags up to the next blank line.
            group = ''

    by_name = {f['name']: idx for idx, f in flags.items()}
    func_re = re.compile(r'^[A-Za-z_][\w\s\*]*?\b([A-Za-z_]\w*)\s*\([^;]*$')
    for dirpath, _, files in os.walk(os.path.join(ROOT, 'src')):
        rel = os.path.relpath(dirpath, os.path.join(ROOT, 'src')).replace('\\', '/')
        mm = re.match(r'maps/(map\d_s\d\d|mapt_s00|mapx_s00)', rel)
        where = mm.group(1).upper() if mm else ('game code (' + rel.split('/')[0] + ')')
        for fn in files:
            if not fn.endswith(('.c', '.h')):
                continue
            func = ''
            with open(os.path.join(dirpath, fn), encoding='utf-8', errors='replace') as f:
                for line in f:
                    if line and not line[0].isspace() and '(' in line and not line.startswith(('#', '//', '/*', '*', 'typedef', 'extern', 'static const', 'return')):
                        m = func_re.match(line)
                        if m and m.group(1) not in ('if', 'while', 'for', 'switch'):
                            func = m.group(1)
                    for m in re.finditer(r'\bEventFlag_(\w+)', line):
                        idx = by_name.get(m.group(1))
                        if idx is None:
                            continue
                        flags[idx]['maps'].add(where)
                        if func and not re.match(r'(func|sharedFunc)_[0-9A-Fa-f]{8}', func):
                            flags[idx]['funcs'].add(func)

    data['flags'] = {
        idx: [f['name'], f['group'], f['note'], sorted(f['maps']), sorted(f['funcs'])[:4]]
        for idx, f in sorted(flags.items())
    }

    js = ('// Generated by gen_savecard_data.py from the decomp headers. Do not edit.\n'
          '(function (root) {\n  const DATA = ' + json.dumps(data, ensure_ascii=False, separators=(',', ':')) + ';\n'
          "  if (typeof module !== 'undefined' && module.exports) module.exports = DATA; else root.SHCardData = DATA;\n"
          "})(typeof self !== 'undefined' ? self : this);\n")
    with open(OUT, 'w', encoding='utf-8', newline='\n') as f:
        f.write(js)
    named = sum(1 for f in flags.values() if not f['name'].isdigit())
    used = sum(1 for f in flags.values() if f['maps'])
    print(f'{OUT}: {len(js)} bytes, {len(flags)} flags ({named} named, {used} referenced in code), '
          f'{len(data["items"])} items, {len(maps)} maps')


if __name__ == '__main__':
    main()
