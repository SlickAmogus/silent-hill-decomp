"""Painted mouth shapes for a face pass (face.py): one patch over the mouth per
Rhubarb Lip Sync shape, A..H and X, each keyed open by its own shape key
mouth_<shape> and folded to a point at rest, so the face is untouched until a
line plays.

Each patch's texture is the face itself round the mouth, seen front-on (so
its edges match the face exactly), with the painted mouth blended away and the
shape drawn in the face's own lip, mouth and teeth colours; X is the copy as it
is. All nine sit in one atlas image added to the model.
"""
import numpy as np
from PIL import Image, ImageDraw

SHAPES = 'ABCDEFGHX'
COLS, ROWS = 7, 5
OUT = 0.6           # in front of the face (nearer, the lips bulged through between the grid points);
                    # the shapes step 0.01 apart so a crossfade does not fight
SCALE = 2           # atlas pixels per face texel: finer looked drawn on, next to the face's texels


def colours(im, cx, cy, hw):
    """Lip, mouth and teeth colours from the painted mouth: the lips are its
    middle tones, the inside its darkest, the teeth a pale skin."""
    px = []
    for y in range(int(cy - hw * 0.5), int(cy + hw * 0.5) + 1):
        for x in range(int(cx - hw), int(cx + hw) + 1):
            if 0 <= x < im.width and 0 <= y < im.height:
                r, g, b, a = im.getpixel((x, y))
                if a > 128:
                    px.append((r, g, b))
    px = np.array(px, float)
    lum = px.sum(1)
    order = np.argsort(lum)
    dark = px[order[:max(1, len(px) // 10)]].mean(0) * 0.8
    lips = px[order[len(px) // 3: 2 * len(px) // 3]].mean(0)
    skin = px[order[-max(1, len(px) // 4):]].mean(0)
    teeth = skin * 0.6 + np.array([200.0, 196, 184]) * 0.4
    tongue = dark * 0.4 + np.array([150.0, 70, 70]) * 0.6
    c = lambda v: tuple(int(max(0, min(255, x))) for x in v) + (255,)
    return c(lips), c(dark), c(teeth), c(tongue)


def erase(cell, cx, cy, a, b):
    """Blend the painted mouth away: each column inside the ellipse takes the
    colours just above and below it, graded across."""
    px = cell.load()
    W, H = cell.size
    for x in range(max(0, int(cx - a)), min(W, int(cx + a) + 1)):
        t = 1 - ((x - cx) / a) ** 2
        if t <= 0:
            continue
        h = b * np.sqrt(t)
        y0, y1 = int(cy - h) - 1, int(cy + h) + 1
        if y0 < 0 or y1 >= H:
            continue
        top, bot = np.array(px[x, y0], float), np.array(px[x, y1], float)
        for y in range(y0 + 1, y1):
            f = (y - y0) / float(y1 - y0)
            px[x, y] = tuple(int(v) for v in top * (1 - f) + bot * f)


def draw_shape(cell, s, cx, cy, a, cols):
    """One shape on an erased cell, centred (cx, cy), half width a (pixels)."""
    lips, dark, teeth, tongue = cols
    d = ImageDraw.Draw(cell)

    def ell(w, h, col, dy=0.0):
        d.ellipse([cx - w, cy - h + dy, cx + w, cy + h + dy], fill=col)
    lw = max(1.0, a * 0.09)    # lip thickness
    if s == 'A':               # M, B, P: lips pressed together
        ell(a * 0.95, lw * 1.6, lips)
        d.line([cx - a * 0.9, cy, cx + a * 0.9, cy], fill=dark, width=max(1, int(lw * 0.6)))
        return
    # opening: half width, half height, upper teeth, lower teeth, tongue
    spec = {'B': (0.85, 0.16, 1, 1, 0), 'C': (0.85, 0.32, 1, 0, 0), 'D': (0.9, 0.55, 1, 0, 1),
            'E': (0.55, 0.42, 0, 0, 0), 'F': (0.32, 0.26, 0, 0, 0), 'G': (0.8, 0.14, 1, 0, 0),
            'H': (0.8, 0.4, 1, 0, 1)}[s]
    ow, oh, ut, lt, tg = spec
    ell(a * ow + lw, a * oh + lw * (1.4 if s in 'EF' else 1.0), lips)
    ell(a * ow, a * oh, dark)
    if tg:
        ell(a * ow * 0.7, a * oh * 0.45, tongue, dy=a * oh * 0.45 if s == 'D' else -a * oh * 0.1)
    if ut:
        d.rectangle([cx - a * ow * 0.8, cy - a * oh, cx + a * ow * 0.8, cy - a * oh + max(1.0, a * oh * (0.9 if s == 'G' else 0.35))], fill=teeth)
    if lt:
        d.rectangle([cx - a * ow * 0.75, cy + a * oh - max(1.0, a * oh * 0.45), cx + a * ow * 0.75, cy + a * oh], fill=teeth)
    if s == 'G':               # F, V: the upper teeth rest on the lower lip
        d.rectangle([cx - a * ow * 0.8, cy + a * oh * 0.2, cx + a * ow * 0.8, cy + a * oh + lw], fill=lips)


def build(prims, fp, imgcache, ray, l2w, l2w_dir, jaw_delta, ensure_targets):
    mx, my, mw = fp['mouth']
    # As large as the face allows round the mouth: a chin close under it
    # (Cybil's) leaves less room below.
    # Only the part below the mouth gives way: shrunk sideways too (Cybil's),
    # the patch was narrower than her painted mouth and its corners showed.
    for f in (1.0, 0.9, 0.8, 0.7, 0.6, 0.5):
        xs = np.linspace(mx - mw * 1.6, mx + mw * 1.6, COLS)
        ys = np.linspace(my - mw * 1.1 * f, my + mw * 0.8, ROWS)
        hits = [[ray(x, y) for x in xs] for y in ys]
        if any(h is None for row in hits for h in row):
            continue
        pi = hits[ROWS // 2][COLS // 2][1]
        break
    else:
        print('  no room for a mouth patch on the face, no shapes')
        return []
    im = imgcache[prims[pi]['row']]
    W, H = im.size
    # Texel density round the mouth, from its painted width.
    cl, cr = ray(mx - mw, my)[2][0] * W, ray(mx + mw, my)[2][0] * W
    hw_tex = abs(cr - cl) / 2
    c = ray(mx, my)[2] * [W, H]
    cols = colours(im, c[0], c[1], hw_tex)
    # The patch texture is the face itself seen front-on, texel by texel (a
    # crop by UV folded over where these faces mirror their texture at the
    # middle and left chips of the wrong side on the patch).
    dens = hw_tex / mw * SCALE
    X0, X1, Y0, Y1 = xs[0], xs[-1], ys[0], ys[-1]
    cw, ch = max(8, int(round((X1 - X0) * dens))), max(8, int(round((Y1 - Y0) * dens)))
    base = Image.new('RGBA', (cw, ch))
    bp = base.load()
    for j in range(ch):
        y = Y1 - (j + 0.5) / ch * (Y1 - Y0)
        for i in range(cw):
            x = X0 + (i + 0.5) / cw * (X1 - X0)
            h = ray(x, y)
            if h is None:
                continue
            src = imgcache[prims[h[1]]['row']]
            bp[i, j] = src.getpixel((min(src.width - 1, int(h[2][0] * src.width)), min(src.height - 1, int(h[2][1] * src.height))))
    ccx, ccy = (mx - X0) / (X1 - X0) * cw, (Y1 - my) / (Y1 - Y0) * ch
    a = mw / (X1 - X0) * cw
    atlas = Image.new('RGBA', (cw * len(SHAPES), ch))
    for k, s in enumerate(SHAPES):
        cell = base.copy()
        if s != 'X':
            erase(cell, ccx, ccy + a * 0.1, a * 1.45, a * 0.6)
            draw_shape(cell, s, ccx, ccy, a, cols)
        atlas.paste(cell, (k * cw, 0))
    key = 'mouthshapes'
    imgcache[key] = atlas
    # One part: nine grids, each folded to the mouth centre at rest.
    hc = ray(mx, my)
    P, N, UV, I = [], [], [], []
    D = {('mouth_' + s): [] for s in SHAPES}
    jaw = []
    centre = np.array([mx, my, hc[0] + OUT])
    for k, s in enumerate(SHAPES):
        b = len(P)
        for r in range(ROWS):
            for q in range(COLS):
                h = hits[r][q]
                op = np.array([xs[q], ys[r], h[0] + OUT + 0.01 * k])
                P.append(centre)
                N.append(h[3])
                tx = (xs[q] - X0) / (X1 - X0) * cw + k * cw
                ty = (Y1 - ys[r]) / (Y1 - Y0) * ch
                UV.append((tx / atlas.width, ty / atlas.height))
                for s2 in SHAPES:
                    D['mouth_' + s2].append(op - centre if s2 == s else np.zeros(3))
                jaw.append(jaw_delta(xs[q], ys[r]))
        for r in range(ROWS - 1):
            for q in range(COLS - 1):
                t0, t1 = b + r * COLS + q, b + r * COLS + q + 1
                b0, b1 = b + (r + 1) * COLS + q, b + (r + 1) * COLS + q + 1
                # Counter-clockwise from in front: rows run upward here.
                I += [t0, t1, b1, t0, b1, b0]
    pr = dict(mat='mouthshapes', row=key, W=atlas.width, H=atlas.height,
              P=l2w(np.array(P)), N=l2w_dir(np.array(N)), UV=np.array(UV), J=[2] * len(P), I=I, SRC=[], PART=[])
    ensure_targets(pr)
    for s in SHAPES:
        pr['D']['mouth_' + s] = l2w_dir(np.array(D['mouth_' + s]))
    pr['D']['jawOpen'] = np.array(jaw)
    prims.append(pr)
    print('  mouth shapes: %d x %d atlas, lips %s mouth %s teeth %s' % (atlas.width, atlas.height, cols[0][:3], cols[1][:3], cols[2][:3]))
    return ['mouth_' + s for s in SHAPES]
