"""Target hanzi next to its top candidate glyphs, for visual confirmation."""
import json, sys
sys.stdout.reconfigure(encoding='utf-8')
from PIL import Image, ImageDraw, ImageFont
import cnfont, cnmatch

chars = sys.argv[1]
out = sys.argv[2]
S = 3
CELL = 16 * S + 8
lab = ImageFont.truetype(r'C:\Windows\Fonts\msyh.ttc', 14)
TOP = int(sys.argv[3]) if len(sys.argv) > 3 else 4
img = Image.new('L', (CELL * (TOP + 1) + 40, (CELL + 18) * len(chars)), 255)
d = ImageDraw.Draw(img)
found = {}
for row, ch in enumerate(chars):
    y0 = row * (CELL + 18)
    d.text((4, y0 + 18), ch, font=ImageFont.truetype(r'C:\Windows\Fonts\msyh.ttc', 36), fill=0)
    cands = cnmatch.find(ch, top=TOP)
    found[ch] = ['%d-%d' % c for _s, c in cands]
    for i, (s, (ku, ten)) in enumerate(cands):
        x0 = 50 + i * CELL
        d.text((x0, y0), '%d:%d-%d' % (i, ku, ten), fill=0)
        g = cnfont.glyph(ku, ten)
        for r in range(16):
            for c in range(16):
                if g[r] >> (15 - c) & 1:
                    d.rectangle([x0 + c * S, y0 + 16 + r * S, x0 + c * S + S - 1, y0 + 16 + r * S + S - 1], fill=0)
img.save(out)
json.dump(found, open(out + '.json', 'w', encoding='utf-8'), ensure_ascii=False)
