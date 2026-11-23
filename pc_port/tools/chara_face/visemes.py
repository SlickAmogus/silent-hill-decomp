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


# Per shape (Rhubarb's A-H): opening half width and half height (of the mouth's
# half width), roundness (0 = a lens with pointed corners, 1 = an oval), upper
# and lower lip thickness, upper teeth, lower teeth, tongue (0 none, 1 low, 2 raised
# to the teeth), and how far the lower lip drops below the opening.
POSE = {
    'A': (0.92, 0.0, 0.0, 0.10, 0.13, 0, 0, 0, 0.0),    # M B P: lips pressed together
    'B': (0.82, 0.10, 0.1, 0.10, 0.14, 1, 1, 0, 0.0),   # most consonants: teeth together, lips apart
    'C': (0.84, 0.26, 0.2, 0.10, 0.15, 1, 0, 1, 0.0),   # EH, AE
    'D': (0.86, 0.50, 0.35, 0.09, 0.15, 1, 0, 1, 0.08), # AA: wide open
    'E': (0.58, 0.38, 0.8, 0.13, 0.17, 0, 0, 1, 0.0),   # AO, ER: rounded
    'F': (0.34, 0.25, 1.0, 0.16, 0.19, 0, 0, 0, 0.0),   # UW, OW, W: pursed
    'G': (0.80, 0.11, 0.1, 0.10, 0.10, 1, 0, 0, -0.06), # F V: upper teeth on the tucked lower lip
    'H': (0.80, 0.30, 0.3, 0.10, 0.15, 1, 0, 2, 0.0),   # L: tongue up behind the teeth
}
SS = 4   # drawn this much finer, then filtered down: soft edges that sit into the face


def _mix(c, d, t):
    return tuple(int(c[k] * (1 - t) + d[k] * t) for k in range(3)) + (255,)


def _profile(x, w, h, rnd):
    """Half height of the opening at x: a lens (pointed corners) for rnd 0, an
    oval for 1."""
    u = min(1.0, abs(x) / w)
    lens = 1 - u * u
    oval = (1 - u * u) ** 0.5
    return h * (lens * (1 - rnd) + oval * rnd)


def _outline(cx, cy, w, h, rnd, top, n=48):
    xs = np.linspace(-w, w, n)
    return [(cx + x, cy + (-1 if top else 1) * _profile(x, w, h, rnd)) for x in xs]


def draw_shape(cell, s, cx, cy, a, cols):
    """One shape over the erased mouth, centred (cx, cy), the mouth's half width a
    (pixels): upper and lower lip in shades of the face's own lip colour, the
    opening dark and deeper inside, teeth and tongue where the sound shows them."""
    lips, dark, teeth, tongue = cols
    W, H = cell.size
    lay = Image.new('RGBA', (W * SS, H * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(lay)
    X, Y, A = cx * SS, cy * SS, a * SS
    ow, oh, rnd, ut_l, lt_l, ut, lt, tg, drop = POSE[s]
    w, h = A * ow, max(A * oh, SS * 0.6)
    # The painted lips are near the skin's colour on these faces: a little more
    # of a lip's own red, or the closed shapes all but vanish.
    lips = _mix(lips, (150, 72, 66), 0.3)
    upper = _mix(lips, (0, 0, 0), 0.22)
    lower = _mix(lips, (255, 235, 225), 0.06)
    shine = _mix(lips, (255, 240, 230), 0.35)
    # Lips: the upper one dips in a cupid's bow, the lower one fuller and lit.
    ul, ll = A * ut_l, A * lt_l
    up_in = _outline(X, Y, w, h, rnd, True)
    up_out = [(x, y - ul * (1 - 0.35 * np.exp(-((x - X) / (0.18 * A)) ** 2)) * (0.55 + 0.45 * (1 - (abs(x - X) / w) ** 2)))
              for x, y in up_in]
    lo_in = _outline(X, Y + A * drop, w, h, rnd, False)
    lo_out = [(x, y + ll * (0.5 + 0.5 * (1 - (abs(x - X) / w) ** 2))) for x, y in lo_in]
    d.polygon(up_out + up_in[::-1], fill=upper)
    d.polygon(lo_in + lo_out[::-1], fill=lower)
    # a soft highlight along the lower lip
    hi = [(x, y + ll * 0.35) for x, y in lo_in if abs(x - X) < w * 0.45]
    if len(hi) > 1:
        d.line(hi, fill=shine, width=max(1, int(ll * 0.3)))
    if s == 'A':
        d.line([(x, Y) for x, _ in up_in], fill=_mix(dark, (0, 0, 0), 0.2), width=max(1, int(SS * 1.1)))
    else:
        # The opening, darker toward the middle (deeper in the mouth).
        d.polygon(up_in + lo_in[::-1], fill=_mix(dark, lips, 0.25))
        d.polygon(_outline(X, Y, w * 0.8, h * 0.75, rnd, True) + _outline(X, Y + A * drop, w * 0.8, h * 0.75, rnd, False)[::-1],
                  fill=dark)
        if tg:
            ty = Y + h * 0.35 if tg == 1 else Y - h * 0.45
            tw, th = w * 0.42, h * (0.32 if tg == 1 else 0.28)
            d.ellipse([X - tw, ty - th, X + tw, ty + th], fill=_mix(tongue, dark, 0.45))
        tooth_h = max(SS * 1.2, min(h * 0.6, A * 0.16))
        if ut:
            band = [(x, y) for x, y in up_in if abs(x - X) < w * 0.72]
            d.polygon(band + [(x, y + tooth_h) for x, y in band[::-1]], fill=teeth)
            d.line([(x, y + tooth_h) for x, y in band], fill=_mix(teeth, dark, 0.35), width=max(1, SS // 2))
        if lt:
            band = [(x, y) for x, y in lo_in if abs(x - X) < w * 0.65]
            d.polygon(band + [(x, y - tooth_h * 0.8) for x, y in band[::-1]], fill=_mix(teeth, dark, 0.12))
        # the lips' own shadow on the opening's top edge
        d.line(up_in, fill=_mix(dark, (0, 0, 0), 0.3), width=max(1, SS // 2))
    small = lay.resize((W, H), Image.LANCZOS)
    cell.alpha_composite(small)


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
