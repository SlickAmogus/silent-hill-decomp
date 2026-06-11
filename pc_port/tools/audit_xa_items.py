#!/usr/bin/env python3
"""Audit g_XaItemData against the disc image.

For each XA voice entry, read the raw sector at (g_FileXaLoc[fileIdx] +
sector_4) from the BIN and compare its subheader (file, channel) with the
table's authoritative filter (field_8_24, field_4_24) — the values PSX
passes to CdlSetfilter. The PC xa_player derives its software filter from
the first sector's subheader, so every mismatch here is a voice line that
plays the WRONG channel (usually silence) on PC.

Usage: python audit_xa_items.py [path-to-BIN]
"""
import re
import sys

BIN_PATH = sys.argv[1] if len(sys.argv) > 1 else r"C:\Claude\silenthill\Silent Hill (USA).bin"
SOUND_DATA = r"C:\Claude\silenthill\silent-hill-decomp\src\bodyprog\sound\sound_data.c"

FILE_XA_LOC = [0x00000, 0x099BF, 0x0A227, 0x0B377, 0x0D0BF, 0x0EA57,
               0x0F997, 0x1096F, 0x16F07, 0x19797, 0x00000]

SECTOR = 2352
HDR = 16

rows = []
with open(SOUND_DATA, "r", encoding="utf-8", errors="replace") as f:
    src = f.read()
m = re.search(r"g_XaItemData\[727\]\s*=\s*\{(.*?)\n\};", src, re.S)
body = m.group(1)
for rm in re.finditer(r"\{\s*([\d,\s-]+?)\s*\}", body):
    nums = [int(x) for x in rm.group(1).split(",")]
    rows.append(nums)

print(f"parsed {len(rows)} rows")

bin_f = open(BIN_PATH, "rb")

def read_subheader(lba):
    bin_f.seek(lba * SECTOR + HDR)
    return bin_f.read(8)

mismatch = 0
nulls = 0
checked = 0
worst_scan = 0
report = []
for idx, r in enumerate(rows):
    fileIdx, _, _, _, sector, chan, length, ffile = r
    if fileIdx < 1 or fileIdx > 9:
        continue
    checked += 1
    lba = FILE_XA_LOC[fileIdx] + sector
    sh = read_subheader(lba)
    ok = (sh[0] == ffile and sh[1] == chan)
    if not ok:
        mismatch += 1
        if sh[0] == 0 and sh[1] == 0 and sh[2] == 0:
            nulls += 1
        # scan forward for first matching sector
        dist = -1
        for k in range(1, 64):
            sh2 = read_subheader(lba + k)
            if sh2[0] == ffile and sh2[1] == chan:
                dist = k
                break
        worst_scan = max(worst_scan, dist)
        report.append((idx, fileIdx, sector, ffile, chan, sh[0], sh[1], sh[2], dist))

print(f"checked {checked} entries: {mismatch} first-sector mismatches ({nulls} null sectors)")
print(f"worst forward-scan distance to matching sector: {worst_scan}")
for e in report[:40]:
    print(" xaIdx=%3d file=%d sector=%5d want=(%d,%d) got=(%d,%d) submode=0x%02X firstMatch=+%d" % e)
if len(report) > 40:
    print(f" ... and {len(report)-40} more")
