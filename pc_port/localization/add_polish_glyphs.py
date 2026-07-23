#!/usr/bin/env python3
"""Add the Polish letterforms to a loose HD FONT16.png so an HD font pack works
with the Polish language pack (language = pl, EUR only).

Background. Polish ships as a PC-side pack ([[project_polish_language_pack]]);
its glyphs are normally built into the low-res disc FONT16 atlas at upload time
(font_region.c, Font_PatchPolishGlyphs). A loose HD font override
(gamedata/load/1ST/FONT16.png) replaces that texture wholesale in VRAM, so the
low-res patch never reaches the screen and the Polish letters go blank. This
tool paints the same seven built glyphs into the HD PNG instead, at HD quality.

Only seven glyphs need it: the composed accents (c/s/n/z-acute, o-acute, and
their capitals) render for free — the draw path stacks the HD acute mark
(cell 119) over the HD base letter at run time. The seven that modify the
letter body or add a below/near mark are built here:

    a-ogonek  e-ogonek  z-dot(lower)   l-stroke
    A-ogonek  E-ogonek  L-stroke       + the Z-dot combining mark (cell 123)

Each is the HD base letter (copied straight from the PNG, so it keeps the
pack's exact style and resolution) plus a diacritic drawn to match the font's
ink, shadow colour, and drop-shadow offset. Placement is measured from each
HD letter's own ink bounding box, so it adapts to any pack's metrics/scale.

The atlas is the retail EUR layout: a 21x6 grid of 12x16 cells over a 256x96
texture, uniformly scaled. Cells 90/91/101/109 (blank accent slots) and
120-123 (past the retail 120-glyph count, still inside the grid) take the new
forms — matching s_PolishChars / s_FontLayout_EUR_PL in font_region.c.

Usage:
  python add_polish_glyphs.py FONT16.png                # writes in place (backs up .orig)
  python add_polish_glyphs.py in.png -o out.png         # explicit output
"""
import argparse
import math
import os
import shutil
import sys

from PIL import Image, ImageDraw
try:
    import numpy as np
except ImportError:
    sys.exit("needs numpy + pillow: pip install numpy pillow")

GRID_COLS, GRID_ROWS = 21, 6
ATLAS_W, ATLAS_H = 256, 96      # native EUR FONT16 texture (grid is 21*12 x 6*16)
CELL_NX, CELL_NY = 12, 16       # native cell size

# Base letter per built cell (font_region.c s_PolishGlyphs), by atlas cell id.
# -1 base = a standalone combining mark (the Z-dot), no letter copied.
CELL = {'a': 58, 'e': 62, 'l': 69, 'z': 83, 'A': 26, 'E': 30, 'L': 37}
BUILD = [
    (90,  CELL['a'], 'ogonek'),
    (91,  CELL['e'], 'ogonek'),
    (109, CELL['z'], 'dot_above'),
    (101, CELL['l'], 'stroke'),
    (120, CELL['A'], 'ogonek'),
    (121, CELL['E'], 'ogonek'),
    (122, CELL['L'], 'stroke'),
    (123, -1,        'dot_mark'),
]

SS = 4  # supersample for anti-aliased diacritics


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("png", help="HD FONT16.png to add Polish glyphs to")
    ap.add_argument("-o", "--out", help="output path (default: overwrite, keeping a .orig backup)")
    args = ap.parse_args()

    im = Image.open(args.png).convert("RGBA")
    W, H = im.size
    cw, ch = W / GRID_COLS if False else W // (ATLAS_W // CELL_NX), H // (ATLAS_H // CELL_NY)
    # cw/ch = HD cell size. ATLAS_W//CELL_NX = 21 columns exist across ATLAS_W,
    # but the PNG scales the full 256-wide texture, so a cell is (W/256)*12 wide.
    scale_x, scale_y = W / ATLAS_W, H / ATLAS_H
    cw, ch = round(CELL_NX * scale_x), round(CELL_NY * scale_y)
    if abs(scale_x - round(scale_x)) > 0.02 and abs(scale_x * CELL_NX - cw) > 1:
        print("warning: unusual width %d (expected a multiple of %d)" % (W, ATLAS_W))
    print("atlas %dx%d, scale %.2fx%.2f, cell %dx%d" % (W, H, scale_x, scale_y, cw, ch))

    A = np.array(im)

    def cell_xy(n):
        return (n % GRID_COLS) * cw, (n // GRID_COLS) * ch

    def cell(n):
        x, y = cell_xy(n)
        return A[y:y + ch, x:x + cw]

    # Ink / shadow colour and the drop-shadow offset, sampled from a base letter.
    probe = cell(CELL['l'])
    opaque = probe[probe[:, :, 3] > 128]
    lum = opaque[:, :3].mean(1)
    ink = tuple(int(v) for v in opaque[lum > lum.max() * 0.7][:, :3].mean(0))
    sha = tuple(int(v) for v in opaque[lum < lum.min() + (lum.max() - lum.min()) * 0.3][:, :3].mean(0))
    sdx, sdy = max(1, round(scale_x)), max(1, round(scale_y))  # native 1px shadow

    def ink_bbox(n):
        c = cell(n)
        l = c[:, :, :3].mean(2)
        m = (c[:, :, 3] > 128) & (l > 90)
        ys, xs = np.where(m)
        if len(xs) == 0:
            return (0, 0, cw - 1, ch - 1)
        return int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max())

    def ss_shape(drawfn):
        big = Image.new("L", (cw * SS, ch * SS), 0)
        drawfn(ImageDraw.Draw(big))
        return np.array(big.resize((cw, ch), Image.LANCZOS)).astype(np.float32) / 255.0

    def stamp(n, cov):
        """Composite a coverage mask onto cell n: shadow (offset) then ink."""
        x, y = cell_xy(n)
        base = A[y:y + ch, x:x + cw].astype(np.float32)
        for col, (ox, oy) in ((sha, (sdx, sdy)), (ink, (0, 0))):
            m = np.zeros((ch, cw), np.float32)
            m[oy:, ox:] = cov[:ch - oy or ch, :cw - ox or cw] if (ox or oy) else cov
            if not (ox or oy):
                m = cov
            for k in range(3):
                base[:, :, k] = base[:, :, k] * (1 - m) + col[k] * m
            base[:, :, 3] = np.maximum(base[:, :, 3], m * 255)
        A[y:y + ch, x:x + cw] = np.clip(base, 0, 255).astype(np.uint8)

    def copy_base(dst, src):
        sx, sy = cell_xy(src)
        dx, dy = cell_xy(dst)
        A[dy:dy + ch, dx:dx + cw] = A[sy:sy + ch, sx:sx + cw]

    # --- diacritic coverage generators (coords in HD cell pixels) ---
    def ogonek(cx, baseline):
        # dimensions in NATIVE cell units * scale (a hook ~1.3x1.5 native px).
        w = 0.55 * scale_x
        h = 1.5 * scale_y
        r = 1.3 * scale_x

        def d(dr):
            pts = []
            for t in range(101):
                u = t / 100.0
                ang = math.pi * (-0.15 + 1.25 * u)
                pts.append(((cx + r * math.sin(ang) * (1 - 0.35 * u)) * SS,
                            (baseline + h * u) * SS))
            dr.line(pts, fill=255, width=int(w * SS), joint="curve")
        return ss_shape(d)

    def dot(cx, cy):
        rad = 1.0 * scale_x

        def d(dr):
            dr.ellipse([(cx - rad) * SS, (cy - rad) * SS, (cx + rad) * SS, (cy + rad) * SS], fill=255)
        return ss_shape(d)

    def stroke(x0, y0, x1, y1):
        w = 0.9 * scale_x

        def d(dr):
            dr.line([(x0 * SS, y0 * SS), (x1 * SS, y1 * SS)], fill=255, width=int(w * SS))
        return ss_shape(d)

    for dst, src, kind in BUILD:
        if src >= 0:
            copy_base(dst, src)
        x0, y0, x1, y1 = ink_bbox(src) if src >= 0 else (0, 0, cw, ch)

        if kind == "ogonek":
            # under the letter's right side, hanging below the baseline
            stamp(dst, ogonek(cx=x0 + (x1 - x0) * 0.62, baseline=y1 - 1 * scale_y))
        elif kind == "dot_above":
            stamp(dst, dot(cx=(x0 + x1) / 2.0, cy=y0 - 1.8 * scale_y))
        elif kind == "dot_mark":
            # standalone combining dot near the cell top (drawn 3px above a capital)
            stamp(dst, dot(cx=4.5 * scale_x, cy=1.5 * scale_y))
        elif kind == "stroke":
            # diagonal bar crossing the stem at mid x-height
            midx = x0 + (x1 - x0) * 0.5
            cy = (y0 + y1) / 2.0
            stamp(dst, stroke(x0 - 1 * scale_x, cy + 4 * scale_y,
                              midx + 3 * scale_x, cy - 4 * scale_y))

    out = args.out
    if not out:
        backup = args.png + ".orig"
        if not os.path.exists(backup):
            shutil.copy2(args.png, backup)  # byte copy: keep the exact original
            print("backed up original -> %s" % backup)
        out = args.png
    Image.fromarray(A).save(out)
    print("wrote %s (added %d Polish glyphs)" % (out, len(BUILD)))


if __name__ == "__main__":
    main()
