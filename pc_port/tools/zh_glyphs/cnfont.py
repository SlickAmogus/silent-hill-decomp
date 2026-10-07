"""Chinese glyphs of the NTSC-J fan translation, addressed by JIS kuten."""
import io, os, re

SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'src')


def _load():
    jp = io.open(os.path.join(SRC, 'kanji_font.inc'), encoding='utf-8', errors='replace').read()
    idx = [int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{4})',
                                          re.search(r'KANJI_FONT_IDX\[94\*94\]\s*=\s*\{(.*?)\};', jp, re.S).group(1))]
    jbits = bytes(int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{2})\b',
                                                re.search(r'KANJI_FONT_BITS\[\d+\]\s*=\s*\{(.*?)\};', jp, re.S).group(1)))
    cn = io.open(os.path.join(SRC, 'kanji_font_cn.inc'), encoding='utf-8').read()
    first = int(re.search(r'KANJI_CN_FIRST (\d+)', cn).group(1))
    cbits = bytes(int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{2})\b',
                                                re.search(r'KANJI_FONT_BITS_CN\[\d+\]\s*=\s*\{(.*?)\};', cn, re.S).group(1)))
    return idx, jbits, first, cbits


IDX, JBITS, CN_FIRST, CBITS = _load()
CN_COUNT = len(CBITS) // 32


def kuten_to_sjis(ku, ten):
    s1 = (ku + 1) // 2 + 0x80
    if s1 > 0x9F:
        s1 += 0x40
    if ku % 2:
        s2 = ten + (0x3F if ten <= 63 else 0x40)
    else:
        s2 = ten + 0x9E
    return (s1 << 8) | s2


def sjis_to_kuten(code):
    s1, s2 = code >> 8, code & 0xFF
    if s1 >= 0xE0:
        s1 -= 0x40
    ku = (s1 - 0x81) * 2 + 1
    if s2 >= 0x9F:
        ku += 1
        ten = s2 - 0x9E
    else:
        ten = s2 - (0x40 if s2 >= 0x80 else 0x3F)
    return ku, ten


def glyph(ku, ten, chinese=True):
    """16 rows of 16-bit ints, or None."""
    pos = IDX[(ku - 1) * 94 + (ten - 1)]
    if pos == 0xFFFF:
        return None
    if chinese and CN_FIRST <= pos < CN_FIRST + CN_COUNT:
        b = CBITS[(pos - CN_FIRST) * 32:(pos - CN_FIRST) * 32 + 32]
    else:
        b = JBITS[pos * 32:pos * 32 + 32]
    return [(b[2 * r] << 8) | b[2 * r + 1] for r in range(16)]


def sheet(cells, path, scale=3, cols=16):
    """cells: list of (label, rows) -> PNG with a grid."""
    from PIL import Image, ImageDraw
    cw = 16 * scale + 6
    rows = (len(cells) + cols - 1) // cols
    img = Image.new('L', (cols * cw, rows * (cw + 12)), 255)
    d = ImageDraw.Draw(img)
    for i, (label, g) in enumerate(cells):
        x0, y0 = (i % cols) * cw, (i // cols) * (cw + 12)
        d.text((x0 + 2, y0), str(label), fill=0)
        if g is None:
            continue
        for r in range(16):
            for c in range(16):
                if g[r] >> (15 - c) & 1:
                    d.rectangle([x0 + 3 + c * scale, y0 + 12 + r * scale,
                                 x0 + 3 + c * scale + scale - 1, y0 + 12 + r * scale + scale - 1], fill=0)
    img.save(path)
