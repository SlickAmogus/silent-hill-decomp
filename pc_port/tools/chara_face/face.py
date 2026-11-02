"""Face pass for npc2glb: blink and jawOpen for a PSX head whose eyes and mouth
are painted on. Works in the head bone's frame (OBJ axis convention: +y up,
+z out of the face). Each eye gets a lid strip (cheek-coloured) flat on its
top edge at rest, drawn down over the eye by `blink`; the mouth gets a dark
lens flat on the mouth line, opened by `jawOpen`, which also drops the chin.
params: eyes [[cx, cy, hw, hh], ...], mouth [mx, my, mw], open (units)."""
import numpy as np

LID_COLS = 7
MOUTH_COLS = 8
OUT = 0.35   # how far in front of the face the added strips float


def build(prims, fp, imgcache, tex_for, T, RR):
    R2, T2 = RR[2], T[2]
    R2i = np.linalg.inv(R2)

    def w2l(P):
        g = np.c_[P[:, 0], -P[:, 1], P[:, 2]] - T2
        l = g @ R2i.T
        return np.c_[l[:, 0], -l[:, 1], l[:, 2]]

    def l2w(L):
        g = np.c_[L[:, 0], -L[:, 1], L[:, 2]] @ R2.T + T2
        return np.c_[g[:, 0], -g[:, 1], g[:, 2]]

    def l2w_dir(D):
        g = np.c_[D[:, 0], -D[:, 1], D[:, 2]] @ R2.T
        return np.c_[g[:, 0], -g[:, 1], g[:, 2]]

    # Head triangles in the head frame, for the surface rays.
    tris = []
    for pi, pr in enumerate(prims):
        L = w2l(pr['P'])
        pr['L'] = L
        I = pr['I']
        for t in range(0, len(I), 3):
            a, b, c = I[t:t + 3]
            if pr['J'][a] == 2 and pr['J'][b] == 2 and pr['J'][c] == 2:
                tris.append((pi, a, b, c))

    def ray(x, y):
        """Front-most head surface at (x, y): z, prim, uv, normal (head frame)."""
        best = None
        for pi, a, b, c in tris:
            L = prims[pi]['L']
            A, B, C = L[a], L[b], L[c]
            d = (B[1] - C[1]) * (A[0] - C[0]) + (C[0] - B[0]) * (A[1] - C[1])
            if abs(d) < 1e-9:
                continue
            u = ((B[1] - C[1]) * (x - C[0]) + (C[0] - B[0]) * (y - C[1])) / d
            v = ((C[1] - A[1]) * (x - C[0]) + (A[0] - C[0]) * (y - C[1])) / d
            w = 1 - u - v
            if u < -1e-4 or v < -1e-4 or w < -1e-4:
                continue
            z = u * A[2] + v * B[2] + w * C[2]
            if best is None or z > best[0]:
                UV = prims[pi]['UV']
                uv = u * UV[a] + v * UV[b] + w * UV[c]
                n = np.cross(B - A, C - A); n /= (np.linalg.norm(n) or 1)
                if n[2] < 0:
                    n = -n
                best = (z, pi, uv, n)
        return best

    def ensure_targets(pr):
        if 'D' not in pr:
            pr['D'] = {'blink': np.zeros((len(pr['P']), 3)), 'jawOpen': np.zeros((len(pr['P']), 3))}

    def add_strip(xs, top, bot_rest, bot_open, uvsrc, key, uv_rows=None):
        """A two-row strip: top row fixed, bottom row at bot_rest, keyed to bot_open.
        uv_rows: per-column UVs for the top and bottom rows, else uvsrc's one UV."""
        hits_t = [ray(x, y) for x, y in zip(xs, top)]
        hits_b = [ray(x, y) for x, y in zip(xs, bot_open)]
        if any(h is None for h in hits_t + hits_b):
            print('  strip off the head surface, skipped (%s)' % key)
            return
        pi = uvsrc[0]
        pr = prims[pi]
        ensure_targets(pr)
        base = len(pr['P'])
        Lt = np.array([[x, y, h[0] + OUT] for x, y, h in zip(xs, top, hits_t)])
        Lb_rest = np.array([[x, y, h[0] + OUT] for x, y, h in zip(xs, bot_rest, hits_t)])
        Lb_open = np.array([[x, y, h[0] + OUT] for x, y, h in zip(xs, bot_open, hits_b)])
        Nt = np.array([h[3] for h in hits_t]); Nb = np.array([h[3] for h in hits_b])
        newP = l2w(np.vstack([Lt, Lb_rest]))
        newN = l2w_dir(np.vstack([Nt, Nb]))
        uvs = np.vstack(uv_rows) if uv_rows is not None else np.array([uvsrc[1]] * (2 * len(xs)))
        pr['P'] = np.vstack([pr['P'], newP]); pr['N'] = np.vstack([pr['N'], newN]); pr['UV'] = np.vstack([pr['UV'], uvs])
        pr['L'] = np.vstack([pr['L'], np.vstack([Lt, Lb_rest])])
        pr['J'] += [2] * (2 * len(xs))
        delta = np.zeros((2 * len(xs), 3))
        delta[len(xs):] = l2w_dir(Lb_open - Lb_rest)
        for k in pr['D']:
            pr['D'][k] = np.vstack([pr['D'][k], delta if k == key else np.zeros_like(delta)])
        n = len(xs)
        for i in range(n - 1):
            t0, t1, b0, b1 = base + i, base + i + 1, base + n + i, base + n + i + 1
            # Counter-clockwise seen from in front of the face.
            pr['I'] += [t0, b0, b1, t0, b1, t1]

    def uv_at(x, y, darkest=False):
        h = ray(x, y)
        if h is None:
            return None
        pi, uv = h[1], h[2]
        if darkest:
            im = imgcache[prims[pi]['row']]
            W, H = im.size
            px, py = int(uv[0] * W), int(uv[1] * H)
            best = (1e9, uv)
            for dy in range(-3, 4):
                for dx in range(-6, 7):
                    qx, qy = min(max(px + dx, 0), W - 1), min(max(py + dy, 0), H - 1)
                    r, g, b, a = im.getpixel((qx, qy))
                    if a > 128 and r + g + b < best[0]:
                        best = (r + g + b, np.array([(qx + 0.5) / W, (qy + 0.5) / H]))
            uv = best[1]
        return (pi, uv)

    keys = []
    for cx, cy, hw, hh in fp.get('eyes', []):
        xs = np.linspace(cx - hw * 1.15, cx + hw * 1.15, LID_COLS)
        t = (xs - (cx - hw * 1.15)) / (2.3 * hw)
        f = np.sqrt(np.clip(1 - (2 * t - 1) ** 2, 0, 1))
        top = cy + hh * (0.35 + 0.75 * f)
        bot = cy - hh * (0.35 + 0.85 * f)
        src = uv_at(cx, cy - hh * 2.2)
        if src is None:
            print('  no cheek under the eye at', cx, cy); continue
        # The lid takes the band of skin just under the eye, so it shades like
        # the face round it without the painted eye inside. Above the eye is
        # brow on most of these faces.
        rows = []
        for ys in (bot - hh * 0.7, bot - hh * 1.7):
            row = []
            for x, y in zip(xs, ys):
                h = uv_at(x, y)
                row.append(h[1] if h is not None and h[0] == src[0] else src[1])
            rows.append(np.array(row))
        add_strip(xs, top, top, bot, src, 'blink', rows)
        keys.append('blink')
    if fp.get('mouth'):
        mx, my, mw = fp['mouth']
        A = fp.get('open', 1.6)
        xs = np.linspace(mx - mw, mx + mw, MOUTH_COLS)
        t = (xs - (mx - mw)) / (2 * mw)
        f = np.sin(np.pi * t)
        top = np.full_like(xs, my + 0.25)
        src = uv_at(mx, my, darkest=True)
        if src is not None:
            add_strip(xs, top, top, my + 0.25 - A * f, src, 'jawOpen')
            keys.append('jawOpen')
        # The chin drops with it: head verts below the mouth line, in front.
        zc = np.median(np.concatenate([pr['L'][[j for j in range(len(pr['L'])) if pr['J'][j] == 2], 2]
                                       for pr in prims if 2 in pr['J']]))
        # Moved by position, so the neck's copy of a seam vertex goes with the chin.
        moved = {}
        for pr in prims:
            for j, (x, y, z) in enumerate(pr['L']):
                if pr['J'][j] != 2 or j >= len(pr['SRC']) or pr['PART'][j] != '02HEAD1':
                    continue
                if y >= my or z < zc or abs(x - mx) > mw * 1.9:
                    continue
                wy = np.clip((my - y) / 1.5, 0, 1); wy = wy * wy * (3 - 2 * wy)
                wx = np.clip(1 - (abs(x - mx) / (mw * 1.9)) ** 2, 0, 1)
                wf = np.clip(1 - (my - y - 6) / 10, 0, 1)
                moved[tuple(np.round(pr['P'][j], 3))] = l2w_dir(np.array([[0, -A * 0.8 * wy * wx * wf, 0]]))[0]
        for pr in prims:
            ensure_targets(pr)
            for j in range(len(pr['SRC'])):
                if pr['J'][j] == 2:
                    d = moved.get(tuple(np.round(pr['P'][j], 3)))
                    if d is not None:
                        pr['D']['jawOpen'][j] = d
    for pr in prims:
        ensure_targets(pr)
        del pr['L']
    return sorted(set(keys), key=['blink', 'jawOpen'].index)
