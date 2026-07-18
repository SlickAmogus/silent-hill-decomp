#!/usr/bin/env python3
# Extract the PC-only Options menu labels (row names + value labels) for translation.
import os, re, json

ROOT = r"C:\Claude\silenthill\silent-hill-decomp"
OPT  = os.path.join(ROOT, "src", "screens", "options", "options.c")
OUT  = os.path.join(ROOT, "pc_port", "localization")

txt = open(OPT, encoding="utf-8", errors="replace").read()

# Row labels: first string literal of each option-table entry `{ "Name", ...`
row_labels = []
seen = set()
for m in re.finditer(r'^\s*\{\s*"([A-Za-z][^"]*)"\s*,', txt, re.M):
    lbl = m.group(1)
    if lbl not in seen:
        seen.add(lbl); row_labels.append(lbl)

# Value labels: every LBL_xxx[] = { "a", "b", ... } array, de-duped globally
val_labels = []
vseen = set()
for m in re.finditer(r'LBL_[A-Z]+\[\]\s*=\s*\{([^}]*)\}', txt):
    for s in re.findall(r'"([^"]*)"', m.group(1)):
        if s and s not in vseen:
            vseen.add(s); val_labels.append(s)

def readable(s): return s.replace('_', ' ')

LEGEND = """\
================================================================================
 SILENT HILL — PC OPTIONS MENU  (English, for translation)
================================================================================
 These are the PC port's own Options menu (graphics / system / controls /
 camera). Same format as the main script file: fill in each PT: line.

 * Leave technical terms, acronyms and numbers AS-IS (do not translate):
     PGXP, CRT, VSync, FMV, OTS, TPS, FOV, FPS, ACES, Reinhard, Filmic,
     2x / 4x / 8x, 30 / 60 / 120 / 240, PSX_Retro.
 * Translate the descriptive words: On/Off, Windowed/Fullscreen/Borderless,
   Scanlines, Vignette, Film Grain, Sharpen, Cinematic, Classic, Modern,
   Shadows, Both, External, In Game, etc.
 * `_` in a label is a space — write your translation with normal spaces.
 * Keep translations SHORT: the menu draws the label on the left and the value
   on the right of one line, so very long words get clipped. Aim for roughly
   the English length.
================================================================================

"""

lines = [LEGEND]
lines.append('#' * 80)
lines.append('##  OPTION ROWS')
lines.append('#' * 80 + '\n')
for lbl in row_labels:
    lines.append(f'[PCOPT.{lbl}]')
    lines.append(f'EN: {readable(lbl)}')
    lines.append('PT: ')
    lines.append('')

lines.append('\n' + '#' * 80)
lines.append('##  OPTION VALUES  (shown on the right; some are technical — keep as-is)')
lines.append('#' * 80 + '\n')
for v in val_labels:
    lines.append(f'[PCOPT_VAL.{v}]')
    lines.append(f'EN: {readable(v)}')
    lines.append('PT: ')
    lines.append('')

os.makedirs(OUT, exist_ok=True)
path = os.path.join(OUT, 'SilentHill_PCOptions_EN_for_translation.txt')
open(path, 'w', encoding='utf-8').write('\n'.join(lines))

raw = {f'PCOPT.{l}': l for l in row_labels}
raw.update({f'PCOPT_VAL.{v}': v for v in val_labels})
json.dump(raw, open(os.path.join(OUT, 'SilentHill_PCOptions_raw.json'), 'w', encoding='utf-8'),
          ensure_ascii=False, indent=1)

print(f"row labels: {len(row_labels)}, value labels: {len(val_labels)}")
print("WROTE:", path)
