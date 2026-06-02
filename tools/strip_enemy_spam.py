import re, os
ROOT = r"C:\Claude\silenthill\silent-hill-decomp"

# files -> tag regexes whose 3-line (#ifdef SH_PC_PORT / SH_DBG("[TAG]...) / #endif) wrappers to strip
TARGETS = {
 "src/maps/characters/stalker.c": [r'\[STALKER\]'],
 "src/bodyprog/events/npc_main.c": [r'\[NPC\] (ai-|post-)'],
}

for rel, tags in TARGETS.items():
    p = os.path.join(ROOT, rel)
    lines = open(p, encoding="utf-8").read().split("\n")
    out = []
    i = 0
    removed = 0
    tagre = re.compile(r'\s*SH_DBG\("(' + "|".join(tags) + r')')
    while i < len(lines):
        # match: #ifdef SH_PC_PORT \n <SH_DBG "[TAG]..."> \n #endif
        if (lines[i].strip() == "#ifdef SH_PC_PORT"
                and i + 2 < len(lines)
                and tagre.match(lines[i+1])
                and lines[i+2].strip() == "#endif"):
            i += 3
            removed += 1
            continue
        out.append(lines[i]); i += 1
    open(p, "w", encoding="utf-8").write("\n".join(out))
    print(rel, "removed blocks:", removed)
