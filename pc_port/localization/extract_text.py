#!/usr/bin/env python3
# Extract every English in-game string (dialogue, prompts, menus, PC menus, items) for translation.
import os, re, json, sys

ROOT = r"C:\Claude\silenthill\silent-hill-decomp"
COMMON = os.path.join(ROOT, "include", "maps", "shared", "map_msg_common.h")
ITEMS  = os.path.join(ROOT, "src", "bodyprog", "items", "item_screens_3.c")
MENU   = os.path.join(ROOT, "pc_port", "src", "lang_menu.c")
OPTIONS = os.path.join(ROOT, "src", "screens", "options", "options.c")

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
# '=' would end the key in a .lang line; lang_pack.c maps it the same way.
def menu_key(lit):
    return 'MENU.' + lit.replace(chr(1), '').replace('_', ' ').strip().replace(' ', '_').replace('=', '-')

menu_body = extract_array_body(MENU, r's_MenuTr\s*\[\s*\]\s*=')
menu_literals = []
if menu_body:
    # each entry: { "KEY", { ... } }  -> take first string literal per top-level {..}
    depth = 0; cur_entry = ''
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
    for e in entries:
        parsed = parse_c_entries(e)
        if parsed and parsed[0][0] == 'str':
            menu_literals.append(parsed[0][1])

# --- PC OPTIONS (options.c): row names page by page, then value labels. They
#     are looked up at runtime by their US literal exactly like s_MenuTr, so
#     they share the MENU.<literal> key space. ---
with open(OPTIONS, encoding='utf-8') as f:
    opt_src = f.read()

# (table, page, label budget, value budget) in characters: the label column
# starts at x=64 and the value column at 196 / 204 / 240, so these are the
# widest English strings each column already holds ("Texture Filter",
# "Disable Culling", "Mouse Sensitivity"; "Color Grade", "C + Shadows",
# "Bottom L").
PCOPT_PAGES = [('PCOPT_G', 'Graphics', 14, 11), ('PCOPT_S', 'System', 15, 11),
               ('PCOPT_C', 'Controls', 17, 8), ('PCOPT_T', 'Camera', 17, 8),
               ('PCOPT_H', 'HUD', 17, 8)]
pcopt_rows = []     # (literal, page title, label budget)
pcopt_seen = set()
for arr, title, lbl_budget, _ in PCOPT_PAGES:
    body = extract_array_body(OPTIONS, arr + r'\s*\[\s*\]\s*=')
    assert body, arr
    for m in re.finditer(r'\{\s*"([^"]+)"\s*,', body):
        lit = m.group(1)
        if lit not in pcopt_seen:
            pcopt_seen.add(lit)
            pcopt_rows.append((lit, title, lbl_budget))

# value budget per label array = tightest page that uses it
lbl_arrays = {}
for m in re.finditer(r'static const char\* const (LBL_\w+)\[\]\s*=\s*\{([^}]*)\}', opt_src):
    lbl_arrays[m.group(1)] = re.findall(r'"([^"]*)"', m.group(2))
lbl_budget = {}
for arr, title, _, val_budget in PCOPT_PAGES:
    body = extract_array_body(OPTIONS, arr + r'\s*\[\s*\]\s*=')
    for name in re.findall(r'\b(LBL_\w+)\b', body):
        lbl_budget[name] = min(lbl_budget.get(name, 99), val_budget)
pcopt_vals = []
for name, vals in lbl_arrays.items():
    if name not in lbl_budget:
        continue
    for v in vals:
        if not re.search(r'[A-Za-z]', v) or re.fullmatch(r'\d+x', v):
            continue   # bare numbers and 2x/4x/8x: nothing to translate
        if v not in pcopt_seen:
            pcopt_seen.add(v)
            pcopt_vals.append((v, lbl_budget[name]))

PCOPT_EXTRA = ['PC_Options', '[R]_Reset']   # heading + reset hint drawn by the same menu

# MENU section = s_MenuTr minus whatever a dedicated section owns: PC Options
# here, and the results screen / inventory labels further down, which carry
# their own notes. Emitted at the end, once those have claimed their keys.
def emit_plain_menu():
    claimed = {r[1] for r in records}
    seen = set()
    for lit in menu_literals:
        if lit in pcopt_seen or lit in PCOPT_EXTRA or menu_key(lit) in claimed:
            continue
        r = readable(lit)
        if r and r not in seen:
            seen.add(r)
            records.append(('MENU', menu_key(lit), lit, r, ''))

for lit in PCOPT_EXTRA:
    note = 'menu heading' if lit == 'PC_Options' else 'hint beside the heading, keep [R]'
    records.append(('PCOPT', menu_key(lit), lit, readable(lit), note))
for lit, page, budget in pcopt_rows:
    shared = ' (also used in the original menus)' if lit in menu_literals else ''
    records.append(('PCOPT', menu_key(lit), lit, readable(lit),
                    f'row name, {page} page, max ~{budget} characters{shared}'))
for lit, budget in pcopt_vals:
    shared = ' (also used in the original menus)' if lit in menu_literals else ''
    records.append(('PCOPT', menu_key(lit), lit, readable(lit),
                    f'setting value, max ~{budget} characters{shared}'))

# --- QUICK MENU (F10), CONTROLS PANEL, confirm boxes. These are drawn with a
#     TrueType font, so they take real spaces and any Unicode letter. Keys are
#     'QUICK.' + the English with ' '->'_' and '='->'-' (pc_port/src/lang_quick.c
#     builds the same key). {name} placeholders are filled in by the game. ---
QO_SRC   = os.path.join(ROOT, 'pc_port', 'src', 'pc_quick_options.c')
CH_SRC   = os.path.join(ROOT, 'pc_port', 'src', 'pc_cheats.c')
BP_SRC   = os.path.join(ROOT, 'pc_port', 'src', 'pc_bind_panel.c')
CS_SRC   = os.path.join(ROOT, 'pc_port', 'src', 'control_style.c')
CD_SRC   = os.path.join(ROOT, 'pc_port', 'src', 'pc_confirm_dialog.c')
qo_src = open(QO_SRC, encoding='utf-8').read()
ch_src = open(CH_SRC, encoding='utf-8').read()
bp_src = open(BP_SRC, encoding='utf-8').read()
cs_src = open(CS_SRC, encoding='utf-8').read()

def quick_key(lit):
    return 'QUICK.' + lit.replace(' ', '_').replace('=', '-')

quick_seen = {}
def add_quick(sect, lit, note=''):
    k = quick_key(lit)
    if k in quick_seen:
        assert quick_seen[k] == lit, ('QUICK key collision', k, lit)
        return
    quick_seen[k] = lit
    records.append((sect, k, lit, lit, note))

m = re.search(r's_pageNames\[QO_PAGES\]\s*=\s*\{([^}]*)\}', qo_src)
for p in re.findall(r'"([^"]+)"', m.group(1)):
    add_quick('QUICK', p, 'page name (title bar and Previous / Next row)')
add_quick('QUICK', 'QUICK OPTIONS', 'title bar, before the page name')
for cam in ['Classic', 'Thirdperson', 'Over-the-Shoulder', 'Firstperson']:
    add_quick('QUICK', cam, 'camera name in the View page title')
add_quick('QUICK', 'Previous', 'page row: go to the previous page')
add_quick('QUICK', 'Next', 'page row: go to the next page')
add_quick('QUICK', 'Close', 'bottom row: close the menu')
for lit in re.findall(r'\{\s*ROW_(?:EXTRA|ACTION)\s*,\s*NULL\s*,\s*\w+\s*,\s*"([^"]+)"', qo_src):
    add_quick('QUICK', lit, 'row name')
for lit in ['Auto', 'Stereo', 'Quad', 'HRTF']:
    add_quick('QUICK', lit, 'Speaker Layout value')
for lit in ['Simple', 'Advanced']:
    add_quick('QUICK', lit, 'Control Type value')
add_quick('QUICK', '{n} rows', 'value, e.g. "+2 rows" (screen rows). Keep {n}')
add_quick('QUICK', '(default)', 'shown after a FOV value that is the default')
add_quick('QUICK', '(off)', 'shown after Dream Blur Strength while Dream Blur is off')
add_quick('QUICK', '(simple)', 'shown after Aspect Trim when it only applies to Simple')
for lit in ['Up/Down select', 'Left/Right adjust', 'Q/E or PgUp/PgDn page',
            'Drag the title to move', '{key} or Esc close', '* needs restart']:
    add_quick('QUICK', lit, 'help line at the bottom. Keep key names and {key}')

for lit in re.findall(r'\{\s*"([^"]+)",\s*CH_\w+', ch_src):
    add_quick('CHEATS', lit, 'cheat / debug row. Keep key names in brackets')
add_quick('CHEATS', '(not in this map)', 'after a Spawn name that cannot appear here')

add_quick('CONTROLS', 'CONTROLS', 'panel title')
for lit in re.findall(r'\{\s*"([^"]+)",\s*\{\s*"key_', bp_src):
    add_quick('CONTROLS', lit, 'game action')
for lit in re.findall(r'\{\s*"\w+",\s*"([^"]+)"\s*\}', cs_src):
    add_quick('CONTROLS', lit, 'camera name')
for lit, note in [
        ('Editing: alternate camera binds (current camera: {camera})', 'Keep {camera}'),
        ('Editing: classic camera binds', ''),
        ('Controller: {name}', 'Keep {name} (the controller model)'),
        ('Controller: none connected', ''),
        ('Action', 'column heading'), ('Keyboard', 'column heading'),
        ('Keyboard 2', 'column heading'), ('Controller', 'column heading'),
        ('Controller 2', 'column heading'),
        ('Reset to Defaults', 'button'),
        ('Press a key...', 'in the cell being rebound'),
        ('Press a button...', 'in the cell being rebound'),
        ('more above', 'scroll hint'), ('more below', 'scroll hint'),
        ('Press a key for {action}', 'Keep {action}'),
        ('Press a key or mouse button for {action}', 'Keep {action}'),
        ('Press a controller button for {action}', 'Keep {action}'),
        ('Esc to cancel', ''),
        ('Esc, or wait a few seconds, to cancel', ''),
        ('Enter / A: rebind', 'help line. Keep key names'),
        ('Delete / X: clear', 'help line. Keep key names'),
        ('Esc / B: back', 'help line. Keep key names'),
        ('Mouse: click a bind to change it, right-click to clear. Orange = used twice.', 'help line'),
        ('Left Stick', 'controller input name'),
        ('Left Mouse', 'mouse button name'), ('Right Mouse', 'mouse button name'),
        ('Middle Mouse', 'mouse button name'),
        ('Wheel Up', 'mouse wheel'), ('Wheel Down', 'mouse wheel'),
        ('D-pad Up', 'controller input name'), ('D-pad Down', 'controller input name'),
        ('D-pad Left', 'controller input name'), ('D-pad Right', 'controller input name'),
        ('RESET CONTROLS', 'confirm box title'),
        ('Reset the classic camera binds to their defaults?', 'confirm box'),
        ('Reset the alternate camera binds to their defaults?', 'confirm box'),
        ('RESET SETTINGS', 'confirm box title'),
        ('Reset all PC options to their defaults?', 'confirm box'),
        ('Yes', 'confirm box button'), ('No', 'confirm box button'),
        ('Left/Right select', 'confirm box help. Keep key names'),
        ('[OK] confirm', 'confirm box help. Keep the brackets'),
        ('[Cancel] back', 'confirm box help. Keep the brackets')]:
    add_quick('CONTROLS', lit, note)

# --- ITEMS ---
for arr, sect in [(r'INVENTORY_ITEM_NAMES\s*\[\s*\]\s*=', 'ITEM_NAME'),
                  (r'g_ItemDescriptions\s*\[\s*\]\s*=', 'ITEM_DESC')]:
    body = extract_array_body(ITEMS, arr)
    if body is None: continue
    pp = preprocess_region(body)
    for idx, (kind, val) in enumerate(parse_c_entries(pp)):
        if kind == 'str' and val.strip():
            records.append((sect, f'{sect}.{idx}', val, readable(val), ''))

# --- RESULTS SCREEN (Results_DisplayInfo, after the credits). Also drawn
#     through Gfx_StringDraw, so MENU.<literal> keys. Budgets are the room
#     between the label (x=24, or x=72 for the shot rows) and where the value
#     is drawn, at the font's ~10px per glyph. Units (h/m/s/km), separators
#     and the ending names (GOOD+, UFO ...) are left as they are. ---
RESULTS_BUDGET = {
    'GAME_RESULT': (20, 'title'),
    'Mode': (18, 'label, the difficulty follows'),
    'Saves': (20, 'label, a number follows'),
    'Continues': (20, 'label, a number follows'),
    'Total_time': (13, 'label, the play time follows'),
    'Walking_distance': (16, 'label, a distance follows'),
    'Running_distance': (16, 'label, a distance follows'),
    'Items': (16, 'label, "found / total" follows'),
    'Game_clear': (20, 'label, the number of clears follows'),
    'Ending': (19, 'label, the ending name follows'),
    '==Your_rank==': (13, 'label, the rank stars follow on the same line'),
    'Defeated_enemy_by_shooting': (23, 'label, a number follows'),
    'Defeated_enemy_by_fighting': (23, 'label, a number follows'),
    'Shooting_style': (20, 'heading over the four rows below'),
    'Short_range_shots': (19, 'label, a percentage follows'),
    'Middle_range_shots': (19, 'label, a percentage follows'),
    'Long_range_shots': (19, 'label, a percentage follows'),
    'No_aiming_shots': (19, 'label, a percentage follows'),
}
res_body = extract_array_body(os.path.join(ROOT, 'src', 'bodyprog', 'ranking.c'),
                              r'void Results_DisplayInfo\(u32\* arg0\)[^{]*')
m = re.search(r'D_8002B4C0\[\]\s*=\s*\{(.*?)\};', res_body, re.S)
assert m, 'results string table'
have = {r[1] for r in records}
res_seen = 0
for kind, lit in parse_c_entries(m.group(1)):
    plain = lit.replace(chr(1), '')
    if kind != 'str' or plain not in RESULTS_BUDGET:
        continue
    res_seen += 1
    if menu_key(lit) in have:
        continue
    budget, what = RESULTS_BUDGET[plain]
    records.append(('RESULTS', menu_key(lit), lit, readable(lit), f'results screen {what}, max ~{budget} characters'))
assert res_seen == len(RESULTS_BUDGET), ('results strings moved', res_seen)

# --- INVENTORY SCREEN: prompts and labels Gfx_Inventory_ItemDescriptionDraw
#     draws straight from local arrays. They reach Gfx_StringDraw like every
#     menu string, so they are MENU.<literal> keys too. ---
inv_body = extract_array_body(ITEMS, r'void Gfx_Inventory_ItemDescriptionDraw\(s32\* selectedItemId\)[^{]*')
for arr in ('D_80027F14', 'D_80027F94'):
    m = re.search(arr + r'\[\]\s*=\s*\{(.*?)\};', inv_body, re.S)
    assert m, arr
    for kind, lit in parse_c_entries(m.group(1)):
        if kind == 'str' and menu_key(lit) not in {r[1] for r in records}:
            note = 'inventory prompt' if arr == 'D_80027F14' else 'inventory label, max ~10 characters'
            records.append(('MENU', menu_key(lit), lit, readable(lit), note))

emit_plain_menu()

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
 SILENT HILL (1999)  -  ENGLISH SCRIPT FOR TRANSLATION  (PC port)
================================================================================
 Every visible in-game English string that is NOT baked into an image:
 story/cutscene dialogue, examine/pickup prompts, the original menus, the PC
 port's own menus (PC Options, the F10 Quick Options menu, the Controls
 panel), item names and item descriptions.  Total entries: {total}.

 HOW TO USE
 ----------
 * Each entry looks like this:
       [KEY]        <- an ID. DO NOT change or translate this.
       NOTE: ...    <- (some entries) where the text appears / length limit.
       EN: ...      <- the English text.
       TR:          <- write your translation here, on this line.
 * Translate ONLY the words. Keep every control code exactly, in place.
 * Leave a TR: line empty to keep that entry in English.
 * Save the file as UTF-8 (any modern editor does). Write accented and
   non-Latin letters normally.

 CONTROL CODES (story text, prompts, items) - keep unchanged, in place:
 * ~N        line break inside a message (start a new on-screen line).
             You MAY add extra ~N if your line runs long.
 * ~E        end-of-message marker. Always keep it at the very end.
 * ~C2 ... ~C7   colour on / off. The words between them are highlighted
                 (usually an item name). Keep the codes around the same words.
 * ~S4       a Yes/No choice prompt. Keep as-is.
 * ~J0(1.2), ~J1(3.8), ...   on-screen display time in seconds for that line.
                 Keep the code AND the number exactly - do not translate it,
                 and do not add new ~J codes (they are timed to the voice).
 * ~D        a short leading marker on some map/notice lines. Keep as-is.
 * [Lion], [Woodman], [Scarecrow] ...  bracketed key names - keep the brackets.
 * {{n}}, {{key}}, {{name}}, {{action}}, {{camera}}  (PC menus) - filled in by
   the game. Keep them, spelled exactly the same, anywhere in your sentence.

 LENGTH
 ------
 * Story text: no automatic word wrap. Keep lines to about 20-24 characters
   and break them with ~N. At most 9 lines per message.
 * Original menus and PC Options: the label and its value share one line, so
   long words get cut off. NOTE: lines give a character budget - stay inside
   it, abbreviate if you must.
 * Quick Options / Controls panel: scales to fit, but aim for roughly the
   English length.
 * Technical terms may stay in English: PGXP, CRT, VSync, FMV, OTS, TPS, FOV,
   FPS, HUD, HRTF, ACES, Reinhard, PSX, Hz.

 NOTES
 -----
 * These strings use the US/NTSC English script (the game's original text).
 * A few entries are blank on purpose (timing-only lines): they show
   "(no text - timing only, leave blank)". Skip those.
 * Line order within each area follows the game's internal order, not
   necessarily on-screen order, but the English gives the context.
 * When you are done, send the file back and we import it as a new
   selectable language.
================================================================================

""".format(total=len(records))

SECTION_TITLES = {
    'COMMON':     'COMMON MESSAGES  (shown in every area: Yes/No, pickups, doors)',
    'MENU':       'MENUS & UI  (title, options, pause, save/load, memory card, inventory actions, save-location names)',
    'RESULTS':    'RESULTS SCREEN  (after the credits: play statistics and rank)',
    'PCOPT':      'PC OPTIONS MENU  (Options > PC Options: graphics, system, controls, camera, HUD)',
    'QUICK':      'QUICK OPTIONS MENU  (F10 in game)',
    'CHEATS':     'QUICK OPTIONS - CHEATS AND DEBUG PAGES',
    'CONTROLS':   'CONTROLS PANEL AND CONFIRM BOXES  (key/controller binding screen)',
    'ITEM_NAME':  'ITEM NAMES  (inventory)',
    'ITEM_DESC':  'ITEM DESCRIPTIONS  (inventory)',
}

# group records, keeping first-seen order of sections
order = []
groups = {}
for rec in records:
    sec = rec[0]
    if sec not in groups:
        groups[sec] = []; order.append(sec)
    groups[sec].append(rec)

fixed = ['COMMON', 'MENU', 'RESULTS', 'PCOPT', 'QUICK', 'CHEATS', 'CONTROLS', 'ITEM_NAME', 'ITEM_DESC']
map_secs = [s for s in order if s.startswith('MAP ')]

keys = [k for (_, k, *_r) in records]
dups = {k for k in keys if keys.count(k) > 1}
assert not dups, ('duplicate keys', sorted(dups)[:10])

# Other tools (make_ru_template.py) render the same layout with a different
# second column, so the writer takes the TR: text per key.
def write_template(path, tr_label='TR', tr_text=None, legend=LEGEND):
    lines = [legend]
    def emit_section(sec, title):
        lines.append('\n\n' + '#' * 80)
        lines.append('##  ' + title)
        lines.append('#' * 80 + '\n')
        for (_, k, raw, rd, note) in groups[sec]:
            lines.append(f'[{k}]')
            if note:
                lines.append(f'NOTE: {note}')
            lines.append(f'EN: {rd}')
            if not has_words(rd):
                lines.append(f'{tr_label}:   (no text - timing only, leave blank)')
            else:
                t = tr_text.get(k, '') if tr_text else ''
                lines.append(f'{tr_label}: {t}'.rstrip() + ('' if t else ' '))
            lines.append('')
    for s in fixed:
        if s in groups: emit_section(s, SECTION_TITLES[s])
    for s in map_secs:
        mapid = s.split(' ',1)[1]
        emit_section(s, f'DIALOGUE - {mapid}')
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines))

if __name__ == '__main__':
    txt_path = os.path.join(OUT, 'SilentHill_EN_for_translation.txt')
    write_template(txt_path)

    # raw json (machine, for re-import)
    raw_map = {k: raw for (_, k, raw, _, _) in records}
    with open(os.path.join(OUT, 'SilentHill_EN_raw.json'), 'w', encoding='utf-8', newline='\n') as f:
        json.dump(raw_map, f, ensure_ascii=False, indent=1)

    from collections import Counter
    counts = Counter(sec.split(' ')[0] for (sec, *_) in records)
    print("TOTAL STRINGS:", len(records))
    for k, v in counts.most_common():
        print(f"  {k}: {v}")
    print("MAP FILES:", len(MAP_FILES))
    print("WROTE:", txt_path)
