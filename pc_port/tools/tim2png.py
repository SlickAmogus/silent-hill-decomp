#!/usr/bin/env python3
"""
tim2png — PSX TIM -> PNG converter for Silent Hill PC-port modding.

Cross-platform (pure Python 3, no third-party modules — PNG is written with the
stdlib `zlib`), so Linux/macOS modders get the one authoring tool the Windows
launcher otherwise provides. The decoder mirrors the game's own
pc_port/src/hires_override.c parse_tim_to_rgba EXACTLY (BGR555 expansion, and the
alpha rule: a 16-bit colour word of 0x0000 is transparent, everything else fully
opaque; the STP bit is ignored).

MULTI-PALETTE TIMs: a paletted TIM is ONE 4-/8-bit index sheet plus N CLUT ROWS
(palette variants). A monster draws its head, body and limbs through DIFFERENT
rows over the same texels, so a single row-0 PNG can only recolour one region.
This tool emits one PNG per row for a multi-CLUT TIM, named to match the game's
per-row loose-override probe:

    FOO.TIM  (6 CLUT rows)  ->  FOO.TIM.p00.png .. FOO.TIM.p05.png
    FLAT.TIM (1 CLUT row)   ->  FLAT.png

Drop the PNGs into  gamedata/load/<FOLDER>/  (same folder layout as the disc)
and set  allow_loose_files = 1  in config.cfg. Edit the palette-row PNG for the
region you want to change; ship only the rows you edited (unshipped rows keep
the original art).

Usage:
    python3 tim2png.py FILE.TIM [FILE2.TIM ...]     # convert each, beside it
    python3 tim2png.py --bulk DIR [--delete]        # recurse DIR, convert in place
    python3 tim2png.py --row N FILE.TIM             # write only CLUT row N
"""

import os
import sys
import zlib
import struct
import argparse


# ---- minimal RGBA8 PNG writer (stdlib only) --------------------------------

def _png_chunk(tag, data):
    return (struct.pack(">I", len(data)) + tag + data +
            struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def write_png_rgba(path, width, height, rgba):
    """rgba: bytes of length width*height*4 in R,G,B,A order."""
    raw = bytearray()
    stride = width * 4
    for y in range(height):
        raw.append(0)  # filter type 0 (None)
        raw += rgba[y * stride:(y + 1) * stride]
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)  # 8-bit RGBA
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(_png_chunk(b"IHDR", ihdr))
        f.write(_png_chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        f.write(_png_chunk(b"IEND", b""))


# ---- TIM decode (mirrors parse_tim_to_rgba) --------------------------------

def _u16(data, o):
    return data[o] | (data[o + 1] << 8)


def _u32(data, o):
    return data[o] | (data[o + 1] << 8) | (data[o + 2] << 16) | (data[o + 3] << 24)


def _bgr555(cx, out, o):
    r5 = cx & 0x1F
    g5 = (cx >> 5) & 0x1F
    b5 = (cx >> 10) & 0x1F
    out[o + 0] = (r5 << 3) | (r5 >> 2)
    out[o + 1] = (g5 << 3) | (g5 >> 2)
    out[o + 2] = (b5 << 3) | (b5 >> 2)
    out[o + 3] = 0 if cx == 0 else 255


def clut_row_count(data):
    """Number of CLUT rows in a paletted TIM; 1 for 16/24bpp or a bad header."""
    if len(data) < 20 or data[0] != 0x10 or data[1] or data[2] or data[3]:
        return 1
    flags = _u32(data, 4)
    if not ((flags >> 3) & 1):
        return 1
    clut_h = _u16(data, 8 + 10)
    return clut_h if clut_h > 0 else 1


def decode_tim(data, clut_row=0):
    """Return (rgba_bytes, width, height, clut_rows). Raises ValueError on a bad TIM."""
    size = len(data)
    if size < 12:
        raise ValueError("file too small to be a TIM")
    if data[0] != 0x10 or data[1] or data[2] or data[3]:
        raise ValueError("not a TIM (bad magic)")

    flags = _u32(data, 4)
    bpp_code = flags & 0x7
    has_clut = bool((flags >> 3) & 1)
    src_bpp = {0: 4, 1: 8, 2: 16, 3: 24}.get(bpp_code)
    if src_bpp is None:
        raise ValueError("unsupported bit depth")

    p = 8
    clut_off = -1
    clut_w = clut_h = 0
    if has_clut:
        if p + 12 > size:
            raise ValueError("CLUT block truncated")
        block_len = _u32(data, p)
        clut_w = _u16(data, p + 8)
        clut_h = _u16(data, p + 10)
        clut_off = p + 12
        if block_len < 12 or p + block_len > size:
            raise ValueError("CLUT block overruns file")
        p += block_len

    clut_rows = clut_h if clut_h > 0 else 1
    if clut_row < 0 or clut_row >= clut_rows:
        clut_row = 0  # clamp, like the game

    if p + 12 > size:
        raise ValueError("pixel block truncated")
    pix_cell_w = _u16(data, p + 8)
    pix_h = _u16(data, p + 10)
    pix = p + 12

    if src_bpp == 4:
        pix_w = pix_cell_w * 4
    elif src_bpp == 8:
        pix_w = pix_cell_w * 2
    elif src_bpp == 16:
        pix_w = pix_cell_w
    else:
        pix_w = (pix_cell_w * 2) // 3
    if pix_w <= 0 or pix_h <= 0 or pix_w > 8192 or pix_h > 8192:
        raise ValueError("unusable dimensions")

    out = bytearray(pix_w * pix_h * 4)

    if src_bpp in (4, 8):
        if clut_off < 0:
            raise ValueError("paletted TIM without a CLUT")
        row_clut_base = clut_off + clut_row * clut_w * 2
        row_bytes = pix_cell_w * 2
        for y in range(pix_h):
            row = pix + y * row_bytes
            for x in range(pix_w):
                if src_bpp == 4:
                    byte_idx = x >> 1
                    if byte_idx >= row_bytes or row + byte_idx >= size:
                        idx = 0
                    else:
                        b = data[row + byte_idx]
                        idx = (b >> 4) & 0xF if (x & 1) else b & 0xF
                else:
                    idx = data[row + x] if (x < row_bytes and row + x < size) else 0
                if idx >= clut_w:
                    idx = 0
                ce = row_clut_base + idx * 2
                cx = _u16(data, ce) if ce + 1 < size else 0
                _bgr555(cx, out, (y * pix_w + x) * 4)
    elif src_bpp == 16:
        for y in range(pix_h):
            row = pix + y * pix_cell_w * 2
            for x in range(pix_w):
                pp = row + x * 2
                cx = _u16(data, pp) if pp + 1 < size else 0
                _bgr555(cx, out, (y * pix_w + x) * 4)
    else:  # 24bpp: 3 bytes/pixel (B,G,R), row stride pix_cell_w*2
        for y in range(pix_h):
            row = pix + y * pix_cell_w * 2
            for x in range(pix_w):
                pp = row + x * 3
                o = (y * pix_w + x) * 4
                out[o + 0] = data[pp] if pp < size else 0
                out[o + 1] = data[pp + 1] if pp + 1 < size else 0
                out[o + 2] = data[pp + 2] if pp + 2 < size else 0
                out[o + 3] = 255

    return bytes(out), pix_w, pix_h, clut_rows


# ---- conversion entry points -----------------------------------------------

def convert_file(tim_path, only_row=None):
    """Convert one .TIM to its PNG set. Returns the list of written paths."""
    with open(tim_path, "rb") as f:
        data = f.read()
    rows = clut_row_count(data)
    directory = os.path.dirname(tim_path) or "."
    stem = os.path.splitext(os.path.basename(tim_path))[0]
    full = os.path.basename(tim_path)  # e.g. DOB.TIM
    written = []

    if only_row is not None:
        rgba, w, h, _ = decode_tim(data, only_row)
        png = os.path.join(directory, "%s.p%02d.png" % (full, only_row))
        write_png_rgba(png, w, h, rgba)
        return [png]

    if rows <= 1:
        rgba, w, h, _ = decode_tim(data, 0)
        png = os.path.join(directory, stem + ".png")
        write_png_rgba(png, w, h, rgba)
        return [png]

    pad = 3 if rows > 100 else 2
    for r in range(rows):
        rgba, w, h, _ = decode_tim(data, r)
        png = os.path.join(directory, "%s.p%0*d.png" % (full, pad, r))
        write_png_rgba(png, w, h, rgba)
        written.append(png)
    return written


def main(argv):
    ap = argparse.ArgumentParser(
        description="Convert Silent Hill PSX TIM textures to PNG (per-palette for multi-CLUT TIMs).")
    ap.add_argument("paths", nargs="*", help="TIM file(s) to convert")
    ap.add_argument("--bulk", metavar="DIR", help="recursively convert every *.TIM under DIR, in place")
    ap.add_argument("--delete", action="store_true", help="with --bulk: delete each .TIM after converting it")
    ap.add_argument("--row", type=int, default=None, metavar="N",
                    help="write only CLUT row N (as FILE.TIM.pNN.png)")
    args = ap.parse_args(argv)

    targets = list(args.paths)
    if args.bulk:
        for root, _dirs, files in os.walk(args.bulk):
            for name in files:
                if name.lower().endswith(".tim"):
                    targets.append(os.path.join(root, name))

    if not targets:
        ap.print_help()
        return 1

    ok = fail = pngs = 0
    for tim in targets:
        try:
            written = convert_file(tim, only_row=args.row)
            ok += 1
            pngs += len(written)
            note = " (%d palettes)" % len(written) if len(written) > 1 else ""
            print("  %s -> %s%s" % (tim, ", ".join(os.path.basename(w) for w in written), note))
            if args.bulk and args.delete and args.row is None:
                try:
                    os.remove(tim)
                except OSError:
                    pass
        except Exception as ex:  # noqa: BLE001 — report and continue
            fail += 1
            print("  %s: FAILED — %s" % (tim, ex), file=sys.stderr)

    print("\nConverted %d file(s) to %d PNG(s); %d failed." % (ok, pngs, fail))
    return 0 if fail == 0 else 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
