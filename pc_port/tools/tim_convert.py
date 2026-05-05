#!/usr/bin/env python3
"""
TIM ↔ PNG batch converter for Silent Hill PC port texture mods.

The PC port's allow_loose_files config option lets you drop replacement
TIMs into gamedata/load/{FOLDER}/{NAME}.TIM and have them override the
disc-image versions. This script extracts TIMs to editable PNGs (for
upscaling / repaint) and packs them back, preserving the original
TIM's pixel-mode and VRAM target coords so the in-game samplers find
the data at the same VRAM location.

Subcommands:

  extract  TIM → PNG (single file or directory tree)
  pack     PNG → TIM (single file or directory tree)
  info     dump TIM headers without writing anything

Usage:

  # Extract every TIM under disc_extract/ into a sibling tree of PNGs
  python tim_convert.py extract disc_extract/ extracted_pngs/

  # Pack edited PNGs back into TIMs at gamedata/load/ (any 16bpp PNG
  # works; 4bpp/8bpp source TIMs are re-encoded as 16bpp direct color
  # by default — uses more disc space but skips the CLUT quantize step).
  python tim_convert.py pack extracted_pngs/ gamedata/load/

  # Quick header dump
  python tim_convert.py info BG/ER_AQSH1.TIM

The pack step writes a sidecar `.TIM.json` next to each PNG on extract
that records the original VRAM target rect, CLUT rect, and pmode.
On re-pack, that sidecar tells us where to anchor the new TIM in
VRAM. If you delete or omit the sidecar, the new TIM uses the
extracted-from-PNG dimensions and zero VRAM coords (fine if your
replacement is the same size as the original at the same target).

Dependencies: pip install pillow
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    sys.stderr.write("Error: this tool needs Pillow. Install with:  pip install pillow\n")
    sys.exit(1)


# ---------- TIM format ----------
#
# Header (8 bytes):
#   u32  magic    — always 0x00000010
#   u32  flags    — bit 3 = CLUT present, bits 0-2 = pmode
#                     0 = 4bpp indexed (4 nibbles per u16 stored pixel)
#                     1 = 8bpp indexed (2 bytes per u16 stored pixel)
#                     2 = 16bpp BGR555  (1 pixel per u16)
#                     3 = 24bpp packed  (3 u16 = 6 bytes = 2 RGB triplets)
#
# Optional CLUT block (only if flags bit 3 set):
#   u32  block_size  — total bytes including this header
#   u16  x, u16 y    — VRAM coords for the CLUT
#   u16  w, u16 h    — CLUT size in u16 entries (typ. 16x1 for 4bpp,
#                      256x1 for 8bpp; some files split CLUTs across
#                      rows)
#   u16  pixels[w*h] — BGR555: bit15=STP, [14:10]=B, [9:5]=G, [4:0]=R
#
# Pixel block (always present):
#   u32  block_size  — total bytes including this header
#   u16  x, u16 y    — VRAM coords for the pixel data
#   u16  w           — width in u16 STORED pixels (multiply by 4 for
#                      4bpp / 2 for 8bpp / 1 for 16bpp / 1.5 for 24bpp
#                      to get the real image width in displayed pixels)
#   u16  h           — height in scanlines
#   u8   data[...]
#
# References used: PSY-Q libgpu docs, Wessel Stoop's TIM Tool source,
# OpenLara's tim_decoder.

PMODE_4BPP = 0
PMODE_8BPP = 1
PMODE_16BPP = 2
PMODE_24BPP = 3

PMODE_NAME = {0: "4bpp", 1: "8bpp", 2: "16bpp", 3: "24bpp"}


# ---------- helpers ----------

def bgr555_to_rgba(c: int) -> tuple[int, int, int, int]:
    """One TIM 16-bit BGR555+STP pixel → 8-bit RGBA tuple."""
    r5 = c & 0x1F
    g5 = (c >> 5) & 0x1F
    b5 = (c >> 10) & 0x1F
    stp = (c >> 15) & 1
    # 5→8 bit replication keeps full intensity range
    r = (r5 << 3) | (r5 >> 2)
    g = (g5 << 3) | (g5 >> 2)
    b = (b5 << 3) | (b5 >> 2)
    if c == 0 and stp == 0:
        # All zero with STP=0 is the standard "transparent" entry on
        # PSX; surface that as PNG alpha=0 so editors show it as
        # transparent.
        return (0, 0, 0, 0)
    return (r, g, b, 255)


def rgba_to_bgr555(r: int, g: int, b: int, a: int) -> int:
    """8-bit RGBA → TIM 16-bit BGR555+STP. Alpha=0 → all-zero pixel."""
    if a == 0:
        return 0
    r5 = (r >> 3) & 0x1F
    g5 = (g >> 3) & 0x1F
    b5 = (b >> 3) & 0x1F
    return r5 | (g5 << 5) | (b5 << 10)  # STP=0 (drawn)


def real_width(pmode: int, stored_w: int) -> int:
    """Convert TIM 'stored u16 width' to actual displayed-pixel width."""
    if pmode == PMODE_4BPP:
        return stored_w * 4
    if pmode == PMODE_8BPP:
        return stored_w * 2
    if pmode == PMODE_16BPP:
        return stored_w
    if pmode == PMODE_24BPP:
        # 3 u16 = 6 bytes = 2 RGB triplets, so 1.5x
        return (stored_w * 2) // 3
    raise ValueError(f"unknown pmode {pmode}")


def stored_width(pmode: int, real_w: int) -> int:
    """Convert real-pixel width back to 'stored u16 width' for the TIM."""
    if pmode == PMODE_4BPP:
        return (real_w + 3) // 4
    if pmode == PMODE_8BPP:
        return (real_w + 1) // 2
    if pmode == PMODE_16BPP:
        return real_w
    if pmode == PMODE_24BPP:
        return (real_w * 3 + 1) // 2
    raise ValueError(f"unknown pmode {pmode}")


# ---------- TIM parser ----------

class Tim:
    def __init__(self, raw: bytes, path: str = "<bytes>"):
        if len(raw) < 8:
            raise ValueError(f"{path}: too short to be a TIM ({len(raw)} bytes)")
        magic, flags = struct.unpack_from("<II", raw, 0)
        if magic != 0x10:
            raise ValueError(f"{path}: bad magic 0x{magic:X} (expected 0x10)")
        self.flags = flags
        self.pmode = flags & 0x07
        self.has_clut = bool(flags & 0x08)
        if self.pmode not in PMODE_NAME:
            raise ValueError(f"{path}: unknown pmode {self.pmode}")

        offset = 8

        if self.has_clut:
            if len(raw) < offset + 12:
                raise ValueError(f"{path}: truncated CLUT header")
            clut_blk = struct.unpack_from("<I", raw, offset)[0]
            self.clut_x, self.clut_y, self.clut_w, self.clut_h = struct.unpack_from(
                "<HHHH", raw, offset + 4
            )
            clut_data_size = self.clut_w * self.clut_h * 2
            data_offset = offset + 12
            if len(raw) < data_offset + clut_data_size:
                raise ValueError(f"{path}: truncated CLUT data")
            self.clut = list(
                struct.unpack_from(f"<{self.clut_w * self.clut_h}H", raw, data_offset)
            )
            offset += clut_blk
        else:
            self.clut_x = self.clut_y = self.clut_w = self.clut_h = 0
            self.clut = None

        # pixel block
        if len(raw) < offset + 12:
            raise ValueError(f"{path}: truncated pixel header")
        pix_blk = struct.unpack_from("<I", raw, offset)[0]
        self.pix_x, self.pix_y, self.pix_stored_w, self.pix_h = struct.unpack_from(
            "<HHHH", raw, offset + 4
        )
        self.real_w = real_width(self.pmode, self.pix_stored_w)
        pix_data_size = self.pix_stored_w * self.pix_h * 2
        if len(raw) < offset + 12 + pix_data_size:
            raise ValueError(f"{path}: truncated pixel data")
        self.pix_bytes = bytes(raw[offset + 12 : offset + 12 + pix_data_size])

    # ---- decoders ----

    def decode_to_image(self) -> Image.Image:
        """Render the TIM to a Pillow Image (RGBA)."""
        W, H = self.real_w, self.pix_h
        out = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        px = out.load()

        if self.pmode == PMODE_4BPP:
            if self.clut is None:
                raise ValueError("4bpp TIM without CLUT")
            # Each byte = two 4-bit indices, low nibble first.
            for y in range(H):
                for x in range(W):
                    byte_idx = (y * self.pix_stored_w * 2) + (x // 2)
                    raw_byte = self.pix_bytes[byte_idx]
                    nibble = (raw_byte >> ((x & 1) * 4)) & 0x0F
                    # CLUT might have multiple rows — for 4bpp the
                    # game picks a row at runtime via clutY+offset;
                    # for export we use row 0 (the first 16 entries).
                    px[x, y] = bgr555_to_rgba(self.clut[nibble])
        elif self.pmode == PMODE_8BPP:
            if self.clut is None:
                raise ValueError("8bpp TIM without CLUT")
            for y in range(H):
                row_base = y * self.pix_stored_w * 2
                for x in range(W):
                    idx = self.pix_bytes[row_base + x]
                    if idx >= len(self.clut):
                        idx = 0
                    px[x, y] = bgr555_to_rgba(self.clut[idx])
        elif self.pmode == PMODE_16BPP:
            for y in range(H):
                for x in range(W):
                    pi = (y * self.pix_stored_w + x) * 2
                    c = self.pix_bytes[pi] | (self.pix_bytes[pi + 1] << 8)
                    px[x, y] = bgr555_to_rgba(c)
        elif self.pmode == PMODE_24BPP:
            for y in range(H):
                for x in range(W):
                    bi = (y * self.pix_stored_w * 2) + x * 3
                    if bi + 2 >= len(self.pix_bytes):
                        break
                    r, g, b = self.pix_bytes[bi : bi + 3]
                    px[x, y] = (r, g, b, 255)
        return out

    def info_dict(self) -> dict:
        d = {
            "pmode": self.pmode,
            "pmode_name": PMODE_NAME[self.pmode],
            "has_clut": self.has_clut,
            "pixel_rect": [self.pix_x, self.pix_y, self.real_w, self.pix_h],
            "pixel_stored_w": self.pix_stored_w,
        }
        if self.has_clut:
            d["clut_rect"] = [self.clut_x, self.clut_y, self.clut_w, self.clut_h]
        return d


# ---------- PNG → TIM encoder ----------

def encode_png_to_tim_16bpp(img: Image.Image, vram_x: int, vram_y: int) -> bytes:
    """Pack an arbitrary PNG into a 16bpp direct-color TIM at (vram_x, vram_y).

    16bpp avoids the lossy quantize-back-to-N-colors step that 4bpp and
    8bpp re-encoding would require. The trade-off is file size (2 bytes
    per pixel vs. 0.5 / 1) and VRAM footprint. For texture mods the
    PC port's allow_loose_files loader doesn't care about either.
    """
    img = img.convert("RGBA")
    W, H = img.size
    px = img.load()
    pixel_bytes = bytearray(W * H * 2)
    for y in range(H):
        for x in range(W):
            r, g, b, a = px[x, y]
            c = rgba_to_bgr555(r, g, b, a)
            off = (y * W + x) * 2
            pixel_bytes[off] = c & 0xFF
            pixel_bytes[off + 1] = (c >> 8) & 0xFF

    pix_block = (
        struct.pack("<I", 12 + len(pixel_bytes))      # block length
        + struct.pack("<HH", vram_x, vram_y)          # VRAM rect xy
        + struct.pack("<HH", W, H)                     # stored w (=W for 16bpp), h
        + bytes(pixel_bytes)
    )

    # flags = 0 (no CLUT) | pmode 2 (16bpp)
    header = struct.pack("<II", 0x10, PMODE_16BPP)
    return header + pix_block


# ---------- file walking ----------

def walk_tims(root: Path):
    if root.is_file():
        yield root
        return
    for p in root.rglob("*"):
        if p.is_file() and p.suffix.upper() == ".TIM":
            yield p


def walk_pngs(root: Path):
    if root.is_file():
        yield root
        return
    for p in root.rglob("*"):
        if p.is_file() and p.suffix.lower() == ".png":
            yield p


# ---------- subcommands ----------

def cmd_info(args):
    for tim_path in walk_tims(Path(args.input)):
        try:
            tim = Tim(tim_path.read_bytes(), str(tim_path))
        except ValueError as e:
            print(f"  SKIP {tim_path}: {e}")
            continue
        d = tim.info_dict()
        print(f"{tim_path}  {d['pmode_name']:>5}  pix=({d['pixel_rect'][0]},{d['pixel_rect'][1]}) {d['pixel_rect'][2]}x{d['pixel_rect'][3]}", end="")
        if d.get("has_clut"):
            cr = d["clut_rect"]
            print(f"  clut=({cr[0]},{cr[1]}) {cr[2]}x{cr[3]}")
        else:
            print()


def cmd_extract(args):
    in_root = Path(args.input)
    if args.output is None:
        # default: <input>_pngs alongside the input (works for single
        # files too — `foo.TIM` → `foo_pngs/foo.png`)
        out_root = in_root.with_name(in_root.stem + "_pngs") if in_root.is_file() \
                   else in_root.parent / (in_root.name + "_pngs")
        print(f"  (output not specified, using {out_root})")
    else:
        out_root = Path(args.output)

    n_ok = 0
    n_skip = 0
    for tim_path in walk_tims(in_root):
        try:
            tim = Tim(tim_path.read_bytes(), str(tim_path))
            img = tim.decode_to_image()
        except ValueError as e:
            print(f"  SKIP {tim_path}: {e}")
            n_skip += 1
            continue

        rel = tim_path.relative_to(in_root) if in_root.is_dir() else Path(tim_path.name)
        png_path = out_root / rel.with_suffix(".png")
        json_path = out_root / rel.with_suffix(".TIM.json")
        png_path.parent.mkdir(parents=True, exist_ok=True)

        img.save(png_path)
        with json_path.open("w") as f:
            json.dump(tim.info_dict(), f, indent=2)

        print(f"  {tim_path}  →  {png_path}  ({tim.real_w}x{tim.pix_h} {PMODE_NAME[tim.pmode]})")
        n_ok += 1

    print(f"\nextracted {n_ok}, skipped {n_skip}")


def cmd_pack(args):
    in_root = Path(args.input)
    if args.output is None:
        out_root = in_root.with_name(in_root.stem + "_tims") if in_root.is_file() \
                   else in_root.parent / (in_root.name + "_tims")
        print(f"  (output not specified, using {out_root})")
    else:
        out_root = Path(args.output)

    n_ok = 0
    n_skip = 0
    for png_path in walk_pngs(in_root):
        rel = png_path.relative_to(in_root) if in_root.is_dir() else Path(png_path.name)
        tim_path = out_root / rel.with_suffix(".TIM")
        json_path = png_path.with_suffix(".TIM.json")

        # Recover the original VRAM coords from the sidecar so the
        # game's tpage/UV/CLUT references still hit the right region.
        vram_x = vram_y = 0
        if json_path.exists():
            try:
                meta = json.loads(json_path.read_text())
                rect = meta.get("pixel_rect")
                if rect and len(rect) >= 2:
                    vram_x, vram_y = rect[0], rect[1]
            except Exception as e:
                print(f"  WARN  bad sidecar {json_path}: {e}")

        try:
            img = Image.open(png_path)
            tim_bytes = encode_png_to_tim_16bpp(img, vram_x, vram_y)
        except Exception as e:
            print(f"  SKIP {png_path}: {e}")
            n_skip += 1
            continue

        tim_path.parent.mkdir(parents=True, exist_ok=True)
        tim_path.write_bytes(tim_bytes)
        print(f"  {png_path}  →  {tim_path}  (16bpp @ ({vram_x},{vram_y}))")
        n_ok += 1

    print(f"\npacked {n_ok}, skipped {n_skip}")


# ---------- main ----------

def main():
    ap = argparse.ArgumentParser(description="TIM ↔ PNG batch converter")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_info = sub.add_parser("info", help="dump TIM headers")
    p_info.add_argument("input", help="TIM file or directory tree")
    p_info.set_defaults(func=cmd_info)

    p_ex = sub.add_parser("extract", help="TIM tree → PNG tree (+ sidecar metadata)")
    p_ex.add_argument("input", help="root TIM directory or single .TIM")
    p_ex.add_argument("output", nargs="?", default=None,
                      help="output dir (default: <input>_pngs alongside input)")
    p_ex.set_defaults(func=cmd_extract)

    p_pk = sub.add_parser("pack", help="PNG tree → TIM tree (16bpp)")
    p_pk.add_argument("input", help="root PNG directory or single .png")
    p_pk.add_argument("output", nargs="?", default=None,
                      help="output dir (default: <input>_tims alongside input)")
    p_pk.set_defaults(func=cmd_pack)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
