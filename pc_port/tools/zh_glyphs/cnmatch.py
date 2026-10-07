"""Find the JIS code whose glyph in the Chinese fan font draws a given hanzi.

Renders the character with Windows CJK fonts and scores it against all 2965
redefined glyphs (ku 16..47) by best-shift overlap. Prints the top matches so a
human can confirm them from a rendered sheet."""
import sys
sys.stdout.reconfigure(encoding='utf-8')
from PIL import Image, ImageDraw, ImageFont
import cnfont

FONTS = [r'C:\Windows\Fonts\simsun.ttc', r'C:\Windows\Fonts\msyh.ttc']

CANDS = []
for ku in range(16, 48):
    for ten in range(1, 95):
        g = cnfont.glyph(ku, ten)
        if g is None:
            continue
        pts = {(r, c) for r in range(16) for c in range(16) if g[r] >> (15 - c) & 1}
        if pts:
            CANDS.append(((ku, ten), pts))


def render(ch, font, size):
    img = Image.new('1', (24, 24), 0)
    ImageDraw.Draw(img).text((2, 2), ch, font=font, fill=1)
    px = img.load()
    pts = {(y, x) for y in range(24) for x in range(24) if px[x, y]}
    if not pts:
        return set()
    y0 = min(p[0] for p in pts); x0 = min(p[1] for p in pts)
    return {(y - y0, x - x0) for y, x in pts}


def norm(pts):
    y0 = min(p[0] for p in pts); x0 = min(p[1] for p in pts)
    return {(y - y0, x - x0) for y, x in pts}


def score(a, b):
    best = 0.0
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            sb = {(y + dy, x + dx) for y, x in b}
            inter = len(a & sb)
            best = max(best, inter / float(len(a | sb)))
    return best


REFS = {}


def refs(ch):
    if ch not in REFS:
        out = []
        for path in FONTS:
            for size in (14, 15, 16):
                try:
                    f = ImageFont.truetype(path, size)
                except OSError:
                    continue
                r = render(ch, f, size)
                if r:
                    out.append(r)
        REFS[ch] = out
    return REFS[ch]


def boxvec(pts, n=12):
    """Bounding box of the ink, resampled to n x n grey, mean-removed."""
    ys = [p[0] for p in pts]; xs = [p[1] for p in pts]
    y0, y1, x0, x1 = min(ys), max(ys), min(xs), max(xs)
    h, w = y1 - y0 + 1, x1 - x0 + 1
    img = Image.new('L', (w, h), 0)
    px = img.load()
    for y, x in pts:
        px[x - x0, y - y0] = 255
    v = list(img.resize((n, n), Image.BILINEAR).getdata())
    m = sum(v) / float(len(v))
    v = [a - m for a in v]
    s = sum(a * a for a in v) ** 0.5 or 1.0
    return [a / s for a in v]


CAND_VECS = None


def find(ch, top=3):
    global CAND_VECS
    if CAND_VECS is None:
        CAND_VECS = [(code, boxvec(pts)) for code, pts in CANDS]
    rvs = [boxvec(r) for r in refs(ch)]
    res = []
    for code, cv in CAND_VECS:
        res.append((max(sum(a * b for a, b in zip(rv, cv)) for rv in rvs), code))
    res.sort(reverse=True)
    return res[:top]


if __name__ == '__main__':
    for ch in sys.argv[1]:
        print(ch, [('%d-%d' % c, round(s, 2)) for s, c in find(ch)])
