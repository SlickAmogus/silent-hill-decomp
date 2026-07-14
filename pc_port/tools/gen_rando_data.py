#!/usr/bin/env python3
"""Generate pc_port/include/pc_rando_data.h for the randomizer gamemode.

Two tables are harvested straight from the map sources:

1. ARRIVAL RECORDS. A door's arrival spawn point is authored in the SOURCE map's
   MAP_POINTS[] but expressed in the DESTINATION map's coordinate space (verified:
   map2_s04's door to map2_s02 carries MAP_POINTS[9] = (-99.0, 11.0), and map2_s02's
   own door-trigger for that doorway sits at (-99.0, 11.25) in its own space).
   So to teleport into an arbitrary map we scan every map for `SysState_LoadOverlay`
   events targeting it and copy the mapPoint each one names. These are real,
   designer-authored, guaranteed-valid entry points.

   Room-transition (`SysState_LoadRoom`) events name a mapPoint in the SAME map --
   those are harvested per-map as in-map arrival points (used for "another room in
   this map" doors). Their event `mapIdx` field is a BGM track, not a map, and is
   carried along.

2. MESSAGE COUNTS. `mapMessages` has no length anywhere at runtime, and the
   randomizer needs to append its own item prompts past the end of each map's
   array. Count the entries in each MAP_MESSAGES[] (15 shared common lines +
   the map's own).

Run from the repo root:  python3 pc_port/tools/gen_rando_data.py
"""

import os
import re
import sys
import glob

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(REPO, "pc_port", "include", "pc_rando_data.h")

# The 15 lines every map inherits from include/maps/shared/map_msg_common.h.
COMMON_MSG_COUNT = 15


def top_level_braces(text):
    """Split a brace-initializer body into its top-level {...} blocks."""
    out, depth, start = [], 0, None
    for i, c in enumerate(text):
        if c == '{':
            if depth == 0:
                start = i
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                out.append(text[start:i + 1])
    return out


def strip_comments(s):
    s = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
    return re.sub(r'//[^\n]*', '', s)


def field(block, name):
    m = re.search(r'\.' + name + r'\s*=\s*([^,\n}]+)', block)
    return m.group(1).strip() if m else None


def map_points(m):
    """Return each map point's C initializer body, verbatim, so we can re-emit it."""
    path = os.path.join(REPO, 'src', 'maps', m, 'map_points.h')
    if not os.path.exists(path):
        return []
    return [strip_comments(b) for b in top_level_braces(open(path).read())]


def map_events(m):
    files = glob.glob(os.path.join(REPO, 'src', 'maps', m, '*events_data.c'))
    if not files:
        return []
    t = open(files[0]).read()
    body = t[t.index('{') + 1: t.rindex('}')]
    return [strip_comments(b) for b in top_level_braces(body)]


def msg_count(m):
    """Count MAP_MESSAGES[] entries: the 15 shared ones plus the map's own strings."""
    for f in glob.glob(os.path.join(REPO, 'src', 'maps', m, '*.c')):
        t = open(f, errors='ignore').read()
        i = t.find('MAP_MESSAGES[]')
        if i < 0:
            continue
        j = t.index('{', i)
        depth, k = 0, j
        while k < len(t):
            if t[k] == '{':
                depth += 1
            elif t[k] == '}':
                depth -= 1
                if depth == 0:
                    break
            k += 1
        body = t[j + 1:k]
        has_common = 'map_msg_common.h' in body
        # Count top-level commas outside string literals. Adjacent-concatenated
        # parts ("a" "b") and multi-line literals then count as ONE entry, which a
        # line-based count would get wrong -- and an overcount here would make the
        # randomizer copy pointers from past the end of MAP_MESSAGES[].
        body = re.sub(r'^\s*#include[^\n]*$', '', body, flags=re.M)
        own, depth, i, in_str = 0, 0, 0, False
        while i < len(body):
            c = body[i]
            if in_str:
                if c == '\\':
                    i += 2
                    continue
                if c == '"':
                    in_str = False
            elif c == '"':
                in_str = True
            elif c == '{':
                depth += 1
            elif c == '}':
                depth -= 1
            elif c == ',' and depth == 0:
                own += 1
            i += 1
        # A trailing entry without a comma still counts.
        if re.search(r'"[^"]*"\s*$', body.strip()):
            own += 1
        return (COMMON_MSG_COUNT if has_common else 0) + own
    return 0


def is_origin(rec):
    x = field(rec, 'positionX') or ''
    z = field(rec, 'positionZ') or ''
    num = lambda s: re.search(r'Q12\(\s*(-?[\d.]+)f?\s*\)', s)
    mx, mz = num(x), num(z)
    return bool(mx and mz and float(mx.group(1)) == 0.0 and float(mz.group(1)) == 0.0)


def door_func_mask(m):
    """Which of mapEventFuncs[0]/[1] are the shared jammed/locked door handlers."""
    path = os.path.join(REPO, 'src', 'maps', m, m + '_header.c')
    if not os.path.exists(path):
        return 0
    t = strip_comments(open(path).read())
    mm = re.search(r'g_MapEventFuncs\[\][^=]*=\s*\{(.*?)\n\};', t, re.S)
    if not mm:
        return 0
    ents = [e.strip() for e in mm.group(1).split(',') if e.strip()]
    mask = 0
    if len(ents) > 0 and ents[0] == 'MapEvent_DoorJammed':
        mask |= 1
    if len(ents) > 1 and ents[1] == 'MapEvent_DoorLocked':
        mask |= 2
    return mask


MAP_NAMES = []
for n in range(8):
    for s in range(7):
        d = os.path.join(REPO, 'src', 'maps', 'map%d_s%02d' % (n, s))
        if os.path.isdir(d):
            MAP_NAMES.append('map%d_s%02d' % (n, s))

IDX = {name: i for i, name in enumerate(MAP_NAMES)}


def map_idx_of(token):
    """`MapIdx_MAP2_S04` -> index."""
    if not token or not token.startswith('MapIdx_'):
        return None
    return IDX.get(token[len('MapIdx_'):].lower())


def main():
    # dest map -> list of (source map, mapPoint initializer)
    arrivals = {m: [] for m in MAP_NAMES}
    # map -> list of (mapPoint initializer, bgmIdx token) for in-map room transitions
    rooms = {m: [] for m in MAP_NAMES}
    counts = {}
    doorfn = {}

    for m in MAP_NAMES:
        pts = map_points(m)
        counts[m] = msg_count(m)
        doorfn[m] = door_func_mask(m)
        for e in map_events(m):
            ss = field(e, 'sysState')
            ep = field(e, 'eventParam')
            if ep is None or not ep.isdigit():
                continue
            ep = int(ep)
            if ep >= len(pts):
                continue
            rec = pts[ep]
            # triggerParam1 == 1 means "offset the arrival by the player's position
            # relative to a POI in the source map" -- meaningless for a teleport, skip.
            if field(rec, 'triggerParam1') == '1':
                continue
            # Exactly one record in the game is a (0,0) placeholder: map4_s05's
            # post-Floatstinger exit to map2_s02, where the death cutscene repositions
            # the player instead. Teleporting to it would drop Harry at the origin.
            if is_origin(rec):
                continue
            if ss == 'SysState_LoadOverlay':
                dst = map_idx_of(field(e, 'mapIdx'))
                if dst is not None:
                    # Several door rows commonly share one arrival point (a doorway with
                    # a locked/unlocked pair, or two trigger volumes). Dedupe on the
                    # landing pose so the random pick isn't biased toward those.
                    key = (field(rec, 'positionX'), field(rec, 'positionZ'),
                           field(rec, 'triggerParam0'))
                    if key not in {(field(r, 'positionX'), field(r, 'positionZ'),
                                    field(r, 'triggerParam0'))
                                   for _, r in arrivals[MAP_NAMES[dst]]}:
                        arrivals[MAP_NAMES[dst]].append((m, rec))
            elif ss == 'SysState_LoadRoom':
                rooms[m].append((rec, field(e, 'mapIdx') or '0', ep))

    def emit_rec(rec):
        body = ' '.join(l.strip() for l in rec.strip()[1:-1].split('\n') if l.strip())
        return '{ %s }' % body.rstrip(',').strip()

    L = []
    L.append('/* GENERATED by pc_port/tools/gen_rando_data.py -- do not edit by hand.')
    L.append(' *')
    L.append(' * Arrival records for the randomizer gamemode. A door\'s arrival spawn point')
    L.append(' * is stored in the SOURCE map\'s mapPoints[] but holds coordinates in the')
    L.append(' * DESTINATION map\'s space, so a teleport into map X must reuse a record that')
    L.append(' * some other map authored for its own door into X. These are those records.')
    L.append(' */')
    L.append('#ifndef PC_RANDO_DATA_H')
    L.append('#define PC_RANDO_DATA_H')
    L.append('')
    L.append('#include "bodyprog/map/map.h"')
    L.append('')
    L.append('typedef struct { s_MapPoint2d point; u8 bgmIdx; u8 pointIdx; } s_RandoRoomPoint;')
    L.append('')

    # Per-map arrival tables.
    for m in MAP_NAMES:
        if not arrivals[m]:
            continue
        L.append('/* %s <- authored by: %s */' % (m, ', '.join(sorted({s for s, _ in arrivals[m]}))))
        L.append('static const s_MapPoint2d RANDO_ARRIVALS_%s[] = {' % m.upper())
        for src, rec in arrivals[m]:
            L.append('    %s, /* from %s */' % (emit_rec(rec), src))
        L.append('};')
        L.append('')

    L.append('typedef struct { const s_MapPoint2d* points; u8 count; } s_RandoArrivalSet;')
    L.append('static const s_RandoArrivalSet RANDO_ARRIVALS[] = {')
    for m in MAP_NAMES:
        if arrivals[m]:
            L.append('    [MapIdx_%s] = { RANDO_ARRIVALS_%s, (u8)ARRAY_SIZE(RANDO_ARRIVALS_%s) },'
                     % (m.upper(), m.upper(), m.upper()))
        else:
            L.append('    [MapIdx_%s] = { NULL, 0 },' % m.upper())
    L.append('};')
    L.append('')

    # Per-map in-map room arrival points (for "another room in this map" doors).
    for m in MAP_NAMES:
        if not rooms[m]:
            continue
        L.append('static const s_RandoRoomPoint RANDO_ROOMS_%s[] = {' % m.upper())
        seen = set()
        for rec, bgm, ep in rooms[m]:
            if ep in seen:
                continue
            seen.add(ep)
            bgmv = bgm if bgm.isdigit() else '0'
            L.append('    { %s, %s, %d },' % (emit_rec(rec), bgmv, ep))
        L.append('};')
        L.append('')

    L.append('typedef struct { const s_RandoRoomPoint* points; u8 count; } s_RandoRoomSet;')
    L.append('static const s_RandoRoomSet RANDO_ROOMS[] = {')
    for m in MAP_NAMES:
        if rooms[m]:
            L.append('    [MapIdx_%s] = { RANDO_ROOMS_%s, (u8)ARRAY_SIZE(RANDO_ROOMS_%s) },'
                     % (m.upper(), m.upper(), m.upper()))
        else:
            L.append('    [MapIdx_%s] = { NULL, 0 },' % m.upper())
    L.append('};')
    L.append('')

    L.append('/* MAP_MESSAGES[] entry count per map. mapMessages carries no length at')
    L.append(' * runtime and the randomizer appends its own item prompts past the end.')
    L.append(' * 0 = the map only extern-declares MAP_MESSAGES (unused stub) -- never')
    L.append(' * run the mode there. */')
    L.append('static const u8 RANDO_MSG_COUNT[] = {')
    for m in MAP_NAMES:
        L.append('    [MapIdx_%s] = %d,' % (m.upper(), counts[m]))
    L.append('};')
    L.append('')

    L.append('/* MAP_POINTS[] entry count per map. The randomizer scatters monsters over a')
    L.append(' * map\'s own points (filtered by a walkable-ground test, which also rejects the')
    L.append(' * arrival records held in other maps\' coordinate spaces), so it needs a bound. */')
    L.append('static const u8 RANDO_MAPPOINT_COUNT[] = {')
    for m in MAP_NAMES:
        L.append('    [MapIdx_%s] = %d,' % (m.upper(), len(map_points(m))))
    L.append('};')
    L.append('')

    L.append('/* A vanilla locked/jammed door is a second event row on the doorway\'s POI')
    L.append(' * whose sysState is SysState_EventCallback and whose eventParam selects the')
    L.append(' * shared MapEvent_DoorJammed / MapEvent_DoorLocked. Those sit at')
    L.append(' * mapEventFuncs[0] and [1] -- but only in SOME maps, so identifying (and')
    L.append(' * unlocking) them needs this per-map mask.')
    L.append(' *   bit 0 = mapEventFuncs[0] is MapEvent_DoorJammed')
    L.append(' *   bit 1 = mapEventFuncs[1] is MapEvent_DoorLocked */')
    L.append('#define RANDO_DOORFN_JAMMED  (1 << 0)')
    L.append('#define RANDO_DOORFN_LOCKED  (1 << 1)')
    L.append('static const u8 RANDO_DOORFN_MASK[] = {')
    for m in MAP_NAMES:
        L.append('    [MapIdx_%s] = %d,' % (m.upper(), doorfn[m]))
    L.append('};')
    L.append('')
    L.append('#endif /* PC_RANDO_DATA_H */')

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, 'w').write('\n'.join(L) + '\n')

    # Report coverage for the maps the mode actually uses.
    used = ['map2_s04', 'map3_s03', 'map5_s01', 'map6_s01', 'map6_s04',
            'map2_s02', 'map1_s05', 'map4_s05', 'map7_s03']
    print('wrote %s' % OUT)
    print('\n%-10s %8s %8s %6s' % ('map', 'arrivals', 'rooms', 'msgs'))
    for m in used:
        flag = '' if arrivals[m] else '   <-- NO ARRIVAL RECORD'
        print('%-10s %8d %8d %6d%s' % (m, len(arrivals[m]), len(rooms[m]), counts[m], flag))
    over = [m for m in MAP_NAMES if counts[m] > 200]
    if over:
        print('\nWARNING: message count > 200 in %s' % over)

    # Cross-check: a SysState_ReadMessage event's eventParam IS a message index, and
    # Event_ItemTake's 4th arg is one too. Every referenced index must be < the count.
    # The "Unused" stub maps only extern-declare MAP_MESSAGES and never define it,
    # so they have no message table to count. RANDO_MSG_COUNT is 0 for them, which
    # pc_rando.c treats as "never run the mode here".
    nodef = [m for m in MAP_NAMES if counts[m] == 0]
    if nodef:
        print('\nno MAP_MESSAGES definition (unused stub maps, count 0): %s' % ', '.join(nodef))

    print('\nmessage-count validation (max referenced index must be < count):')
    bad = 0
    for m in MAP_NAMES:
        if counts[m] == 0:
            continue
        refs = []
        for e in map_events(m):
            if field(e, 'sysState') == 'SysState_ReadMessage':
                ep = field(e, 'eventParam')
                if ep and ep.isdigit():
                    refs.append(int(ep))
        for f in glob.glob(os.path.join(REPO, 'src', 'maps', m, '*.c')):
            src = strip_comments(open(f, errors='ignore').read())
            for mm in re.finditer(r'Event_ItemTake\s*\([^;]*?,\s*(\d+)\s*\)', src, re.S):
                refs.append(int(mm.group(1)))
        if refs and max(refs) >= counts[m]:
            print('  BAD  %-10s max ref %3d >= count %3d' % (m, max(refs), counts[m]))
            bad += 1
    if bad == 0:
        print('  OK -- all %d maps consistent' % len(MAP_NAMES))
    else:
        sys.exit('message counts are wrong; fix msg_count()')


if __name__ == '__main__':
    main()
