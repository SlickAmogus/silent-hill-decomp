#!/usr/bin/env python3
"""Proactively find latent zero-stub bugs BEFORE players hit them.

Cross-references three sources:
  1. data_stubs.c  — symbols defined as `{0}` zero-stubs (the exe fallback)
  2. build_gen/extracted_data/*.c — symbols given real ROM values
  3. the decomp headers (include/**.h) — `extern <type> NAME[N];` declarations
     which carry the AUTHORITATIVE type and array size.

A zero-stub that is (a) never overridden by extracted data and (b) declared in
a header is a latent bug: the game reads ROM data that's all zeros. Classify by
how easily it can be auto-extracted:
  AUTO    — simple scalar/array of a fixed-width non-pointer type WITH an
            explicit [N] size in the header -> extract exactly N*sizeof bytes
            (no over-grab possible). Batch-fixable.
  PTR     — type contains a pointer / is a struct that may contain pointers ->
            needs manual PSX->PC relocation (e.g. boss motion scripts).
  UNSIZED — array declared `[]` (no N) -> size must be gap-inferred (the
            over-grab risk lives here; needs care).
  SCALAR  — single value; trivially AUTO if non-pointer.
"""
import re, pathlib
REPO = pathlib.Path(__file__).resolve().parents[2]
STUBS = REPO / "pc_port/src/stubs/data_stubs.c"
EXTRACTED = REPO / "pc_port/build_gen/extracted_data"
HDR_DIRS = [REPO / "include", REPO / "pc_port/include"]

SYM = r"(?:D_|s_|g_|sharedData_)[A-Za-z0-9_]+"
FIXED = {"u8","s8","char","u16","s16","short","u32","s32","int","long",
         "q3_12","q19_12","q20_12","q23_8","q4_12","VECTOR","VECTOR3","SVECTOR","SVECTOR3","DVECTOR"}
SIZEOF = {"u8":1,"s8":1,"char":1,"u16":2,"s16":2,"short":2,"q3_12":2,"q4_12":2,
          "u32":4,"s32":4,"int":4,"long":4,"q19_12":4,"q20_12":4,"q23_8":4,
          "SVECTOR":8,"SVECTOR3":6,"DVECTOR":4,"VECTOR":16,"VECTOR3":12}

def zero_stubs(t):
    return set(re.findall(r"\b("+SYM+r")\s*(?:\[[0-9]*\])?\s*=\s*\{\s*0\s*\}", t))
def defined(t):
    return set(re.findall(r"\b("+SYM+r")\s*(?:\[[0-9]*\])?\s*=\s*\{", t))

stubs = zero_stubs(STUBS.read_text(errors="ignore"))
extr = set()
for f in EXTRACTED.glob("*.c"):
    extr |= defined(f.read_text(errors="ignore"))
latent = stubs - extr

# header declarations: extern <type ...> NAME [N] ;
decl = {}   # name -> (type_str, count_or_None, is_ptr)
declre = re.compile(r"extern\s+([A-Za-z_][\w ]*?)\s*(\*?)\s*\b("+SYM+r")\s*(\[\s*(\d*)\s*\])?\s*;")
for d in HDR_DIRS:
    for p in d.rglob("*.h"):
        for m in declre.finditer(p.read_text(errors="ignore")):
            ty, star, name, arr, n = m.group(1).strip(), m.group(2), m.group(3), m.group(4), m.group(5)
            if name in decl: continue
            is_ptr = bool(star) or "*" in ty
            cnt = int(n) if (arr and n) else (None if arr else 1)
            decl[name] = (ty, cnt, is_ptr)

buckets = {"AUTO":[], "SCALAR":[], "UNSIZED":[], "PTR":[], "NO_HEADER":[]}
for s in sorted(latent):
    if s not in decl:
        buckets["NO_HEADER"].append((s,"")); continue
    ty, cnt, is_ptr = decl[s]
    base = ty.split()[-1] if ty.split() else ty
    if is_ptr or (base not in FIXED and base.startswith(("s_","struct"))):
        buckets["PTR"].append((s, f"{ty}{'*' if is_ptr else ''}")); continue
    if base not in FIXED:
        buckets["NO_HEADER"].append((s, ty)); continue   # unknown fixed type
    if cnt is None:
        buckets["UNSIZED"].append((s, f"{ty}[]")); continue
    sz = (cnt or 1) * SIZEOF.get(base, 0)
    label = "SCALAR" if cnt == 1 else "AUTO"
    buckets[label].append((s, f"{ty}[{cnt}] = {sz}B"))

print(f"latent zero-stubs (never extracted): {len(latent)}\n")
for b in ["AUTO","SCALAR","UNSIZED","PTR","NO_HEADER"]:
    items = buckets[b]
    print(f"==== {b}: {len(items)} ====")
    if b in ("AUTO","SCALAR"):
        for s, info in items[:60]:
            print(f"   {s:28} {info}")
print("\nAUTO+SCALAR = batch-extractable now with exact header sizes (no over-grab).")
print("PTR = manual relocation. UNSIZED = gap-infer carefully. NO_HEADER = needs a decl/type.")
