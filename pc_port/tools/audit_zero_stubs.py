#!/usr/bin/env python3
"""Classify never-overridden zero-stubs by how game code touches them, to surface
latent read-before-write const-table bugs (puzzle solutions, dispatch tables,
texture-ID/position tables) before players hit them.

A zero-stub is a likely BUG if code READS/INDEXES/CALLS it without ever writing
it (it expects real ROM data). It is likely FINE if code writes it before reading
(runtime work/scratch var) — zero-init is correct there.

Usage: python audit_zero_stubs.py   (prints ranked candidates)
"""
import re, sys, pathlib

REPO = pathlib.Path(__file__).resolve().parents[2]
STUBS = REPO / "pc_port" / "src" / "stubs" / "data_stubs.c"
EXTRACTED = REPO / "pc_port" / "build_gen" / "extracted_data"
SRC_DIRS = [REPO / "src", REPO / "include"]

SYM = r"(?:D_|s_|g_|sharedData_|D_8|jtbl_)[A-Za-z0-9_]+"

def zero_stub_syms(text):
    # `<type> NAME[...] = {0};`  or `= { 0 }`
    out = set()
    for m in re.finditer(r"\b(" + SYM + r")\s*(\[[0-9]*\])?\s*=\s*\{\s*0\s*\}", text):
        out.add(m.group(1))
    return out

def defined_syms(text):
    out = set()
    for m in re.finditer(r"\b(" + SYM + r")\s*(\[[0-9]*\])?\s*=\s*\{", text):
        out.add(m.group(1))
    return out

stub_text = STUBS.read_text(errors="ignore")
stubs = zero_stub_syms(stub_text)
extracted = set()
for f in EXTRACTED.glob("*.c"):
    extracted |= defined_syms(f.read_text(errors="ignore"))
latent = sorted(stubs - extracted)

# Load all game source (exclude stubs file + extracted)
files = []
for d in SRC_DIRS:
    files += [p for p in d.rglob("*.c")] + [p for p in d.rglob("*.h")]
blobs = {p: p.read_text(errors="ignore") for p in files}

# Inverted index: symbol -> concatenated text of only the files that mention it.
latent_set = set(latent)
persym = {s: [] for s in latent_set}
tok = re.compile(r"\b(" + SYM + r")\b")
for txt in blobs.values():
    present = latent_set & set(tok.findall(txt))
    for s in present:
        persym[s].append(txt)
persym = {s: "\n".join(v) for s, v in persym.items()}

def classify(sym):
    b = re.escape(sym)
    t = persym.get(sym, "")
    writes = len(re.findall(rf"\b{b}\b\s*(?:\[[^\]\n]*\])?\s*=(?!=)", t))
    addrof = len(re.findall(rf"&\s*{b}\b", t))
    calls  = len(re.findall(rf"\b{b}\s*\(", t))
    cmps   = len(re.findall(rf"(?:\b{b}\b\s*(?:==|!=|<|>|<=|>=))|(?:(?:==|!=|<|>|<=|>=)\s*{b}\b)", t))
    idxrd  = len(re.findall(rf"\b{b}\s*\[", t))
    refs   = len(re.findall(rf"\b{b}\b", t))
    # header decl
    decl = ""
    for m in re.finditer(rf"extern[^\n;]*\b{b}\b[^\n;]*;", t):
        decl = m.group(0); break
    is_const = "const" in decl
    aliased = bool(re.search(rf"#\s*define\s+{b}\b", t))  # lost-poke #define -> stub dead
    # written-through-pointer: taken by address a lot (init/memset funcs) -> work struct, not ROM table
    ptr_written = addrof >= 3 or (addrof >= 1 and writes == 0 and "[" not in decl)
    reasons = []
    risk = 0
    if aliased:
        risk = 5; reasons.append(f"#define-aliased (stub dead, real value via macro)")
    elif calls > 0:
        risk = 95; reasons.append(f"CALLED as function ({calls}x) -> zero ptr crash/no-op")
    elif ptr_written:
        risk = 25; reasons.append(f"&taken {addrof}x, writes={writes} -> likely pointer-initialised work struct")
    elif is_const:
        risk = 88; reasons.append("declared const (read-only ROM table) but stubbed 0")
    elif cmps > 0 and writes == 0:
        risk = 85; reasons.append(f"COMPARED ({cmps}x) and never written -> expected ROM value")
    elif idxrd > 0 and writes == 0:
        risk = 78; reasons.append(f"INDEXED read ({idxrd}x) and never written -> ROM table")
    elif writes == 0 and refs > 0:
        risk = 60; reasons.append(f"read ({refs}x) and never written")
    elif idxrd > writes and writes > 0:
        risk = 40; reasons.append(f"indexed {idxrd}x vs {writes} writes (mixed)")
    else:
        risk = 10; reasons.append(f"written {writes}x (likely work var) refs={refs}")
    if addrof and not aliased and not ptr_written:
        reasons.append(f"&taken {addrof}x")
    return risk, refs, reasons, decl.strip()[:90]

rows = []
for s in latent:
    risk, refs, reasons, decl = classify(s)
    if refs == 0:
        continue  # unreferenced in game code we can see
    rows.append((risk, s, refs, "; ".join(reasons), decl))

rows.sort(key=lambda r: (-r[0], r[1]))
print(f"latent zero-stubs: {len(latent)}  referenced-in-source: {len(rows)}\n")
buckets = {}
for risk, s, refs, why, decl in rows:
    band = "HIGH(85+)" if risk >= 85 else "MED-HIGH(70-84)" if risk >= 70 else "MED(40-69)" if risk >= 40 else "LOW(<40)"
    buckets.setdefault(band, []).append((s, risk, why, decl))
for band in ["HIGH(85+)", "MED-HIGH(70-84)", "MED(40-69)", "LOW(<40)"]:
    items = buckets.get(band, [])
    print(f"==== {band}: {len(items)} ====")
    if band.startswith("LOW"):
        continue
    for s, risk, why, decl in items:
        print(f"  [{risk}] {s}  {why}")
        if decl:
            print(f"        decl: {decl}")
