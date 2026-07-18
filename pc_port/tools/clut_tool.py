#!/usr/bin/env python3
"""
clut_tool — author Silent Hill character/monster texture mods from ONE image.

A paletted PSX TIM is ONE 4-bit index sheet plus N CLUT rows (palettes). A single
character draws its face, body and limbs through DIFFERENT rows over that shared
sheet (verified: DOB uses 7 rows across 278 prims, HERO 7-of-15, DARIA 11), so no
single `pNN.png` ever looks right on its own — each is the whole sheet tinted by
one palette. Which region uses which row is baked into the model's primitives
(each `s_Primitive.field_2` clut word: `row = (field_2 >> 6) - materialBaseClutY`),
so the true in-game look is reconstructable offline from the `.ILM` model + `.TIM`.

Two directions:

  compose  NAME.ILM + NAME.TIM  ->  NAME_reference.png
      The correct composite: every texel painted through the palette row the game
      actually draws it with. Edit THIS in any image editor.

  split    NAME_reference(edited).png + NAME.ILM + NAME.TIM  ->  NAME.TIM.pNN.png set
      Slices your edited image back into the per-row loose-override PNGs the runtime
      loads (gamedata/load/<FOLDER>/). Each pNN.png keeps only the texels the game
      samples through row N; the rest is transparent. The loose-PNG path uploads
      each row as full RGBA (no 16-colour re-quantise), so your edit is NOT limited
      to 16 colours per region — paint freely.

Usage:
    python3 clut_tool.py compose CHARA/DOB.ILM [CHARA/DOB.TIM] [-o DOB_reference.png]
    python3 clut_tool.py split   DOB_reference.png CHARA/DOB.ILM [CHARA/DOB.TIM] [-o OUTDIR]
If the .TIM path is omitted it is assumed to sit beside the .ILM with the same stem.
"""

import os
import sys
import zlib
import struct
import argparse

# Share the exact decoder/PNG-writer with tim2png so both tools stay byte-consistent.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tim2png import write_png_rgba, _u16, _u32, _bgr555  # noqa: E402


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


# ---- ILM parse to primitive (uv, row) list ----------------------------------

def _u8(d, o):
    return d[o]


def parse_ilm(data):
    """Return (materials, prims). Each prim: dict(uvs=[(u,v)*4], row, mat)."""
    mat_count = _u8(data, 3)
    mat_ptr = _u32(data, 4)
    model_count = _u8(data, 8)
    model_hdrs = _u32(data, 0xC)
    materials = []
    for i in range(mat_count):
        b = mat_ptr + i * 24
        materials.append(dict(
            name=data[b:b + 8].split(b"\x00")[0].decode("ascii", "replace"),
            base_cluty=_u16(data, b + 0x10) >> 6,
            tpage=_u8(data, b + 0xE)))
    prims = []
    for m in range(model_count):
        mh = model_hdrs + m * 16
        mesh_count = _u8(data, mh + 8)
        mesh_hdrs = _u32(data, mh + 0xC)
        for k in range(mesh_count):
            msh = mesh_hdrs + k * 24
            pc = _u8(data, msh + 0)
            pp = _u32(data, msh + 4)
            for pi in range(pc):
                pb = pp + pi * 20
                clut = _u16(data, pb + 2)
                flags = _u16(data, pb + 6)
                mat = (flags >> 8) & 0x7F
                base = materials[mat]["base_cluty"] if mat < len(materials) else 0
                uvs = [(_u16(data, pb + o) & 0xFF, _u16(data, pb + o) >> 8) for o in (0, 4, 8, 0xA)]
                # Triangle prims set the 4th vertex index (field_C[3]) to 0xFF and leave
                # UV3 as garbage (0,0); rasterising them as a quad drags a streak to (0,0).
                is_tri = data[pb + 0xF] == 0xFF
                prims.append(dict(uvs=uvs, row=(clut >> 6) - base, mat=mat, tri=is_tri))
    return materials, prims


# ---- rasterise primitives -> per-texel row map ------------------------------

def _fill_tri(rowmap, a, b, c, row, W, H):
    xs = (a[0], b[0], c[0]); ys = (a[1], b[1], c[1])
    minx = max(0, min(xs)); maxx = min(W - 1, max(xs))
    miny = max(0, min(ys)); maxy = min(H - 1, max(ys))
    det = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1])
    if det == 0:
        return
    for y in range(miny, maxy + 1):
        for x in range(minx, maxx + 1):
            w0 = ((b[1] - c[1]) * (x - c[0]) + (c[0] - b[0]) * (y - c[1])) / det
            w1 = ((c[1] - a[1]) * (x - c[0]) + (a[0] - c[0]) * (y - c[1])) / det
            w2 = 1.0 - w0 - w1
            if w0 >= -0.02 and w1 >= -0.02 and w2 >= -0.02:  # slight over-cover closes cracks
                rowmap[y * W + x] = row


def build_rowmap(prims, W, H, dilate=1):
    rowmap = [-1] * (W * H)
    for p in prims:
        uv = p["uvs"]
        r = p["row"]
        _fill_tri(rowmap, uv[0], uv[1], uv[2], r, W, H)
        if not p["tri"]:  # quad: second triangle, PSX FT4 winding v0v1v2 / v1v3v2
            _fill_tri(rowmap, uv[1], uv[3], uv[2], r, W, H)
    for _ in range(max(0, dilate)):
        rowmap = _dilate(rowmap, W, H)
    return rowmap


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


# ---- commands ---------------------------------------------------------------

def _resolve_tim(ilm_path, tim_path):
    if tim_path:
        return tim_path
    cand = os.path.splitext(ilm_path)[0] + ".TIM"
    if os.path.exists(cand):
        return cand
    raise SystemExit("could not find a .TIM beside %s; pass it explicitly" % ilm_path)


def compose(ilm_path, tim_path, out_path):
    tim = parse_tim_indexed(open(tim_path, "rb").read())
    _mats, prims = parse_ilm(open(ilm_path, "rb").read())
    W, H = tim["w"], tim["h"]
    rowmap = build_rowmap(prims, W, H, dilate=0)  # compose shows true coverage
    rgba = bytearray(W * H * 4)
    covered = 0
    for i in range(W * H):
        r = rowmap[i]
        if r < 0 or r >= tim["clutH"]:
            continue  # left transparent
        covered += 1
        y, x = divmod(i, W)
        _bgr555(tim["palettes"][r][tim["idx"][y][x]], rgba, i * 4)
    write_png_rgba(out_path, W, H, bytes(rgba))
    print("compose: %s + %s -> %s  (%dx%d, %d prims, %d/%d texels covered)"
          % (os.path.basename(ilm_path), os.path.basename(tim_path), out_path,
             W, H, len(prims), covered, W * H))
    return out_path


def split(edited_path, ilm_path, tim_path, out_dir):
    tim = parse_tim_indexed(open(tim_path, "rb").read())
    _mats, prims = parse_ilm(open(ilm_path, "rb").read())
    W, H = tim["w"], tim["h"]
    rgba, ew, eh = read_png_rgba(edited_path)
    if (ew, eh) != (W, H):
        raise SystemExit("edited image is %dx%d but the texture sheet is %dx%d; "
                         "v1 needs a native-resolution edit (HD support is a later add)"
                         % (ew, eh, W, H))
    rowmap = build_rowmap(prims, W, H, dilate=1)  # dilate so no drawn texel becomes a hole
    used = sorted({r for r in rowmap if r >= 0})
    os.makedirs(out_dir, exist_ok=True)
    stem = os.path.splitext(os.path.basename(tim_path))[0]
    full = os.path.basename(tim_path) if tim_path.upper().endswith(".TIM") else stem + ".TIM"
    pad = 3 if tim["clutH"] > 100 else 2
    written = []
    rows_to_emit = sorted(set(used) | {0})  # p00 is the runtime's per-row sentinel
    for r in rows_to_emit:
        buf = bytearray(W * H * 4)
        for i in range(W * H):
            if rowmap[i] == r:
                buf[i * 4:i * 4 + 4] = rgba[i * 4:i * 4 + 4]
            # else: leave transparent — only row-r prims sample this PNG
        png = os.path.join(out_dir, "%s.p%0*d.png" % (full, pad, r))
        write_png_rgba(png, W, H, bytes(buf))
        written.append(png)
    print("split: %s -> %d per-row PNG(s) in %s  (rows used: %s)"
          % (os.path.basename(edited_path), len(written), out_dir,
             ", ".join(map(str, used))))
    for w in written:
        print("   " + os.path.basename(w))
    return written


def main(argv):
    ap = argparse.ArgumentParser(description="Compose/split Silent Hill character CLUT textures.")
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("compose", help="ILM + TIM -> single correct reference PNG")
    c.add_argument("ilm")
    c.add_argument("tim", nargs="?")
    c.add_argument("-o", "--out")
    s = sub.add_parser("split", help="edited reference PNG + ILM + TIM -> per-row PNG set")
    s.add_argument("edited")
    s.add_argument("ilm")
    s.add_argument("tim", nargs="?")
    s.add_argument("-o", "--out")
    a = ap.parse_args(argv)
    if a.cmd == "compose":
        tim = _resolve_tim(a.ilm, a.tim)
        out = a.out or (os.path.splitext(a.ilm)[0] + "_reference.png")
        compose(a.ilm, tim, out)
    else:
        tim = _resolve_tim(a.ilm, a.tim)
        out = a.out or (os.path.dirname(os.path.abspath(a.edited)) or ".")
        split(a.edited, a.ilm, tim, out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
