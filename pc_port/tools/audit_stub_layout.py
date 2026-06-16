#!/usr/bin/env python3
"""Find zero-stub LAYOUT bugs: truncated struct stubs and lost symbol-overlap
aliases. These are NOT read-before-write ROM-table bugs (see audit_zero_stubs.py)
— they are wrong-SIZE / wrong-ADDRESS bugs that corrupt memory or split aliased
symbols, and neither the data_stubs default `u8 X[256]` nor a hand "next-symbol"
size catches them.

TRUNCATED: a stub `u8 X[N]` whose extern type is a struct with
  STATIC_ASSERT_SIZEOF(T, S) and N < S. Reads of X's tail fields run off the end
  of the array into the neighbouring stub (e.g. D_800E0698[0x258] vs sizeof 0x264
  -> the field_258 dust position read garbage).

OVERLAP: two symbols A and B (same map/overlay) where B's address lands strictly
  inside A's struct extent [addr(A), addr(A)+sizeof(A)). On PSX B is a sub-field
  of A (one memory, two names). As separate 64-bit stubs the alias is LOST: a
  write via B never reaches a read via A.field and vice-versa (e.g.
  D_800E08F0 == D_800E0698.field_258, the burrow dust position).
  Only flagged when B is still a live separate stub (not already macro-aliased).

Addresses come from the embedded hex in D_/sharedData_ names + the sym files.
Map grouping (to avoid cross-overlay false positives) is by the header file a
symbol is declared in.
"""
import re, pathlib
REPO = pathlib.Path(__file__).resolve().parents[2]
STUBS = REPO / "pc_port/src/stubs/data_stubs.c"
EXTRACTED = REPO / "pc_port/build_gen/extracted_data"
HDR_DIRS = [REPO / "include", REPO / "pc_port/include"]
SYM_DIR = REPO / "configs"

NAME_ADDR = re.compile(r"(?:D_|sharedData_)0*([0-9A-Fa-f]{6,8})")

def addr_of(name):
    m = NAME_ADDR.match(name)
    return int(m.group(1), 16) if m else None

# --- struct sizes from STATIC_ASSERT_SIZEOF ---
sizeof = {}
for d in HDR_DIRS:
    for p in d.rglob("*.h"):
        for m in re.finditer(r"STATIC_ASSERT_SIZEOF\(\s*([A-Za-z_]\w*)\s*,\s*(0x[0-9A-Fa-f]+|\d+)\s*\)",
                             p.read_text(errors="ignore")):
            sizeof[m.group(1)] = int(m.group(2), 0)

# --- extern decls: name -> (type, declared_count, header_path) ---
decl = {}
declre = re.compile(r"extern\s+([A-Za-z_][\w ]*?)\s*(\*?)\s*\b((?:D_|sharedData_|g_)[A-Za-z0-9_]+)\s*(\[\s*(\w*)\s*\])?\s*;")
for d in HDR_DIRS:
    for p in d.rglob("*.h"):
        txt = p.read_text(errors="ignore")
        for m in declre.finditer(txt):
            ty, star, name, arr, n = m.group(1).strip(), m.group(2), m.group(3), m.group(4), m.group(5)
            if name not in decl:
                cnt = int(n) if (arr and n and n.isdigit()) else (1 if not arr else None)
                decl[name] = (ty.split()[-1] if ty.split() else ty, p.name, bool(star), cnt)

# --- live zero-stubs with their declared byte size ---
stub_bytes = {}
for m in re.finditer(r"\bu8\s+((?:D_|sharedData_)[A-Za-z0-9_]+)\s*\[\s*(0x[0-9A-Fa-f]+|\d+)\s*\]\s*=\s*\{\s*0\s*\}",
                     STUBS.read_text(errors="ignore")):
    stub_bytes[m.group(1)] = int(m.group(2), 0)

# --- every known symbol address, grouped by header (≈ map/overlay) ---
sym_hdr = {}                         # name -> header file (group key)
addrs = {}                           # name -> addr
for name in set(stub_bytes) | set(decl):
    a = addr_of(name)
    if a is None: continue
    addrs[name] = a
    sym_hdr[name] = decl.get(name, (None, "stub-only", None, None))[1]
for sf in SYM_DIR.rglob("sym.*.txt"):
    for m in re.finditer(r"\b((?:D_|sharedData_)\w+)\s*=\s*0x([0-9A-Fa-f]+)", sf.read_text(errors="ignore")):
        addrs.setdefault(m.group(1), int(m.group(2), 16))
        sym_hdr.setdefault(m.group(1), sf.name)

def is_ptr(name):
    d = decl.get(name)
    return bool(d and d[2])

# fixed-width scalar element types whose u8[N] stub size is its own business.
# Anything else used as a stub's element type is a STRUCT -> work-buffer that was
# stubbed at a guessed 256 and may overflow on 64-bit (g_NpcBoneCoords class).
FIXED = {"u8","s8","char","u16","s16","short","u32","s32","int","long",
         "q3_12","q4_12","q19_12","q20_12","q23_8","q7_8",
         "VECTOR","VECTOR3","SVECTOR","SVECTOR3","DVECTOR","MATRIX"}
# structs known to contain pointers (or function pointers) -> PC size > PSX size.
def struct_has_ptr(ty):
    for d in HDR_DIRS:
        for p in d.rglob("*.h"):
            t = p.read_text(errors="ignore")
            m = re.search(r"typedef struct[^{]*\{(.*?)\}\s*"+re.escape(ty)+r"\s*;", t, re.S)
            if m: return "*" in m.group(1)
    return False

# ---------------- TRUNCATED ----------------
trunc, nullptr, structbuf = [], [], []
for name, nbytes in stub_bytes.items():
    ty, _, ptr, cnt = decl.get(name, (None, None, False, None))
    if ptr:                                  # `s_X* P` stubbed -> NULL/garbage pointer
        if ty in sizeof: nullptr.append((name, ty))
        continue
    if ty in sizeof:                         # definite: count*sizeof > stub
        full = (cnt or 1) * sizeof[ty]
        if nbytes < full:
            trunc.append((name, ty, nbytes, full, cnt))
    elif ty and ty not in FIXED:             # struct work-buffer, size unknown on PC
        # array, or a pointer-bearing struct -> 256 is a guess that can overflow.
        if (cnt and cnt != 1) or struct_has_ptr(ty):
            structbuf.append((name, ty, nbytes, cnt))

# ---------------- OVERLAP ----------------
# For each symbol A with a known struct sizeof, find other symbols B inside its
# extent. Same group (header/sym file) only. Flag harder if B is a live stub.
by_group = {}
for n, a in addrs.items():
    by_group.setdefault(sym_hdr.get(n), []).append((a, n))
overlaps = []
for name, a in addrs.items():
    ty = decl.get(name, (None,))[0]
    s = sizeof.get(ty)
    if not s or is_ptr(name): continue
    for b_addr, b_name in by_group.get(sym_hdr.get(name), []):
        if b_name == name: continue
        if a < b_addr < a + s:
            live = b_name in stub_bytes
            overlaps.append((name, ty, s, b_name, b_addr - a, live))

print("=== TRUNCATED struct stubs (stub smaller than count*sizeof -> OOB write/read) ===")
for name, ty, nb, s, cnt in sorted(trunc):
    arr = f"[{cnt}]" if cnt and cnt != 1 else ""
    print(f"  {name:24} u8[{nb}] but sizeof({ty}{arr})={s}  (+{s-nb}B OOB)")
if not trunc: print("  (none)")

print("\n=== STRUCT WORK-BUFFER stubs (struct/array stubbed u8[256]; PC size unknown")
print("    -> may overflow when the engine fills it; the g_NpcBoneCoords class) ===")
for name, ty, nb, cnt in sorted(structbuf):
    arr = f"[{cnt}]" if cnt else "[]"
    print(f"  {name:24} u8[{nb}]  extern {ty}{arr}  <{decl.get(name,(None,'?'))[1]}>")
if not structbuf: print("  (none)")

print("\n=== NULL-pointer stubs (s_X* P stubbed as u8[256] -> deref crash/garbage) ===")
for name, ty in sorted(nullptr):
    print(f"  {name:24} extern {ty}* (points nowhere)  <{decl.get(name,(None,'?'))[1]}>")
if not nullptr: print("  (none)")

print("\n=== OVERLAP aliases (B sits inside A's struct; split on 64-bit) ===")
seen = set()
for a_name, ty, s, b_name, off, live in sorted(overlaps):
    key = (a_name, b_name)
    if key in seen: continue
    seen.add(key)
    tag = "LIVE STUB -> alias LOST" if live else "(macro/handled or extracted)"
    print(f"  {b_name} @ {a_name}+0x{off:X} (in {ty}, sizeof 0x{s:X})  {tag}")
if not overlaps: print("  (none)")
print(f"\nstruct-sizes known: {len(sizeof)}  live u8 stubs: {len(stub_bytes)}  symbols: {len(addrs)}")
