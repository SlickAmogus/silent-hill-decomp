#!/usr/bin/env python3
"""
clut_tool — author Silent Hill texture mods from ONE image, for EVERY texture class.

A paletted PSX TIM is ONE 4-bit index sheet plus N CLUT rows (palettes). A single
model draws its regions through DIFFERENT rows over that shared sheet (verified:
DOB uses 7 rows across 278 prims, HERO 15, a school chunk sheet up to 14), so no
single `pNN.png` ever looks right on its own — each is the whole sheet tinted by
one palette. Which region uses which row is baked into the model's primitives
(each `s_Primitive.field_2` clut word: `row = (field_2 >> 6) - (s_Material.field_10 >> 6)`),
so the true in-game look is reconstructable offline from the model + the .TIM.

Models live in three containers, all carrying the SAME `s_LmHeader`:

    .ILM   characters/monsters   LM header at offset 0
    .PLM   weapons + BG globals  LM header at offset 0
    .IPD   world geometry chunk  LM header at u32 @ offset 4

One TIM is shared by MANY models — a BG sheet is referenced by up to 118 .IPD
chunks, and for 54 sheets no single chunk uses every palette row the corpus does.
The row map therefore has to be UNIONed over the whole tree, which is what the
`index` + `compose-all` commands do.

Commands:

  index    ROOT                  ->  clut_index.json
      Walks the extracted disc tree once, resolves every model material to its
      .TIM by the MATERIAL NAME (not the file stem), and records the references.

  compose  MODEL-or-TIM          ->  NAME_reference.png
      The correct composite: every texel painted through the palette row the game
      actually draws it with. Edit THIS in any image editor. With `--index` the
      row map is the whole-corpus union; without it, only the given model is used.

  split    edited.png  MODEL-or-TIM  ->  NAME.TIM.pNN.png set
      Slices your edited image back into the per-row loose-override PNGs the
      runtime loads (gamedata/load/<FOLDER>/). Each pNN.png keeps only the texels
      the game samples through row N; the rest is transparent. Accepts the native
      size OR any same-aspect upscale. The loose-PNG path uploads each row as full
      RGBA (no 16-colour re-quantise), so your edit is NOT limited to 16 colours
      per region — paint freely.

  compose-all  --index clut_index.json -o OUTDIR
      Every TIM in the tree, in one pass, with per-class coverage stats.

Usage:
    python3 clut_tool.py index "gamedata/Silent Hill (USA)_extracted" -o clut_index.json
    python3 clut_tool.py compose-all --index clut_index.json -o refs/
    python3 clut_tool.py compose BG/THR0003F.TIM --index clut_index.json
    python3 clut_tool.py split  THR0003F_reference.png BG/THR0003F.TIM --index clut_index.json -o out/
    python3 clut_tool.py compose CHARA/DOB.ILM          # single model, no index
"""

import os
import sys
import json
import zlib
import struct
import fnmatch
import argparse

# Share the exact decoder/PNG-writer with tim2png so both tools stay byte-consistent.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tim2png import write_png_rgba, decode_tim, _u16, _u32, _bgr555  # noqa: E402

# pc_port/include/hires_override.h — the runtime only keeps this many CLUT rows per
# pool slot, so a `pNN.png` with N >= this is never read. Never emit one.
HIRES_POOL_MAX_ROWS = 16

LM_MAGIC = 0x30   # '0'
LM_VERSION = 6
IPD_MAGIC = 20    # IPD_HEADER_MAGIC

INDEX_VERSION = 1
INDEX_DEFAULT_NAME = "clut_index.json"


# ---- minimal PNG reader (stdlib only; 8-bit, non-interlaced) ----------------

def read_png_rgba(path):
    """Read an 8-bit PNG (colour type 0/2/3/6, no interlace) -> (rgba, w, h)."""
    d = open(path, "rb").read()
    if d[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    p = 8
    w = h = bitd = ctype = interlace = None
    idat = bytearray()
    plte = None
    trns = None
    while p < len(d):
        ln = struct.unpack_from(">I", d, p)[0]
        tag = d[p + 4:p + 8]
        chunk = d[p + 8:p + 8 + ln]
        p += 12 + ln
        if tag == b"IHDR":
            w, h, bitd, ctype, _comp, _filt, interlace = struct.unpack(">IIBBBBB", chunk)
        elif tag == b"PLTE":
            plte = chunk
        elif tag == b"tRNS":
            trns = chunk
        elif tag == b"IDAT":
            idat += chunk
        elif tag == b"IEND":
            break
    if bitd != 8:
        raise ValueError("only 8-bit PNGs are supported (got bit depth %s)" % bitd)
    if interlace:
        raise ValueError("interlaced PNGs are not supported")
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(ctype)
    if channels is None:
        raise ValueError("unsupported PNG colour type %s" % ctype)
    raw = zlib.decompress(bytes(idat))
    stride = w * channels
    # unfilter
    out = bytearray(stride * h)
    prev = bytearray(stride)
    pos = 0
    bpp = channels
    for y in range(h):
        ft = raw[pos]; pos += 1
        line = bytearray(raw[pos:pos + stride]); pos += stride
        if ft == 1:      # Sub
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif ft == 2:    # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ft == 3:    # Average
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ft == 4:    # Paeth
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                pa = abs(b - c); pb = abs(a - c); pc = abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        out[y * stride:(y + 1) * stride] = line
        prev = line
    # expand to RGBA
    rgba = bytearray(w * h * 4)
    for i in range(w * h):
        o = i * 4
        if ctype == 6:
            rgba[o:o + 4] = out[i * 4:i * 4 + 4]
        elif ctype == 2:
            rgba[o:o + 3] = out[i * 3:i * 3 + 3]; rgba[o + 3] = 255
        elif ctype == 0:
            g = out[i]; rgba[o] = rgba[o + 1] = rgba[o + 2] = g; rgba[o + 3] = 255
        elif ctype == 3:
            idx = out[i]
            rgba[o] = plte[idx * 3]; rgba[o + 1] = plte[idx * 3 + 1]; rgba[o + 2] = plte[idx * 3 + 2]
            rgba[o + 3] = trns[idx] if (trns and idx < len(trns)) else 255
        elif ctype == 4:
            g = out[i * 2]; rgba[o] = rgba[o + 1] = rgba[o + 2] = g; rgba[o + 3] = out[i * 2 + 1]
    return rgba, w, h


# ---- TIM parse to index sheet + palettes ------------------------------------

def parse_tim_indexed(data):
    """Return dict(bpp, clutW, clutH, w, h, palettes[row][entry], idx[y][x])."""
    if data[0] != 0x10 or data[1] or data[2] or data[3]:
        raise ValueError("not a TIM")
    flags = _u32(data, 4)
    bpp = {0: 4, 1: 8, 2: 16, 3: 24}.get(flags & 7)
    if bpp not in (4, 8):
        raise ValueError("compose/split need a paletted (4/8bpp) TIM")
    if not ((flags >> 3) & 1):
        # 1ST/FONT8NOC.TIM: 4bpp with no CLUT block (the game supplies the palette).
        # Reading one anyway yields nonsense dimensions.
        raise ValueError("paletted TIM without a CLUT")
    p = 8
    blen = _u32(data, p)
    clut_w = _u16(data, p + 8)
    clut_h = _u16(data, p + 10) or 1
    cbase = p + 12
    palettes = [[_u16(data, cbase + (r * clut_w + c) * 2) for c in range(clut_w)]
                for r in range(clut_h)]
    p += blen
    pcw = _u16(data, p + 8)
    ph = _u16(data, p + 10)
    pix = p + 12
    pw = pcw * (4 if bpp == 4 else 2)
    rowbytes = pcw * 2
    idx = [[0] * pw for _ in range(ph)]
    for y in range(ph):
        base = pix + y * rowbytes
        for x in range(pw):
            if bpp == 4:
                b = data[base + (x >> 1)]
                idx[y][x] = (b >> 4) & 0xF if (x & 1) else b & 0xF
            else:
                idx[y][x] = data[base + x]
    return dict(bpp=bpp, clutW=clut_w, clutH=clut_h, w=pw, h=ph, palettes=palettes, idx=idx)


def tim_clut_rows(path):
    """CLUT row count of a TIM without decoding it; 1 for 16/24bpp."""
    with open(path, "rb") as f:
        d = f.read(24)
    if len(d) < 20 or d[0] != 0x10:
        return 1
    if not ((_u32(d, 4) >> 3) & 1):
        return 1
    return _u16(d, 18) or 1


# ---- LM container parse (.ILM / .PLM / .IPD) --------------------------------

def _u8(d, o):
    return d[o]


def open_model(path):
    """Locate the s_LmHeader inside any of the three containers.

    .ILM/.PLM hold it at offset 0; an .IPD stores a file-relative offset to it in
    s_IpdHeader::lmHdr (u32 @ 4). Every pointer inside the LM is relative to the LM
    header itself (lm_reformat.c parses with base = the lmHdr address), which is the
    same thing as file-relative only for .ILM/.PLM.
    """
    with open(path, "rb") as f:
        data = f.read()
    ext = os.path.splitext(path)[1].upper()
    if ext == ".IPD" or (ext not in (".ILM", ".PLM") and data[:1] == bytes([IPD_MAGIC])):
        if not data or data[0] != IPD_MAGIC:
            raise ValueError("%s: not an IPD (magic %s)" % (path, data[:1]))
        base = _u32(data, 4)
        kind = "IPD"
    else:
        base = 0
        kind = "PLM" if ext == ".PLM" else "ILM"
    if base + 20 > len(data) or data[base] != LM_MAGIC:
        raise ValueError("%s: no LM header at offset 0x%X" % (path, base))
    version = data[base + 1]
    if version != LM_VERSION:
        # v7 = the port's wide high-poly ILM: a u32 geometry blob with a different
        # primitive layout, not something this parser can walk.
        raise ValueError("%s: LM version %d, expected %d" % (path, version, LM_VERSION))
    # Only .ILM uses the 0xFF 4th-vertex-index triangle sentinel. World geometry
    # (.IPD/.PLM) is 100% quads and byte prim+0xF there is a REAL vertex index —
    # testing it would drop real quads. Verified over the whole corpus: 6109 hits,
    # every one in CHARA/TEST .ILM, zero in 431185 .IPD + 10735 .PLM prims.
    return dict(path=path, data=data, base=base, kind=kind, version=version,
                tri_sentinel=(kind == "ILM"))


def parse_lm_materials(mdl):
    """Material table only - cheap enough to run over the whole tree for indexing."""
    data, base = mdl["data"], mdl["base"]
    mat_count = _u8(data, base + 3)
    mat_ptr = base + _u32(data, base + 4)
    mats = []
    for i in range(mat_count):
        b = mat_ptr + i * 24
        mats.append(dict(
            name=data[b:b + 8].split(b"\x00")[0].decode("ascii", "replace").strip(),
            base_cluty=_u16(data, b + 0x10) >> 6,
            tpage=_u8(data, b + 0xE)))
    return mats


def parse_lm(mdl):
    """Return (materials, prims). Each prim: dict(uvs=[(u,v)*4], row, mat, tri)."""
    data, base = mdl["data"], mdl["base"]
    materials = parse_lm_materials(mdl)
    model_count = _u8(data, base + 8)
    model_hdrs = base + _u32(data, base + 0xC)
    tri_sentinel = mdl["tri_sentinel"]
    prims = []
    for m in range(model_count):
        mh = model_hdrs + m * 16
        mesh_count = _u8(data, mh + 8)
        mesh_hdrs = base + _u32(data, mh + 0xC)
        for k in range(mesh_count):
            msh = mesh_hdrs + k * 24
            pc = _u8(data, msh + 0)
            pp = base + _u32(data, msh + 4)
            for pi in range(pc):
                pb = pp + pi * 20
                clut = _u16(data, pb + 2)
                flags = _u16(data, pb + 6)
                mat = (flags >> 8) & 0x7F
                bs = materials[mat]["base_cluty"] if mat < len(materials) else 0
                uvs = [(_u16(data, pb + o) & 0xFF, _u16(data, pb + o) >> 8) for o in (0, 4, 8, 0xA)]
                # Character triangle prims set the 4th vertex index (field_C[3]) to
                # 0xFF and leave UV3 as garbage (0,0); rasterising them as a quad
                # drags a streak to (0,0).
                is_tri = tri_sentinel and data[pb + 0xF] == 0xFF
                prims.append(dict(uvs=uvs, row=(clut >> 6) - bs, mat=mat, tri=is_tri))
    return materials, prims


def parse_ilm(data):
    """Back-compat: parse a raw .ILM/.PLM byte string (LM header at offset 0)."""
    mdl = dict(path="<memory>", data=data, base=0, kind="ILM",
               version=data[1] if len(data) > 1 else 0, tri_sentinel=True)
    return parse_lm(mdl)


# ---- rasterise primitives -> per-texel row map ------------------------------

def _fill_rect(rowmap, x0, y0, x1, y1, row, W, H):
    x0 = 0 if x0 < 0 else x0
    y0 = 0 if y0 < 0 else y0
    x1 = W - 1 if x1 > W - 1 else x1
    y1 = H - 1 if y1 > H - 1 else y1
    if x1 < x0 or y1 < y0:
        return
    span = [row] * (x1 - x0 + 1)
    for y in range(y0, y1 + 1):
        o = y * W + x0
        rowmap[o:o + len(span)] = span


SLACK_DEN = 50  # barycentric over-cover of 1/50, i.e. the historical -0.02


def _fill_tri(rowmap, a, b, c, row, W, H):
    """Barycentric fill with a 1/50 over-cover (closes hairline cracks between prims).

    Solved per scanline rather than tested per texel: every edge function is linear
    in x, so the covered span is an interval intersection. Kept in exact integers —
    `w/det >= -1/50` is `50*sgn*w + |det| >= 0` — so the boundary never depends on
    float rounding.
    """
    ax, ay = a; bx, by = b; cx, cy = c
    det = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
    if det == 0:
        return
    miny = max(0, min(ay, by, cy))
    maxy = min(H - 1, max(ay, by, cy))
    minx = max(0, min(ax, bx, cx))
    maxx = min(W - 1, max(ax, bx, cx))
    if maxy < miny or maxx < minx:
        return
    sgn = 1 if det > 0 else -1
    ad = det if det > 0 else -det
    # w0 = A0*x + C0*(y-cy) - A0*cx; w1 likewise; w2 = det - w0 - w1.
    A0 = by - cy; C0 = cx - bx
    A1 = cy - ay; C1 = ax - cx
    k = SLACK_DEN * sgn
    e0 = (k * A0, k * C0, -k * A0 * cx + ad)
    e1 = (k * A1, k * C1, -k * A1 * cx + ad)
    e2 = (-k * (A0 + A1), -k * (C0 + C1), k * ((A0 + A1) * cx + det) + ad)
    for y in range(miny, maxy + 1):
        dy = y - cy
        lo, hi = minx, maxx
        for (A, C, K) in (e0, e1, e2):
            if A == 0:
                if C * dy + K < 0:
                    lo = hi + 1
                    break
                continue
            bb = C * dy + K
            if A > 0:
                v = -(bb // A)           # ceil(-bb / A)
                if v > lo:
                    lo = v
            else:
                v = bb // (-A)           # floor(-bb / A), A < 0
                if v < hi:
                    hi = v
            if lo > hi:
                break
        if lo > hi:
            continue
        o = y * W + lo
        rowmap[o:o + hi - lo + 1] = [row] * (hi - lo + 1)


def _fill_prim(rowmap, uv, row, tri, W, H):
    if tri:
        _fill_tri(rowmap, uv[0], uv[1], uv[2], row, W, H)
        return
    us = (uv[0][0], uv[1][0], uv[2][0], uv[3][0])
    vs = (uv[0][1], uv[1][1], uv[2][1], uv[3][1])
    x0, x1 = min(us), max(us)
    y0, y1 = min(vs), max(vs)
    # Axis-aligned rectangle fast path — an optimisation, not a behaviour change:
    # each of the two FT4 triangles spans three corners, so its integer bounding box
    # (which clips the fill) is the whole rect, and their slack-expanded union covers
    # the rect; clipped, that is exactly the rect. Needs the STRIP pairing (uv0/uv3
    # and uv1/uv2 diagonal) — with the other pairing the triangles leave a gap.
    if x0 != x1 and y0 != y1 and \
       uv[0][0] != uv[3][0] and uv[0][1] != uv[3][1] and \
       set(uv) == {(x0, y0), (x1, y0), (x0, y1), (x1, y1)}:
        _fill_rect(rowmap, x0, y0, x1, y1, row, W, H)
        return
    _fill_tri(rowmap, uv[0], uv[1], uv[2], row, W, H)
    _fill_tri(rowmap, uv[1], uv[3], uv[2], row, W, H)  # PSX FT4 winding


def build_rowmap(prims, W, H, dilate=1, stats=None):
    rowmap = [-1] * (W * H)
    clamped = 0
    for p in prims:
        uv = p["uvs"]
        if uv[0][0] >= W or uv[1][0] >= W or uv[2][0] >= W or uv[3][0] >= W or \
           uv[0][1] >= H or uv[1][1] >= H or uv[2][1] >= H or uv[3][1] >= H:
            # Half-page materials (RSR08H, SPR07H, THR9002H, THRB502H) address past
            # their 128-wide sheet into the neighbouring VRAM page. Clamp into the
            # sheet and take the bounding box: the true footprint is a clip anyway,
            # and clamping can make the triangles degenerate (which would leave holes).
            clamped += 1
            cu = [min(u, W - 1) for (u, _v) in uv]
            cv = [min(v, H - 1) for (_u, v) in uv]
            if p["tri"]:
                cu, cv = cu[:3], cv[:3]
            _fill_rect(rowmap, min(cu), min(cv), max(cu), max(cv), p["row"], W, H)
            continue
        _fill_prim(rowmap, uv, p["row"], p["tri"], W, H)
    for _ in range(max(0, dilate)):
        rowmap = _dilate(rowmap, W, H)
    if stats is not None:
        stats["clampedPrims"] = clamped
    return rowmap


def build_rowmap_bleed(prims, W, H, stats=None):
    """Dilated row-map plus, for each texel the dilation ADDED, the covered texel it
    should borrow its colour from.

    Straight dilation is wrong for split: it claims a texel just outside the UV island
    and then copies whatever the edited image happens to hold there. Editors (and every
    AI upscaler) leave that area opaque BLACK — an RGB export has no alpha at all — so
    each island came back ringed in black. Borrowing the adjacent covered texel instead
    extends the art outward, which is the usual UV edge-padding and is what the dilation
    was always meant to achieve."""
    true = build_rowmap(prims, W, H, dilate=0, stats=stats)
    out = list(true)
    src = [-1] * (W * H)
    for y in range(H):
        for x in range(W):
            i = y * W + x
            if true[i] != -1:
                continue
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                nx, ny = x + dx, y + dy
                if 0 <= nx < W and 0 <= ny < H and true[ny * W + nx] != -1:
                    out[i] = true[ny * W + nx]
                    src[i] = ny * W + nx
                    break
    return out, src


def _dilate(rowmap, W, H):
    """Grow covered regions into adjacent uncovered texels (closes hairline cracks;
    over-inclusion is harmless — each row's PNG is only sampled by that row's prims)."""
    out = list(rowmap)
    for y in range(H):
        for x in range(W):
            if rowmap[y * W + x] != -1:
                continue
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                nx, ny = x + dx, y + dy
                if 0 <= nx < W and 0 <= ny < H and rowmap[ny * W + nx] != -1:
                    out[y * W + x] = rowmap[ny * W + nx]
                    break
    return out


# ---- corpus index -----------------------------------------------------------

MODEL_EXTS = (".ILM", ".PLM", ".IPD")


def _walk(root, exts):
    hits = []
    for dp, _dirs, fns in os.walk(root):
        for fn in fns:
            if os.path.splitext(fn)[1].upper() in exts:
                hits.append(os.path.join(dp, fn))
    hits.sort()
    return hits


def _rel(root, path):
    return os.path.relpath(path, root).replace("\\", "/")


def build_index(root, verbose=True):
    """Walk the tree once: every model material -> the .TIM its NAME names.

    Resolving by material name (not by the model's file stem) is what makes
    BIRD.ILM find REBIRD.TIM, MAN.ILM find TEST/DEV.TIM, and every weapon .PLM
    find CHARA/HERO.TIM — the references that carry HERO's missing palette rows.
    """
    root = os.path.abspath(root)
    tim_paths = _walk(root, (".TIM",))
    by_stem = {}
    for tp in tim_paths:
        by_stem.setdefault(os.path.splitext(os.path.basename(tp))[0].upper(), []).append(tp)

    tims = {}
    for tp in tim_paths:
        tims[_rel(root, tp)] = {"refs": []}

    unresolved = {}
    models = _walk(root, MODEL_EXTS)
    bad = []
    for mp in models:
        try:
            mdl = open_model(mp)
            mats = parse_lm_materials(mdl)
        except Exception as ex:  # noqa: BLE001 — report and continue
            bad.append((_rel(root, mp), str(ex)))
            continue
        mdir = os.path.dirname(mp)
        for i, mat in enumerate(mats):
            name = mat["name"].upper()
            if not name:
                continue
            cands = by_stem.get(name)
            if not cands:
                unresolved[name] = unresolved.get(name, 0) + 1
                continue
            # Same folder wins (BLOOD.TIM and TITLE.TIM each exist in 2-3 folders).
            pick = next((c for c in cands if os.path.dirname(c) == mdir), cands[0])
            tims[_rel(root, pick)]["refs"].append([_rel(root, mp), i])
    idx = {"version": INDEX_VERSION, "root": root, "tims": tims,
           "unresolved": unresolved, "badModels": bad}
    if verbose:
        refd = sum(1 for v in tims.values() if v["refs"])
        print("index: %d model file(s), %d TIM(s), %d referenced by geometry, "
              "%d unresolvable material name(s)%s"
              % (len(models), len(tims), refd, len(unresolved),
                 ", %d unparsable model(s)" % len(bad) if bad else ""))
        for n, c in sorted(unresolved.items()):
            print("   material '%s' has no .TIM on disc (%d prim refs) - procedural" % (n, c))
    return idx


def load_index(path, root_override=None):
    with open(path, "r") as f:
        idx = json.load(f)
    if idx.get("version") != INDEX_VERSION:
        raise SystemExit("%s: index version %s, expected %d - rebuild with `index`"
                         % (path, idx.get("version"), INDEX_VERSION))
    if root_override:
        idx["root"] = os.path.abspath(root_override)
    if not os.path.isdir(idx["root"]):
        raise SystemExit("index root %s does not exist; pass --root" % idx["root"])
    return idx


def collect_prims(idx, tim_rels):
    """UNION the primitives of every model that references each TIM.

    Parses each model ONCE even when it feeds several sheets. Prims keep their file
    order within a model and models are visited in sorted order, so the last-writer
    conflict resolution is deterministic; exact duplicates (the same UV rect drawn by
    dozens of chunk instances) are dropped, which is most of the corpus.
    """
    root = idx["root"]
    want = {}
    for t in tim_rels:
        for mrel, mi in idx["tims"][t]["refs"]:
            want.setdefault(mrel, []).append((mi, t))
    out = {t: [] for t in tim_rels}
    seen = {t: set() for t in tim_rels}
    for mrel in sorted(want):
        try:
            _mats, prims = parse_lm(open_model(os.path.join(root, mrel)))
        except Exception as ex:  # noqa: BLE001
            print("   WARNING: %s: %s" % (mrel, ex), file=sys.stderr)
            continue
        bymat = {}
        for mi, t in want[mrel]:
            bymat.setdefault(mi, []).append(t)
        for p in prims:
            targets = bymat.get(p["mat"])
            if not targets:
                continue
            uv = p["uvs"]
            key = (uv[0][0], uv[0][1], uv[1][0], uv[1][1], uv[2][0], uv[2][1],
                   uv[3][0], uv[3][1], p["row"], p["tri"])
            for t in targets:
                if key in seen[t]:
                    continue
                seen[t].add(key)
                out[t].append(p)
    return out


# ---- compose / split --------------------------------------------------------

def _resolve_tim_for_model(model_path, name, index=None):
    """material name -> .TIM path. Sidecar folder first, then the whole index."""
    mdir = os.path.dirname(os.path.abspath(model_path)) or "."
    for cand in (os.path.join(mdir, name + ".TIM"), os.path.join(mdir, name + ".tim")):
        if os.path.exists(cand):
            return cand
    if index:
        up = name.upper()
        for rel in index["tims"]:
            if os.path.splitext(os.path.basename(rel))[0].upper() == up:
                return os.path.join(index["root"], rel)
    return None


def _render_composite(tim, rowmap):
    W, H = tim["w"], tim["h"]
    rgba = bytearray(W * H * 4)
    covered = 0
    oob = 0
    pal = tim["palettes"]
    ix = tim["idx"]
    ch = tim["clutH"]
    for i in range(W * H):
        r = rowmap[i]
        if r < 0:
            continue
        if r >= ch:
            oob += 1
            continue  # left transparent
        covered += 1
        y, x = divmod(i, W)
        _bgr555(pal[r][ix[y][x]], rgba, i * 4)
    return rgba, covered, oob


def compose_tim(tim_path, prims, out_path, quiet=False):
    """Render one composite from an already-unioned prim list."""
    tim = parse_tim_indexed(open(tim_path, "rb").read())
    W, H = tim["w"], tim["h"]
    stats = {}
    rowmap = build_rowmap(prims, W, H, dilate=0, stats=stats)  # compose shows true coverage
    rgba, covered, oob = _render_composite(tim, rowmap)
    write_png_rgba(out_path, W, H, bytes(rgba))
    rows = sorted({r for r in rowmap if r >= 0})
    if not quiet:
        print("compose: %s -> %s  (%dx%d, %d prims, %d/%d texels = %.1f%%, rows %s)"
              % (os.path.basename(tim_path), out_path, W, H, len(prims),
                 covered, W * H, 100.0 * covered / max(1, W * H),
                 ",".join(map(str, rows)) if rows else "-"))
        if stats.get("clampedPrims"):
            print("   %d prim(s) address past the %dx%d sheet (half-page material) - clamped"
                  % (stats["clampedPrims"], W, H))
        if oob:
            print("   WARNING: %d texel(s) select a CLUT row this TIM does not have" % oob)
    return dict(path=out_path, w=W, h=H, covered=covered, total=W * H,
                rows=rows, clutH=tim["clutH"], clamped=stats.get("clampedPrims", 0))


def split_tim(edited_path, tim_path, prims, out_dir, quiet=False):
    tim = parse_tim_indexed(open(tim_path, "rb").read())
    W, H = tim["w"], tim["h"]
    rgba, ew, eh = read_png_rgba(edited_path)
    # The edit may be the sheet's native size OR an HD upscale of it — the runtime uploads each
    # per-row PNG as full RGBA and scales it, so HD detail is kept as-is. Require only that the
    # aspect ratio matches; an exact integer multiple (2x/4x/8x) gives the sharpest region edges.
    if ew < W or eh < H or abs(ew * H - eh * W) > (ew * H) // 25:
        raise SystemExit(
            "edited image is %dx%d; it must be the sheet's native %dx%d or an upscale keeping that "
            "aspect ratio (e.g. %dx%d, %dx%d). For sharpest region edges use an exact multiple."
            % (ew, eh, W, H, W * 2, H * 2, W * 4, H * 4))
    rowmap, srcmap = build_rowmap_bleed(prims, W, H)  # dilate so no drawn texel becomes a hole
    used = sorted({r for r in rowmap if r >= 0})
    os.makedirs(out_dir, exist_ok=True)
    stem = os.path.splitext(os.path.basename(tim_path))[0]
    full = os.path.basename(tim_path) if tim_path.upper().endswith(".TIM") else stem + ".TIM"
    # The runtime keeps HIRES_POOL_MAX_ROWS CLUT rows per slot and never probes past
    # them, so a pNN.png with N >= 16 can only ever be dead weight in a mod archive.
    dropped = [r for r in used if r >= HIRES_POOL_MAX_ROWS]
    used = [r for r in used if r < HIRES_POOL_MAX_ROWS]
    rows_to_emit = sorted(set(used) | {0})  # p00 is the runtime's per-row sentinel
    # Map each edited pixel to its native texel -> palette row, in one pass, keeping full HD pixels.
    sx = [(x * W) // ew for x in range(ew)]
    sy = [(y * H) // eh for y in range(eh)]
    # First edited pixel belonging to each native texel, so a bled texel can copy the
    # matching pixel out of the texel it borrows from (any scale, integer or not).
    xstart = [(tx * ew + W - 1) // W for tx in range(W)]
    ystart = [(ty * eh + H - 1) // H for ty in range(H)]
    bufs = {r: bytearray(ew * eh * 4) for r in rows_to_emit}
    for y in range(eh):
        ty = sy[y]
        base = ty * W
        for x in range(ew):
            t = base + sx[x]
            b = bufs.get(rowmap[t])
            if b is None:
                continue
            o = (y * ew + x) * 4
            s = srcmap[t]
            if s < 0:
                b[o:o + 4] = rgba[o:o + 4]  # only row-r prims sample this PNG; rest stays transparent
            else:
                # Padding texel: take the pixel at the same offset inside the covered
                # texel we borrow from, never the (usually black) background here.
                ux = xstart[s % W] + (x - xstart[sx[x]])
                uy = ystart[s // W] + (y - ystart[ty])
                if ux >= ew: ux = ew - 1
                if uy >= eh: uy = eh - 1
                if ux < 0: ux = 0
                if uy < 0: uy = 0
                so = (uy * ew + ux) * 4
                b[o:o + 4] = rgba[so:so + 4]
    written = []
    for r in rows_to_emit:
        png = os.path.join(out_dir, "%s.p%02d.png" % (full, r))
        write_png_rgba(png, ew, eh, bytes(bufs[r]))
        written.append(png)
    if not quiet:
        print("split: %s -> %d per-row PNG(s) in %s  (rows used: %s)"
              % (os.path.basename(edited_path), len(written), out_dir,
                 ", ".join(map(str, used))))
        for w in written:
            print("   " + os.path.basename(w))
        if dropped:
            print("   WARNING: row(s) %s are past the runtime's %d-row limit "
                  "(HIRES_POOL_MAX_ROWS) and were NOT written - the game could never load them."
                  % (", ".join(map(str, dropped)), HIRES_POOL_MAX_ROWS))
    return written


# ---- single-target helpers (resolve a CLI target to TIM + prims) ------------

def _targets_for(target, tim_arg, index):
    """-> [(tim_path, prims)] for a .TIM or a model file."""
    ext = os.path.splitext(target)[1].upper()
    if ext == ".TIM":
        if not index:
            raise SystemExit("a .TIM target needs --index (build it with `clut_tool.py index ROOT`)")
        rel = _rel(index["root"], os.path.abspath(target))
        if rel not in index["tims"]:
            raise SystemExit("%s is not in the index (root %s)" % (target, index["root"]))
        prims = collect_prims(index, [rel])[rel]
        if not prims:
            raise SystemExit("%s is not referenced by any model in the index - it is a flat "
                             "2D/UI texture; use tim2png.py on it instead" % target)
        return [(os.path.join(index["root"], rel), prims)]

    mdl = open_model(target)
    mats, prims = parse_lm(mdl)
    if not mats:
        raise SystemExit("%s declares no materials" % target)
    out = []
    for i, mat in enumerate(mats):
        tim_path = tim_arg if (tim_arg and len(mats) == 1) else \
            _resolve_tim_for_model(target, mat["name"], index)
        if not tim_path:
            print("   WARNING: material '%s' of %s has no .TIM (procedural) - skipped"
                  % (mat["name"], os.path.basename(target)), file=sys.stderr)
            continue
        if index:
            rel = _rel(index["root"], os.path.abspath(tim_path))
            if rel in index["tims"] and index["tims"][rel]["refs"]:
                out.append((tim_path, collect_prims(index, [rel])[rel]))
                continue
        out.append((tim_path, [p for p in prims if p["mat"] == i]))
    return out


# ---- compose-all ------------------------------------------------------------

def _class_of(rel):
    d = os.path.dirname(rel)
    return d.split("/")[0] if d else "."


def compose_all(idx, out_dir, only=None):
    root = idx["root"]
    rels = sorted(idx["tims"])
    if only:
        rels = [r for r in rels if fnmatch.fnmatch(r.upper(), only.upper())]
    referenced = [r for r in rels if idx["tims"][r]["refs"]]
    flat = [r for r in rels if not idx["tims"][r]["refs"]]
    print("compose-all: %d TIM(s) - %d geometry-referenced, %d flat"
          % (len(rels), len(referenced), len(flat)))

    stats = {}
    done = 0
    for rel in referenced:
        prims = collect_prims(idx, [rel])[rel]
        dst = os.path.join(out_dir, os.path.dirname(rel),
                           os.path.splitext(os.path.basename(rel))[0] + "_reference.png")
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        try:
            r = compose_tim(os.path.join(root, rel), prims, dst, quiet=True)
        except Exception as ex:  # noqa: BLE001
            print("   FAILED %s: %s" % (rel, ex), file=sys.stderr)
            continue
        r["kind"] = "composite"
        stats[rel] = r
        done += 1
        if done % 50 == 0:
            print("   ... %d/%d" % (done, len(referenced)))

    # A TIM no model references is a flat 2D/UI sheet: its own decode IS the reference
    # image, so emit that (row 0) and count it as fully covered. Only a multi-row one
    # without references is genuinely unreconstructable.
    for rel in flat:
        src = os.path.join(root, rel)
        try:
            rows = tim_clut_rows(src)
            rgba, w, h, _ = decode_tim(open(src, "rb").read(), 0)
        except Exception as ex:  # noqa: BLE001
            print("   FAILED %s: %s" % (rel, ex), file=sys.stderr)
            continue
        dst = os.path.join(out_dir, os.path.dirname(rel),
                           os.path.splitext(os.path.basename(rel))[0] + "_reference.png")
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        write_png_rgba(dst, w, h, rgba)
        stats[rel] = dict(path=dst, w=w, h=h, covered=w * h, total=w * h, rows=[0],
                          clutH=rows, clamped=0,
                          kind="flat" if rows <= 1 else "flat-multirow")

    # per-class coverage summary
    cls = {}
    for rel, r in stats.items():
        c = cls.setdefault(_class_of(rel), dict(n=0, cov=0, tot=0, full=0, multi=0, unrec=0))
        c["n"] += 1
        c["cov"] += r["covered"]
        c["tot"] += r["total"]
        if r["covered"] == r["total"]:
            c["full"] += 1
        if r["clutH"] > 1:
            c["multi"] += 1
        if r["kind"] == "flat-multirow":
            c["unrec"] += 1
    print("\n%-8s %6s %8s %8s %8s   %s" % ("CLASS", "TIMs", "multiCLUT", "texel%", "full", "no-rowmap(multiCLUT)"))
    tn = tc = tt = 0
    for k in sorted(cls):
        c = cls[k]
        print("%-8s %6d %8d %8.1f %8d   %d"
              % (k, c["n"], c["multi"], 100.0 * c["cov"] / max(1, c["tot"]), c["full"], c["unrec"]))
        tn += c["n"]; tc += c["cov"]; tt += c["tot"]
    print("%-8s %6d %8s %8.1f" % ("TOTAL", tn, "", 100.0 * tc / max(1, tt)))
    return stats


# ---- CLI --------------------------------------------------------------------

def _maybe_index(args):
    if getattr(args, "index", None):
        return load_index(args.index, getattr(args, "root", None))
    if getattr(args, "root", None):
        return build_index(args.root)
    return None


def main(argv):
    ap = argparse.ArgumentParser(description="Compose/split Silent Hill CLUT textures.")
    sub = ap.add_subparsers(dest="cmd", required=True)

    i = sub.add_parser("index", help="walk an extracted disc tree -> TIM/model reference index")
    i.add_argument("root")
    i.add_argument("-o", "--out")

    c = sub.add_parser("compose", help="model or TIM -> single correct reference PNG")
    c.add_argument("target", help=".ILM/.PLM/.IPD model, or a .TIM (needs --index)")
    c.add_argument("tim", nargs="?")
    c.add_argument("-o", "--out")
    c.add_argument("--index")
    c.add_argument("--root")

    s = sub.add_parser("split", help="edited reference PNG -> per-row PNG set")
    s.add_argument("edited")
    s.add_argument("target", help=".ILM/.PLM/.IPD model, or a .TIM (needs --index)")
    s.add_argument("tim", nargs="?")
    s.add_argument("-o", "--out")
    s.add_argument("--index")
    s.add_argument("--root")

    a = sub.add_parser("compose-all", help="every TIM in the tree -> reference PNGs + coverage")
    a.add_argument("-o", "--out", required=True)
    a.add_argument("--index")
    a.add_argument("--root")
    a.add_argument("--only", help="fnmatch filter on the TIM's path inside the tree, e.g. 'BG/*'")

    args = ap.parse_args(argv)

    if args.cmd == "index":
        idx = build_index(args.root)
        out = args.out or INDEX_DEFAULT_NAME
        with open(out, "w") as f:
            json.dump(idx, f)
        print("wrote %s (%.1f MB)" % (out, os.path.getsize(out) / 1048576.0))
        return 0

    if args.cmd == "compose-all":
        idx = _maybe_index(args)
        if not idx:
            raise SystemExit("compose-all needs --index or --root")
        compose_all(idx, args.out, args.only)
        return 0

    idx = _maybe_index(args)
    if args.cmd == "compose":
        pairs = _targets_for(args.target, args.tim, idx)
        multi = len(pairs) > 1
        for tim_path, prims in pairs:
            stem = os.path.splitext(os.path.basename(tim_path))[0]
            if args.out and not multi:
                out = args.out
            else:
                d = args.out if (args.out and multi) else \
                    (os.path.dirname(os.path.abspath(args.target)) or ".")
                os.makedirs(d, exist_ok=True)
                out = os.path.join(d, stem + "_reference.png")
            compose_tim(tim_path, prims, out)
    else:
        pairs = _targets_for(args.target, args.tim, idx)
        if len(pairs) > 1:
            raise SystemExit("%s uses %d textures; split one at a time by passing the .TIM "
                             "as the target (with --index)" % (args.target, len(pairs)))
        tim_path, prims = pairs[0]
        out = args.out or (os.path.dirname(os.path.abspath(args.edited)) or ".")
        split_tim(args.edited, tim_path, prims, out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
