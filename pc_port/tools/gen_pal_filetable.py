#!/usr/bin/env python3
"""
Generate src/main/filetable.c.EUR.inc + include/main/fileenum.h.EUR.inc from a
PAL (SLES-01514) Silent Hill BIN/CUE disc image.

The 2310-entry file index (g_FileTable) lives in the PSX executable, not the
ISO9660 directory (which only holds SYSTEM.CNF, the EXE, and the SILENT./HILL.
containers). Each entry is a 12-byte s_FileInfo:
    word0: startSector:19 | blockCount:12
    word1: pathIdx:4      | name0123:24
    word2: name4567:24    | type:4
Names are 8 six-bit chars (ASCII range 0x20..0x5F). pathIdx -> g_FilePaths,
type -> g_FileExts (type>=12 means no extension, e.g. XA streams).

PAL differs from USA: g_FilePaths gains VIN2..VIN5 (per-language localized map
overlays) so XA moves to index 14, and the table is 2310 entries (vs 2074).
Validated: decoding the USA EXE this way reproduces filetable.c.USA.inc and
fileenum.h.USA.inc byte-for-byte.

Usage:  python gen_pal_filetable.py "C:\\path\\Silent Hill (Europe) ....bin"
"""
import struct, sys
from pathlib import Path

SECT, DOFF, DLEN = 2352, 24, 2048
EXE_LBA = 0x18          # SLES_015.14 / SLUS_007.07 start sector
TABLE_OFF = 0xb8fc      # g_FileTable offset within the EXE image (PAL); USA=0xb91c
EXTS = ["TIM","VAB","BIN","DMS","ANM","PLM","IPD","ILM","TMD","DAT","KDT","CMP"]
REPO = Path(__file__).resolve().parents[2]

def _rdsec(f, lba):
    f.seek(lba*SECT + DOFF); return f.read(DLEN)

def read_exe(bin_path):
    """Read exactly the PSX executable (SLES_*/SLUS_*) using its ISO9660 size,
    so the path-string scan can't run past the EXE into the SILENT. container."""
    with open(bin_path, "rb") as f:
        pvd = _rdsec(f, 16)
        root = pvd[156:156+34]
        rlba = struct.unpack_from("<I", root, 2)[0]
        rsz  = struct.unpack_from("<I", root, 10)[0]
        dirdata = b"".join(_rdsec(f, rlba+i) for i in range((rsz+DLEN-1)//DLEN))
        exe_sz = None; o = 0
        while o < len(dirdata):
            L = dirdata[o]
            if L == 0: o = ((o//DLEN)+1)*DLEN; continue
            nl = dirdata[o+32]; nm = dirdata[o+33:o+33+nl].split(b';')[0]
            if nm.startswith(b"SLES") or nm.startswith(b"SLUS"):
                exe_sz = struct.unpack_from("<I", dirdata, o+10)[0]
            o += L
        if exe_sz is None: exe_sz = 0x15000
        out = bytearray()
        for i in range((exe_sz + DLEN - 1)//DLEN): out += _rdsec(f, EXE_LBA+i)
        return bytes(out[:exe_sz])

def find_paths(exe):
    """Extract \\NAME\\ path strings in EXE order; g_FilePaths is their reverse."""
    seen, i, n = [], 0, len(exe)
    while i < n:
        if exe[i] == 0x5c:
            j, s = i+1, b""
            while j < n and (65<=exe[j]<=90 or 48<=exe[j]<=57 or exe[j]==0x5f) and len(s)<8:
                s += bytes([exe[j]]); j += 1
            if j < n and exe[j] == 0x5c and len(s) >= 2:
                tok = s.decode("latin1")
                if tok not in seen: seen.append(tok)
                i = j; continue
        i += 1
    return list(reversed(seen))   # EXE stores them high->low; g_FilePaths is 1ST..XA

def parse(b, o):
    w0, w1, w2 = struct.unpack_from("<III", b, o)
    name8 = "".join(chr(((v>>(6*k))&0x3f)+0x20)
                    for v in ((w1>>4)&0xFFFFFF, w2&0xFFFFFF) for k in range(4))
    return (w0&0x7FFFF, (w0>>19)&0xFFF, w1&0xF, (w2>>24)&0xF, name8)

def decode(exe, paths):
    rows, i, prev = [], 0, -1
    while i < 3000:
        ss, bc, pi, ty, n8 = parse(exe, TABLE_OFF + 12*i)
        nm = n8.rstrip()
        if not nm or not all(0x20<=ord(c)<=0x5f for c in nm) or pi>=len(paths) or ss<prev:
            break                       # table ss is monotonic; a drop = past-end garbage
        rows.append((ss,bc,pi,ty,n8)); prev = ss; i += 1
    return rows

def pathname(P,pi,ty,n8):
    nm=n8.rstrip(); ext=EXTS[ty] if ty<len(EXTS) else None
    return f"{P[pi]}/{nm}" + (f".{ext}" if ext else "")
def enumname(P,pi,ty,n8):
    nm=n8.rstrip(); ext=EXTS[ty] if ty<len(EXTS) else None
    return (f"FILE_{P[pi]}_{nm}" + (f"_{ext}" if ext else "")).replace(" ","_").replace("-","_")
def fnchars(n8): return ",".join("'%s'"%c for c in n8)

def main():
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(1)
    exe = read_exe(sys.argv[1])
    paths = find_paths(exe)
    rows = decode(exe, paths)
    print(f"paths ({len(paths)}): {paths}")
    print(f"decoded {len(rows)} entries: {rows[0][4].rstrip()} .. {rows[-1][4].rstrip()}")

    ft = ["// Generated from SLES-01514 (PAL) by pc_port/tools/gen_pal_filetable.py"]
    for i,(ss,bc,pi,ty,n8) in enumerate(rows):
        ft.append(f"/* {i:4d} */ {{ 0x{ss:05x}, {bc:4d}, {pi:2d}, FN({fnchars(n8)}), {ty:2d} }}, // {pathname(paths,pi,ty,n8)}")
    (REPO/"src/main/filetable.c.EUR.inc").write_text("\n".join(ft)+"\n", newline="\n")

    seen, el = {}, ["    // Generated from SLES-01514 (PAL) by pc_port/tools/gen_pal_filetable.py"]
    for i,(ss,bc,pi,ty,n8) in enumerate(rows):
        nm = enumname(paths,pi,ty,n8)
        if nm in seen: nm = f"{nm}_{i}"
        seen[nm]=1
        el.append(f"    {nm} = {i}, // {pathname(paths,pi,ty,n8)}")
    (REPO/"include/main/fileenum.h.EUR.inc").write_text("\n".join(el)+"\n", newline="\n")
    print(f"wrote filetable.c.EUR.inc + fileenum.h.EUR.inc ({len(rows)} entries)")

if __name__ == "__main__":
    main()
