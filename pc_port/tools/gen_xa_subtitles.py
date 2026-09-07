"""Emit the launcher's XaSubtitles.cs: which script message each disc voice line
subtitles.

The game has no such table. A cutscene shows its messages through
Map_MessageWithAudio(msgIdx, &audioIdx, cmdTable): every page shown (the
first page, then each page turn) fires SD_Call(cmdTable[audioIdx++]), and a
0x1NNN command plays XA line NNN. So the pairing is recovered by replaying each
cutscene's calls in source order: page k of the call's message consumes the next
table entry. A message runs on into the next index until a page ends in ~E
(or a selection code), which is how the renderer decides "more follows".

Run after map code or the script changes:
    python pc_port/tools/gen_xa_subtitles.py
Mismatches worth a look are printed to stderr.
"""
import glob, json, os, re, sys
root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
data = json.load(open(os.path.join(root, "pc_port/localization/SilentHill_EN_raw.json"), encoding="utf-8"))
XA_COUNT = 727

def read(p):
    return open(p, encoding="utf-8", errors="replace").read()

# XA line lengths (VSync frames) for the timer sanity check.
src = read(os.path.join(root, "src/bodyprog/sound/sound_data.c"))
m = re.search(r"g_XaItemData\[727\]\s*=\s*\{(.*?)\n\};", src, re.S)
frames = [int([x.strip() for x in r.split(",")][6], 0) for r in re.findall(r"\{\s*([^}]*?)\s*\}", m.group(1))]

def pages(map_name, msg):
    n = 0
    while True:
        k = "%s.%d" % (map_name, msg + n)
        if k not in data:
            return max(n, 1)
        n += 1
        if re.search(r"~[ESC]\d*\s*$", data[k].rstrip()):
            return n

TABLE_RE = re.compile(r"\b(u8|u16)\s+([A-Za-z_0-9]+)\s*\[\s*\d*\s*\]\s*=\s*\{([^}]*)\}", re.S)
def tables_in(path):
    out = {}
    for width, name, body in TABLE_RE.findall(read(path)):
        vals = [int(x, 0) for x in re.findall(r"0x[0-9A-Fa-f]+|\d+", body)]
        if width == "u8":  # a table the extractor typed as bytes: little-endian u16 pairs
            vals = [vals[i] | (vals[i + 1] << 8) for i in range(0, len(vals) - 1, 2)]
        out[name] = vals
    return out

CALL_RE = re.compile(r"(?:Map_MessageWithAudio|Event_DisplayMapMsgWithAudio)\(\s*([^,]+?)\s*,\s*&?\s*([A-Za-z_0-9]+)(?:\[\d+\])?\s*,\s*&?\s*([A-Za-z_0-9]+)(?:\[\d+\])?\s*\)")

keys = [None] * XA_COUNT
alts = {}
for mapdir in sorted(glob.glob(os.path.join(root, "src/maps/map*"))):
    map_name = os.path.basename(mapdir).upper()
    tables = {}
    for p in glob.glob(os.path.join(mapdir, "*.c")):
        tables.update(tables_in(p))
    ext = os.path.join(root, "pc_port/build_gen/extracted_data/%s_extracted_data.c" % os.path.basename(mapdir))
    if os.path.exists(ext):
        tables.update(tables_in(ext))
    pos = {}
    last = {}
    for p in sorted(glob.glob(os.path.join(mapdir, "*.c"))):
        text = read(p)
        events = []
        for mm in CALL_RE.finditer(text):
            events.append((mm.start(), "call", int(mm.group(1), 0), mm.group(2), mm.group(3)))
        for var in set(e[3] for e in events):
            for mm in re.finditer(r"\b%s\s*=\s*0\s*;" % re.escape(var), text):
                events.append((mm.start(), "reset", 0, var, None))
        events.sort()
        for _, kind, msg, var, tbl in events:
            if kind == "reset":
                for key in list(pos):
                    if key[0] == var:
                        pos[key] = 0
                continue
            if tbl not in tables:
                sys.stderr.write("%s: no table %s\n" % (map_name, tbl))
                continue
            entries = tables[tbl]
            key = (var, tbl)
            # The same message shown again by the next step is a resume of the
            # display, not a new one: the game fires nothing for it.
            if last.get(key) == msg:
                continue
            last[key] = msg
            at = pos.get(key, 0)
            n = pages(map_name, msg)
            for k in range(n):
                if at + k >= len(entries):
                    sys.stderr.write("%s: %s ran past table %s at msg %d\n" % (map_name, var, tbl, msg + k))
                    break
                cmd = entries[at + k]
                if (cmd & 0xF000) != 0x1000:
                    continue
                xa = cmd & 0xFFF
                mk = "%s.%d" % (map_name, msg + k)
                if xa >= XA_COUNT:
                    continue
                if keys[xa] is None:
                    keys[xa] = mk
                elif keys[xa] != mk:
                    alts.setdefault(xa, set()).add(mk)
                t = re.search(r"~J\d\((\d+(?:\.\d+)?)\)", data.get(mk, ""))
                if t and abs(float(t.group(1)) - frames[xa] / 60.0) > 2.5:
                    sys.stderr.write("timer %s: %s shows %.1fs, line %d lasts %.1fs\n" % (map_name, mk, float(t.group(1)), xa, frames[xa] / 60.0))
            pos[key] = at + n

mapped = sum(1 for k in keys if k)
sys.stderr.write("%d of %d lines mapped, %d with a second use\n" % (mapped, XA_COUNT, len(alts)))

out = os.path.join(root, "pc_port/launcher/SilentHillPC_Launcher/XaSubtitles.cs")
with open(out, "w", encoding="utf-8-sig", newline="\r\n") as o:
    o.write("// GENERATED by pc_port/tools/gen_xa_subtitles.py from the map sources and the English script. Do not edit.\n")
    o.write("namespace SilentHillPC_Launcher\n{\n")
    o.write("    /// <summary>For each disc voice line (g_XaItemData index) the language-pack key of\n")
    o.write("    /// the message it subtitles (see MsgTable), or null when no cutscene was found\n")
    o.write("    /// playing it.</summary>\n")
    o.write("    internal static class XaSubtitles\n    {\n")
    o.write("        public static readonly string[] Keys = new string[]\n        {\n")
    for i, k in enumerate(keys):
        o.write("            %s,%s\n" % ('"%s"' % k if k else "null", (" // %d" % i) if i % 50 == 0 else ""))
    o.write("        };\n    }\n}\n")
print("wrote", out)
