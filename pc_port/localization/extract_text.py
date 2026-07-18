#!/usr/bin/env python3
# Extract every English in-game string (dialogue, prompts, menus, items) for translation.
import os, re, json, sys

ROOT = r"C:\Claude\silenthill\silent-hill-decomp"
COMMON = os.path.join(ROOT, "include", "maps", "shared", "map_msg_common.h")
ITEMS  = os.path.join(ROOT, "src", "bodyprog", "items", "item_screens_3.c")
MENU   = os.path.join(ROOT, "pc_port", "src", "lang_menu.c")

# ---- C string-literal parser -------------------------------------------------
def parse_c_entries(body):
    """Parse a brace-body of `"literal", NULL, "a" "b",` into a list of
    (kind, value): kind 'str' with decoded raw string, or 'null'."""
    out = []
    i, n = 0, len(body)
    cur = None          # accumulating string for current entry (None = nothing yet)
    saw_token = False   # saw a string or NULL for the current entry
    def flush():
        nonlocal cur, saw_token
        if saw_token:
            out.append(('str', cur if cur is not None else ''))
        cur, saw_token = None, False
    while i < n:
        c = body[i]
        if c == '"':
            # read one string literal (with escapes), append to cur
            i += 1
            buf = []
            while i < n and body[i] != '"':
                if body[i] == '\\' and i + 1 < n:
                    esc = body[i+1]
                    m = {'t':'\t','n':'\n','r':'\r','\\':'\\','"':'"',"'":"'",'0':'\0'}
                    if esc == 'x':
                        j = i + 2
                        hexs = ''
                        while j < n and body[j] in '0123456789abcdefABCDEF' and len(hexs) < 2:
                            hexs += body[j]; j += 1
                        buf.append(chr(int(hexs, 16)) if hexs else 'x')
                        i = j; continue
                    buf.append(m.get(esc, esc))
                    i += 2; continue
                buf.append(body[i]); i += 1
            i += 1  # closing quote
            cur = (cur or '') + ''.join(buf)
            saw_token = True
        elif c == ',':
            flush(); i += 1
        elif body[i:i+4] == 'NULL' and (i+4 >= n or not (body[i+4].isalnum() or body[i+4]=='_')):
            out.append(('null', None)); saw_token = False; cur = None
            i += 4
        elif c == '/' and i+1 < n and body[i+1] == '*':
            j = body.find('*/', i+2); i = (j+2) if j >= 0 else n
        elif c == '/' and i+1 < n and body[i+1] == '/':
            j = body.find('\n', i); i = (j+1) if j >= 0 else n
        else:
            i += 1
    flush()
    return out

# ---- mini preprocessor: keep NTSC (US English) branch, inline common ---------
def preprocess_region(text, inline_common=False):
    """Return the array-body text with only NTSC (VERSION_NTSC) branches kept
    and the common-header include inlined (NTSC branch)."""
    lines = text.split('\n')
    out = []
    # stack of (emitting_bool, any_branch_taken_bool)
    stack = []
    def emitting():
        return all(s[0] for s in stack)
    def region_of(cond):
        m = re.search(r'VERSION_REGION_IS\(\s*(\w+)\s*\)', cond)
        return m.group(1) if m else None
    for ln in lines:
        s = ln.strip()
        if s.startswith('#if'):
            r = region_of(s)
            take = (r == 'NTSC')
            stack.append([emitting() and take, take])
            continue
        if s.startswith('#elif'):
            r = region_of(s)
            take = (r == 'NTSC') and not stack[-1][1]
            parent = all(x[0] for x in stack[:-1]) if len(stack) > 1 else True
            stack[-1][0] = parent and take
            stack[-1][1] = stack[-1][1] or take
            continue
        if s.startswith('#else'):
            parent = all(x[0] for x in stack[:-1]) if len(stack) > 1 else True
            stack[-1][0] = parent and (not stack[-1][1])
            continue
        if s.startswith('#endif'):
            if stack: stack.pop()
            continue
        if s.startswith('#include') and 'map_msg_common.h' in s and inline_common:
            if emitting():
                out.append(read_common_ntsc_body())
            continue
        if emitting():
            out.append(ln)
    return '\n'.join(out)

def read_common_ntsc_body():
    with open(COMMON, encoding='utf-8') as f:
        txt = f.read()
    return preprocess_region(txt)

def extract_array_body(path, array_decl_regex):
    with open(path, encoding='utf-8') as f:
        txt = f.read()
    m = re.search(array_decl_regex, txt)
    if not m:
        return None
    start = txt.index('{', m.end()-1)
    # find matching close brace
    depth = 0; i = start
    while i < len(txt):
        if txt[i] == '{': depth += 1
        elif txt[i] == '}':
            depth -= 1
            if depth == 0: break
        i += 1
    return txt[start+1:i]

# ---- decode raw -> readable for the translator -------------------------------
def readable(raw):
    if raw is None: return ''
    s = raw
    s = s.replace('\x01', '')          # kerning control chars (menu)
    s = s.replace('\t', ' ')            # alignment tabs -> space
    s = s.replace('\n', ' ')            # embedded newline in source literal
    s = s.replace('_', ' ')             # underscore = space
    s = re.sub(r' +', ' ', s).strip()   # collapse
    return s

# =============================================================================
records = []  # (section, key, raw, readable, note)

# --- COMMON (0-14) ---
common_body = read_common_ntsc_body()
for idx, (kind, val) in enumerate(parse_c_entries(common_body)):
    if kind == 'str':
        records.append(('COMMON', f'COMMON.{idx}', val, readable(val), ''))

# --- MAPS ---
MAP_FILES = []
maps_dir = os.path.join(ROOT, 'src', 'maps')
for d in sorted(os.listdir(maps_dir)):
    md = os.path.join(maps_dir, d)
    if not os.path.isdir(md): continue
    for fn in sorted(os.listdir(md)):
        if fn.endswith('.c'):
            p = os.path.join(md, fn)
            with open(p, encoding='utf-8', errors='replace') as f:
                if 'MAP_MESSAGES[]' in f.read():
                    MAP_FILES.append((d, p)); break

for mapid, path in MAP_FILES:
    body = extract_array_body(path, r'MAP_MESSAGES\s*\[\s*\]\s*=')
    if body is None: continue
    pp = preprocess_region(body, inline_common=True)
    entries = parse_c_entries(pp)
    for idx, (kind, val) in enumerate(entries):
        if kind == 'str' and val.strip():
            # skip the common 0-14 (already emitted once) — but they're map-local;
            # keep only map-specific (idx>=15) to avoid 40x duplication.
            if idx >= 15:
                records.append((f'MAP {mapid}', f'{mapid.upper()}.{idx}', val, readable(val), ''))

# --- MENU (s_MenuTr keys) ---
menu_body = extract_array_body(MENU, r's_MenuTr\s*\[\s*\]\s*=')
if menu_body:
    # each entry: { "KEY", { ... } }  -> take first string literal per top-level {..}
    depth = 0; i = 0; cur_entry = ''
    entries = []
    for ch in menu_body:
        if ch == '{':
            depth += 1
            if depth == 1: cur_entry = ''
        elif ch == '}':
            if depth == 1: entries.append(cur_entry)
            depth -= 1
        elif depth >= 1:
            cur_entry += ch
    seen = set()
    for e in entries:
        parsed = parse_c_entries(e)
        if parsed and parsed[0][0] == 'str':
            key = parsed[0][1]
            r = readable(key)
            if r and r not in seen:
                seen.add(r)
                records.append(('MENU', f'MENU.{key.replace(chr(1),"").replace("_"," ").strip().replace(" ","_")}', key, r, ''))

# --- ITEMS ---
for arr, sect in [(r'INVENTORY_ITEM_NAMES\s*\[\s*\]\s*=', 'ITEM_NAME'),
                  (r'g_ItemDescriptions\s*\[\s*\]\s*=', 'ITEM_DESC')]:
    body = extract_array_body(ITEMS, arr)
    if body is None: continue
    pp = preprocess_region(body)
    for idx, (kind, val) in enumerate(parse_c_entries(pp)):
        if kind == 'str' and val.strip():
            records.append((sect, f'{sect}.{idx}', val, readable(val), ''))

# --- MISC PROMPTS: visible strings drawn directly and NOT already covered by
#     the s_MenuTr (MENU) table. s_MenuTr already includes inventory actions,
#     save/load + memory-card messages, AND the save-location names, so only a
#     few item-use prompts remain. Curated readable forms; re-import by hand. ---
MISC = ["Can't use here", 'Too dark too look at the item']
for i,t in enumerate(MISC):
    records.append(('MISC', f'MISC.{i}', t, t, 'in-game prompt'))

# ---- write outputs -----------------------------------------------------------
OUT = os.path.join(ROOT, 'pc_port', 'localization')
os.makedirs(OUT, exist_ok=True)

def has_words(readable_s):
    # strip control codes, then check for any letter/digit
    t = re.sub(r'~[A-Za-z]\d?(\([\d.]+\))?', '', readable_s)
    t = re.sub(r'~S\d', '', t)
    return bool(re.search(r'[A-Za-z0-9]', t))

LEGEND = """\
================================================================================
 SILENT HILL (1999)  —  ENGLISH SCRIPT FOR TRANSLATION
================================================================================
 Every visible in-game English string that is NOT baked into an image:
 story/cutscene dialogue, examine/pickup prompts, menus, item names and
 item descriptions.  Total entries: {total}.

 HOW TO USE
 ----------
 * Each entry is three lines:
       [KEY]        <- an ID. DO NOT change or translate this.
       EN: ...      <- the English text.
       PT:          <- write the Portuguese translation here, on this line.
 * Translate ONLY the words. Keep every control code exactly, in place.

 CONTROL CODES — keep these unchanged and in the same position:
 * ~N        line break inside a message (start a new on-screen line).
 * ~E        end-of-message marker. Always keep it at the very end.
 * ~C2 ... ~C7   colour on / off. The words between them are highlighted
                 (usually an item name). Keep the codes around the same words.
 * ~S4       a Yes/No choice prompt. Keep as-is.
 * ~J0(1.2), ~J1(3.8), ...   on-screen display time in seconds for that line.
                 Keep the code AND the number exactly — do not translate it.
                 (If the Portuguese line is much longer than the English and
                 reads too fast, note it and we can raise the number later.)
 * ~D        a short leading marker on some map/notice lines. Keep as-is.
 * [Lion], [Woodman], [Scarecrow] ...  bracketed key names — keep the brackets.

 NOTES
 -----
 * These strings use the US/NTSC English script (the game's original text).
 * A few entries are blank on purpose (timing-only lines): they show
   "(no text — timing only, leave PT blank)". Skip those.
 * Line order within each area follows the game's internal order, not
   necessarily on-screen order, but the English gives the context.
 * When every PT line is filled, send the file back and we import it as a new
   selectable language.
================================================================================

""".format(total=len(records))

SECTION_TITLES = {
    'COMMON':     'COMMON MESSAGES  (shown in every area: Yes/No, pickups, doors)',
    'MENU':       'MENUS & UI  (title, options, pause, save/load, memory card, inventory actions, save-location names)',
    'ITEM_NAME':  'ITEM NAMES  (inventory)',
    'ITEM_DESC':  'ITEM DESCRIPTIONS  (inventory)',
    'MISC':       'OTHER IN-GAME PROMPTS',
}

# group records, keeping first-seen order of sections
order = []
groups = {}
for rec in records:
    sec = rec[0]
    if sec not in groups:
        groups[sec] = []; order.append(sec)
    groups[sec].append(rec)

# emit: fixed sections first, then maps in id order
fixed = ['COMMON', 'MENU', 'ITEM_NAME', 'ITEM_DESC', 'MISC']
map_secs = [s for s in order if s.startswith('MAP ')]

lines = [LEGEND]
def emit_section(sec, title):
    lines.append('\n\n' + '#' * 80)
    lines.append('##  ' + title)
    lines.append('#' * 80 + '\n')
    for (_, k, raw, rd, note) in groups[sec]:
        lines.append(f'[{k}]')
        if not has_words(rd):
            lines.append(f'EN: {rd}')
            lines.append('PT:   (no text — timing only, leave PT blank)')
        else:
            lines.append(f'EN: {rd}')
            lines.append('PT: ')
        lines.append('')

for s in fixed:
    if s in groups: emit_section(s, SECTION_TITLES[s])
for s in map_secs:
    mapid = s.split(' ',1)[1]
    emit_section(s, f'DIALOGUE — {mapid}')

txt_path = os.path.join(OUT, 'SilentHill_EN_for_translation.txt')
with open(txt_path, 'w', encoding='utf-8') as f:
    f.write('\n'.join(lines))

# raw json (machine, for re-import)
raw_map = {k: raw for (_, k, raw, _, _) in records}
with open(os.path.join(OUT, 'SilentHill_EN_raw.json'), 'w', encoding='utf-8') as f:
    json.dump(raw_map, f, ensure_ascii=False, indent=1)

# stats
from collections import Counter
counts = Counter(sec.split(' ')[0] for (sec, *_) in records)
print("TOTAL STRINGS:", len(records))
for k, v in counts.most_common():
    print(f"  {k}: {v}")
print("MAP FILES:", len(MAP_FILES))
print("WROTE:", txt_path)
