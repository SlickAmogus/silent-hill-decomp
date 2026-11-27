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
OUT = 0.2           # clearance above the face: the patches are the face's own triangles, so a hair is enough
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
    'D': (0.84, 0.42, 0.35, 0.09, 0.15, 1, 0, 1, 0.08), # AA: wide open
    'E': (0.50, 0.28, 0.8, 0.13, 0.17, 0, 0, 1, 0.0),   # AO, ER: rounded
    'F': (0.30, 0.19, 1.0, 0.16, 0.19, 0, 0, 0, 0.0),   # UW, OW, W: pursed
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
        d.polygon(up_in + lo_in[::-1], fill=_mix(dark, lips, 0.4))
        d.polygon(_outline(X, Y, w * 0.8, h * 0.75, rnd, True) + _outline(X, Y + A * drop, w * 0.8, h * 0.75, rnd, False)[::-1],
                  fill=_mix(dark, lips, 0.15))
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


def front_cell(prims, imgcache, ray, rect, dens):
    """The face inside rect (head frame x0, x1, y0, y1) seen front-on, texel by
    texel (a crop by UV folded over where these faces mirror their texture at
    the middle, and left chips of the wrong side on the patch)."""
    X0, X1, Y0, Y1 = rect
    cw, ch = max(8, int(round((X1 - X0) * dens))), max(8, int(round((Y1 - Y0) * dens)))
    base = Image.new('RGBA', (cw, ch))
    bp = base.load()
    for j in range(ch):
        y = Y1 - (j + 0.5) / ch * (Y1 - Y0)
        for i in range(cw):
            h = ray(X0 + (i + 0.5) / cw * (X1 - X0), y)
            if h is None:
                continue
            src = imgcache[prims[h[1]]['row']]
            bp[i, j] = src.getpixel((min(src.width - 1, int(h[2][0] * src.width)), min(src.height - 1, int(h[2][1] * src.height))))
    return base


def conform(prims, tris, ray, rect):
    """The head's own front triangles cut to rect: polygons of head-frame points,
    counter-clockwise seen from in front. A patch made of these lies on the face
    exactly, so it needs only a hair of clearance; a flat grid floated far enough
    out to clear the lips between its points, and from the side it slid off what
    it was meant to cover."""
    X0, X1, Y0, Y1 = rect
    out = []
    for pi, a, b, c in tris:
        L = prims[pi]['L']
        # each point carries the face's own normal there (world frame, as the model stores it):
        # lit by one forward normal, a patch came out a different shade from the skin round it
        # wherever the light has a direction (the flashlight)
        NW = prims[pi]['N']
        poly = [np.concatenate([np.array(L[k], float), np.array(NW[k], float)]) for k in (a, b, c)]
        cen = (poly[0] + poly[1] + poly[2]) / 3
        if max(q[0] for q in poly) < X0 or min(q[0] for q in poly) > X1 or max(q[1] for q in poly) < Y0 or min(q[1] for q in poly) > Y1:
            continue
        h = ray(cen[0], cen[1])
        if h is None or cen[2] < h[0] - 1.0:
            continue   # the back of the head, or a surface behind the face
        for axis, lim, keep_less in ((0, X0, False), (0, X1, True), (1, Y0, False), (1, Y1, True)):
            nxt = []
            for k in range(len(poly)):
                u, v = poly[k], poly[(k + 1) % len(poly)]
                iu = (u[axis] <= lim) if keep_less else (u[axis] >= lim)
                iv = (v[axis] <= lim) if keep_less else (v[axis] >= lim)
                if iu:
                    nxt.append(u)
                if iu != iv:
                    t = (lim - u[axis]) / (v[axis] - u[axis])
                    nxt.append(u + t * (v - u))
            poly = nxt
            if len(poly) < 3:
                break
        if len(poly) < 3:
            continue
        area = sum(poly[k][0] * poly[(k + 1) % len(poly)][1] - poly[(k + 1) % len(poly)][0] * poly[k][1] for k in range(len(poly)))
        if abs(area) < 1e-6:
            continue
        out.append(poly if area > 0 else poly[::-1])
    return out


def add_patches(prims, imgcache, tris, ray, l2w, l2w_dir, ensure_targets, name, cells, all_keys, jaw_delta=None):
    """One part holding a patch per cell: dict(img, rect, key). Each is folded to
    its rect's centre at rest and laid on the face by its shape key, the cell's
    image mapped over it front-on. Patches step 0.01 apart, so two of one rect
    never fight."""
    W = sum(c['img'].width for c in cells)
    H = max(c['img'].height for c in cells)
    atlas = Image.new('RGBA', (W, H))
    P, N, UV, I, jaw = [], [], [], [], []
    D = {k: [] for k in all_keys}
    x_at = 0
    polys = {}
    for n, c in enumerate(cells):
        X0, X1, Y0, Y1 = c['rect']
        atlas.paste(c['img'], (x_at, 0))
        if c['rect'] not in polys:
            polys[c['rect']] = conform(prims, tris, ray, c['rect'])
        hc = ray((X0 + X1) / 2, (Y0 + Y1) / 2)
        centre = np.array([(X0 + X1) / 2, (Y0 + Y1) / 2, hc[0] + OUT])
        index = {}
        for poly in polys[c['rect']]:
            ids = []
            for q in poly:
                kq = (round(q[0], 4), round(q[1], 4), round(q[2], 4))
                if kq not in index:
                    index[kq] = len(P)
                    op = np.array([q[0], q[1], q[2] + OUT + 0.01 * n])
                    P.append(centre)
                    # at its own length: these are the console's scaled normals, and the
                    # light a vertex takes goes by that length
                    N.append(np.array(q[3:6], float))
                    tx = x_at + (q[0] - X0) / (X1 - X0) * c['img'].width
                    ty = (Y1 - q[1]) / (Y1 - Y0) * c['img'].height
                    UV.append((tx / W, ty / H))
                    for k in all_keys:
                        D[k].append(op - centre if k == c['key'] else np.zeros(3))
                    jaw.append(jaw_delta(q[0], q[1]) if jaw_delta else np.zeros(3))
                ids.append(index[kq])
            for k in range(1, len(ids) - 1):
                I += [ids[0], ids[k], ids[k + 1]]
        x_at += c['img'].width
    if not P:
        return False
    imgcache[name] = atlas
    pr = dict(mat=name, row=name, W=W, H=H, P=l2w(np.array(P)), N=np.array(N), UV=np.array(UV),
              J=[2] * len(P), I=I, SRC=[], PART=[])
    # No head-frame copy: the chin drop that follows reads only the head's own vertices.
    pr['L'] = np.zeros((0, 3))
    ensure_targets(pr)
    for k in all_keys:
        pr['D'][k] = l2w_dir(np.array(D[k]))
    if jaw_delta:
        pr['D']['jawOpen'] = np.array(jaw)
    prims.append(pr)
    return True


def _density(prims, imgcache, ray, x, y, hw):
    """Atlas pixels per head unit at (x, y), from the texels hw either side."""
    a, b = ray(x - hw, y), ray(x + hw, y)
    w = imgcache[prims[a[1]]['row']].width
    return abs(b[2][0] - a[2][0]) * w / (2 * hw) * SCALE


def build_eyes(prims, fp, imgcache, tris, ray, l2w, l2w_dir, ensure_targets):
    """Blink as two painted frames per eye: lidhalf (the lid halfway down) and
    blink (shut). Each is the face round the eye with the eye painted over in the
    socket's own shading and a lash line, on a patch that lies on the face. A
    lid strip sliding down in front of the face read as a pale shape floating
    off the eye from any angle but straight on."""
    cells = []
    for cx, cy, hw, hh in fp.get('eyes', []):
        rect = (cx - hw * 1.9, cx + hw * 1.9, cy - hh * 3.0, cy + hh * 3.0)
        if any(ray(x, y) is None for x in (rect[0], rect[1]) for y in (rect[2], rect[3])) or ray(cx, cy) is None:
            print('  eye patch off the head surface, skipped'); continue
        dens = _density(prims, imgcache, ray, cx, cy, hw)
        base = front_cell(prims, imgcache, ray, rect, dens)
        px, py = (cx - rect[0]) * dens, (rect[3] - cy) * dens
        a, b = hw * dens * 1.2, hh * dens * 1.25
        # Where the eye really is: the parameters are a point or two off on some
        # faces (Cybil's left iris sat below and beside the painted-over area).
        # The eye is what is not skin-coloured here: skin is well redder than
        # blue, an iris, the white and the lash line are not.
        arr = np.asarray(base.convert('RGB')).astype(float)
        rb = arr[..., 0] - arr[..., 2]
        yy, xx = np.mgrid[0:base.height, 0:base.width]
        win = (np.abs(xx + 0.5 - px) < hw * dens * 1.35) & (np.abs(yy + 0.5 - py) < hh * dens * 1.7)
        if win.sum() > 8:
            wgt = np.clip(0.45 * np.median(rb) - rb, 0, None) * win
            if wgt.sum() > 0:
                px = float((wgt * (xx + 0.5)).sum() / wgt.sum())
                py = float((wgt * (yy + 0.5)).sum() / wgt.sum())
                sx = np.sqrt((wgt * (xx + 0.5 - px) ** 2).sum() / wgt.sum())
                sy = np.sqrt((wgt * (yy + 0.5 - py) ** 2).sum() / wgt.sum())
                a = float(np.clip(2.4 * sx, hw * dens * 1.0, hw * dens * 1.5))
                b = float(np.clip(2.4 * sy + 1.0, hh * dens * 1.1, hh * dens * 1.9))
        b = min(b, py - 2, base.height - 3 - py)
        dark = min((base.getpixel((int(x), int(y))) for x in np.arange(px - a, px + a) for y in np.arange(py - b, py + b)
                    if 0 <= x < base.width and 0 <= y < base.height), key=lambda c: c[0] + c[1] + c[2])
        shut = base.copy()
        erase(shut, px, py, a, b)
        # the lash line: the eye's darkest, softened toward the lid it lies on
        lid = shut.getpixel((int(px), int(py)))
        dark = tuple(int(dark[k] * 0.7 + lid[k] * 0.3) for k in range(3)) + (255,)
        half = base.copy()
        hp, sp = half.load(), shut.load()
        for y in range(0, min(half.height, int(py) + 1)):
            for x in range(half.width):
                hp[x, y] = sp[x, y]
        for img, sag in ((half, 0.0), (shut, 0.3)):
            d = ImageDraw.Draw(img)
            pts = [(px + u * a * 0.85, py + sag * b * (1 - u * u)) for u in np.linspace(-1, 1, 24)]
            d.line(pts, fill=dark, width=1)
        cells.append(dict(img=half, rect=rect, key='lidhalf'))
        cells.append(dict(img=shut, rect=rect, key='blink'))
    if cells and add_patches(prims, imgcache, tris, ray, l2w, l2w_dir, ensure_targets, 'eyeshapes', cells, ['lidhalf', 'blink']):
        return ['lidhalf', 'blink']
    return []


def build(prims, fp, imgcache, tris, ray, l2w, l2w_dir, jaw_delta, ensure_targets):
    mx, my, mw = fp['mouth']
    # As large as the face allows round the mouth; only the part below it gives
    # way (a chin close under the mouth, Cybil's, leaves less room there).
    for f in (1.0, 0.9, 0.8, 0.7, 0.6, 0.5):
        rect = (mx - mw * 1.6, mx + mw * 1.6, my - mw * 1.1 * f, my + mw * 0.8)
        if all(ray(x, y) is not None for x in np.linspace(rect[0], rect[1], COLS) for y in np.linspace(rect[2], rect[3], ROWS)):
            break
    else:
        print('  no room for a mouth patch on the face, no shapes')
        return []
    hit = ray(mx, my)
    im = imgcache[prims[hit[1]]['row']]
    dens = _density(prims, imgcache, ray, mx, my, mw)
    c = hit[2] * [im.width, im.height]
    cols = colours(im, c[0], c[1], dens / SCALE * mw)
    base = front_cell(prims, imgcache, ray, rect, dens)
    ccx, ccy = (mx - rect[0]) * dens, (rect[3] - my) * dens
    a = mw * dens
    cells = []
    for s in SHAPES:
        cell = base.copy()
        if s != 'X':
            erase(cell, ccx, ccy + a * 0.1, a * 1.45, a * 0.6)
            draw_shape(cell, s, ccx, ccy, a, cols)
        cells.append(dict(img=cell, rect=rect, key='mouth_' + s))
    keys = ['mouth_' + s for s in SHAPES]
    if not add_patches(prims, imgcache, tris, ray, l2w, l2w_dir, ensure_targets, 'mouthshapes', cells, keys, jaw_delta):
        return []
    print('  mouth shapes: %d cells of %d x %d, lips %s mouth %s teeth %s' % (len(cells), base.width, base.height, cols[0][:3], cols[1][:3], cols[2][:3]))
    return keys
