"""Verify pc_port/src/map7_s03_boss_motion.c against the RETAIL map7_s03 overlay.

The reconstructed tables are named for their PSX addresses, so every one can be
checked byte-for-byte. Pointers (sh_scr.next_4) are checked as PSX addresses and
resolved to the symbol the C file points at.
"""
import re, struct, io, hashlib

REPO = r"C:/Claude/silenthill/silent-hill-decomp/"
DISC = REPO + "pc_port/build/gamedata/Silent Hill (USA).bin"
BASE = 0x800C9578          # USA map-overlay link base
LBA, BLOCKS = 0x09924, 698  # from src/main/filetable.c.USA.inc
RAW, OFF, DLEN = 2352, 24, 2048

# ---- pull the overlay off the disc -------------------------------------
buf = bytearray()
with open(DISC, "rb") as f:
    s = LBA
    while len(buf) < BLOCKS * 256:
        f.seek(s * RAW + OFF)
        buf += f.read(DLEN)
        s += 1
ovl = bytes(buf[: BLOCKS * 256])
print("overlay bytes:", len(ovl), "sha1:", hashlib.sha1(ovl).hexdigest())

def at(addr):
    o = addr - BASE
    assert 0 <= o < len(ovl), f"0x{addr:08X} outside overlay"
    return o

def i32(addr, n=1):
    o = at(addr)
    return list(struct.unpack_from("<%di" % n, ovl, o))

# ---- parse the C file --------------------------------------------------
src = io.open(REPO + "pc_port/src/map7_s03_boss_motion.c", encoding="utf-8").read()

def c_rows(name):
    """Rows of { ... } for a named array/scalar, as lists of tokens."""
    m = re.search(r"\b" + name + r"\s*(\[\d+\])?\s*=\s*", src)
    if not m:
        return None
    i = src.index("{", m.end())
    depth, j = 0, i
    while True:
        if src[j] == "{": depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0: break
        j += 1
    body = src[i:j + 1]
    inner = re.findall(r"\{([^{}]*)\}", body)
    if inner:
        return [[t.strip() for t in row.split(",") if t.strip()] for row in inner]
    return [[t.strip() for t in body.strip("{} \n").split(",") if t.strip()]]

fails = 0

# ---- flat int tables ---------------------------------------------------
for name, addr, count, width in [
    ("D_800EC018", 0x800EC018, 26, 4),
    ("D_800EC798", 0x800EC798, 7, 4),
    ("D_800EC34C", 0x800EC34C, 1, 4),
    ("D_800EC3EC", 0x800EC3EC, 1, 4),
]:
    rows = c_rows(name)
    if rows is None:
        print(f"  ?? {name}: not parsed"); continue
    disc = i32(addr, count * width)
    bad = 0
    for r, row in enumerate(rows[:count]):
        for c, tok in enumerate(row[:width]):
            try: want = int(tok, 0)
            except ValueError: continue
            got = disc[r * width + c]
            if want != got:
                bad += 1
                if bad <= 4:
                    print(f"  MISMATCH {name}[{r}].{c}: C={want} disc={got}")
    print(f"{'OK  ' if bad == 0 else 'FAIL'} {name}: {count} entries, {bad} mismatched")
    fails += bad

for name, addr, count in [("D_800EC758", 0x800EC758, 3), ("D_800EC764", 0x800EC764, 3)]:
    rows = c_rows(name)
    want = [int(t, 0) for t in rows[0]]
    got = i32(addr, count)
    ok = want == got
    print(f"{'OK  ' if ok else 'FAIL'} {name}: C={want} disc={got}")
    fails += 0 if ok else 1

# ---- sh_scr tables: {int field_0; void* next_4; int field_8} ------------
SYMS = {0x800EC018: "D_800EC018", 0x800EC1B8: "D_800EC1B8", 0x800EC34C: "D_800EC34C",
        0x800EC3EC: "D_800EC3EC", 0x800EC53C: "D_800EC53C", 0x800EC758: "D_800EC758",
        0x800EC764: "D_800EC764", 0x800EC798: "D_800EC798"}

def name_ptr(p):
    if p == 0: return "NULL"
    for a in sorted(SYMS, reverse=True):
        if p >= a:
            d = p - a
            return f"{SYMS[a]}+{d}" if d else SYMS[a]
    return f"0x{p:08X}"

for name, addr, count in [("D_800EC1B8", 0x800EC1B8, 27), ("D_800EC53C", 0x800EC53C, 45)]:
    rows = c_rows(name)
    if rows is None:
        print(f"  ?? {name}: not parsed"); continue
    print(f"--- {name}: {count} sh_scr entries (PSX stride 12)")
    bad = 0
    for r in range(count):
        f0, p4, f8 = struct.unpack_from("<iIi", ovl, at(addr + r * 12))
        if r >= len(rows): break
        row = rows[r]
        try: cf0 = int(row[0], 0)
        except (ValueError, IndexError): cf0 = None
        try: cf8 = int(row[2], 0)
        except (ValueError, IndexError): cf8 = None
        if cf0 is not None and cf0 != f0:
            bad += 1
            if bad <= 6: print(f"  MISMATCH {name}[{r}].field_0: C={cf0} disc={f0}")
        if cf8 is not None and cf8 != f8:
            bad += 1
            if bad <= 6: print(f"  MISMATCH {name}[{r}].field_8: C={cf8} disc={f8}")
        cptr = row[1] if len(row) > 1 else "?"
        want = name_ptr(p4)
        cnorm = cptr.replace("(void*)", "").replace("&", "").strip()
        m = re.match(r"([A-Za-z_]\w*)\[(\d+)\]", cnorm)
        if m:
            cnorm = f"{m.group(1)}+{int(m.group(2)) * (12 if m.group(1).startswith('D_800EC1B8') or m.group(1).startswith('D_800EC53C') else 16)}"
        if r < 8 or want != cnorm:
            flag = "  " if want == cnorm or cnorm.startswith("s_vel") else "<-"
            print(f"  {flag}[{r:2}] disc: f0={f0:<10} next=0x{p4:08X} ({want:<22}) f8={f8:<8} | C next={cptr}")
    print(f"{'OK  ' if bad == 0 else 'FAIL'} {name}: {bad} scalar mismatches")
    fails += bad

print("\nTOTAL scalar mismatches:", fails)
