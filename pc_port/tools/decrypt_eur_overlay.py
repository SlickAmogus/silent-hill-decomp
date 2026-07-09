#!/usr/bin/env python3
"""Decrypt an encrypted `1ST/` overlay (BODYPROG.BIN, B_KONAMI.BIN) straight out
of a raw CD image (.bin, 2352-byte sectors).

What the on-disc "scrambling" actually is
-----------------------------------------
It is REAL XOR obfuscation of the file payload, not an artifact of raw-sector
framing: the overlay's sectors are ordinary Mode 2 Form 1 (12-byte sync,
4-byte header, 8-byte subheader with submode 0x08, 2048 data bytes at +24),
and the keystream matches neither the ECMA-130 sector scrambler nor any
per-sector pattern.

The algorithm is the game's own `Fs_DecryptOverlay` (src/main/fileinfo.c):

    seed = 0;
    for each little-endian u32 word of the file:
        seed = (seed + 0x01309125) * 0x03A452F7;   // mod 2^32
        plaintext_word = disc_word ^ seed;

The keystream is one continuous LCG stream over the whole file (it does NOT
restart per sector). Verified empirically: the keystream derived as
(US plaintext 1ST/BODYPROG.BIN) XOR (US disc copy at LBA 0xCF) matches this
LCG for all 674560 bytes, with zero mismatch regions. A previously reported
"garbage region near 0x40000" does not exist against a good dump; any such
garbage would indicate a bad rip, not the cipher.

Defaults decrypt the PAL (SLES-01514) BODYPROG.BIN: LBA 0xD0, 2613 blocks
(block = 0x100 bytes -> 668928 bytes), per src/main/filetable.c.EUR.inc.

Usage:
    python decrypt_eur_overlay.py [--bin DISC.bin] [--lba 0xD0]
                                  [--blocks 2613] [--out BODYPROG_EUR.BIN]
"""

import argparse
import os
import struct
import sys

RAW_SECTOR = 2352
DATA_OFFSET = 24  # Mode 2 Form 1: sync(12) + header(4) + subheader(8)
DATA_SIZE = 2048
FS_BLOCK_SIZE = 0x100

REPO_ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
DEFAULT_BIN = os.path.join(REPO_ROOT, "pc_port", "build", "gamedata", "Silent Hill (PAL).bin")


def fs_decrypt_overlay(data: bytes) -> bytes:
    words = struct.unpack("<%dI" % (len(data) // 4), data)
    out = []
    seed = 0
    for w in words:
        seed = ((seed + 0x01309125) * 0x03A452F7) & 0xFFFFFFFF
        out.append(w ^ seed)
    return struct.pack("<%dI" % len(out), *out)


def read_file_from_bin(bin_path: str, lba: int, size: int) -> bytes:
    n_sectors = (size + DATA_SIZE - 1) // DATA_SIZE
    buf = bytearray()
    with open(bin_path, "rb") as f:
        f.seek(lba * RAW_SECTOR)
        raw = f.read(n_sectors * RAW_SECTOR)
    if len(raw) != n_sectors * RAW_SECTOR:
        sys.exit("error: disc image too small for requested sectors")
    for i in range(n_sectors):
        sector = raw[i * RAW_SECTOR : (i + 1) * RAW_SECTOR]
        if sector[:12] != b"\x00" + b"\xff" * 10 + b"\x00":
            sys.exit("error: bad sync at LBA 0x%X — not a raw 2352-byte image?" % (lba + i))
        buf += sector[DATA_OFFSET : DATA_OFFSET + DATA_SIZE]
    return bytes(buf[:size])


def validate(plain: bytes) -> None:
    ids = [tag for tag in (b"SLES", b"SLUS", b"SLPM", b"SLPS") if tag in plain]
    # A real MIPS code section is dominated by a few opcode families; count
    # common primary opcodes in the first 256 KB as a cheap plausibility check.
    common = 0
    total = 0
    for (w,) in struct.iter_unpack("<I", plain[: 256 * 1024]):
        op = w >> 26
        total += 1
        if op in (0, 2, 3, 4, 5, 8, 9, 12, 13, 15, 32, 33, 35, 36, 37, 40, 41, 43) or w == 0:
            common += 1
    print("validation: region-ID strings found: %s" % ([s.decode() for s in ids] or "NONE"))
    print("validation: common-MIPS-opcode ratio (first 256KB): %.1f%%" % (100.0 * common / total))
    if not ids or common / total < 0.5:
        print("warning: output does not look like a decrypted PSX overlay", file=sys.stderr)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--bin", default=DEFAULT_BIN, help="raw 2352-byte-sector disc image")
    ap.add_argument("--lba", type=lambda s: int(s, 0), default=0xD0,
                    help="start sector of the encrypted overlay (default: 0xD0, EUR BODYPROG)")
    ap.add_argument("--blocks", type=lambda s: int(s, 0), default=2613,
                    help="file size in 0x100-byte blocks from filetable (default: 2613)")
    ap.add_argument("--out", default="BODYPROG_EUR.BIN", help="output path for the decrypted overlay")
    args = ap.parse_args()

    size = args.blocks * FS_BLOCK_SIZE
    cipher = read_file_from_bin(args.bin, args.lba, size)
    plain = fs_decrypt_overlay(cipher)
    with open(args.out, "wb") as f:
        f.write(plain)
    print("wrote %s (%d bytes) from LBA 0x%X" % (args.out, len(plain), args.lba))
    validate(plain)


if __name__ == "__main__":
    main()
