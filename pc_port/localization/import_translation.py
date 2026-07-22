#!/usr/bin/env python3
"""Re-import a finished translation into an engine-format language pack.

Reads a translated SilentHill_<XX>_translation.txt (the [KEY]/EN:/PT: file
handed to the translator) plus SilentHill_EN_raw.json, and writes
gamedata/lang/<code>.lang for the runtime loader (pc_port/src/lang_pack.c).

The .txt the translator fills in is a READABLE rendering: '_' was decoded to a
space, '\\t'/'\\n' indent art collapsed, '\\x01' kerning bytes dropped. Going
back to engine format is a re-synthesis, not an inverse:

  * Every '~X' control code consumes exactly ONE argument byte after the code
    letter (verified across all 1646 source strings): a digit for C/J/L/S, a
    whitespace pad for D/E/H/M/N. '~J' additionally carries a '(seconds)'
    group. Drop the arg byte and the parser desyncs and eats the next glyph.
  * A rendered space is '_'. Literal ' ', '\\t' and '\\n' are skipped by both
    the drawer (text_draw.c Gfx_StringDraw) and the width pass, so they are
    cosmetic layout only -- preserved from the source where they follow a code.

Validation is the point of this script as much as conversion: a translation
that moved, dropped or invented a non-'~N' code would silently corrupt a
message at runtime, so those entries are reported and left English. Extra '~N'
is allowed (translations run longer and need more line breaks).

Usage:
  python import_translation.py --in ../build/newlanguages/SilentHill_PL_translation.txt \
                               --code pl --name Polish --menu POLISH
"""
import argparse
import json
import os
import re
import sys
import unicodedata

HERE = os.path.dirname(os.path.abspath(__file__))
RAW_JSON = os.path.join(HERE, "SilentHill_EN_raw.json")

# Codes whose argument byte is a whitespace pad rather than a digit.
PAD_CODES = set("DEHMN")
# Codes whose argument byte is a digit.
DIGIT_CODES = set("CJLS")

MSG_LINES_MAX = 9   # FONT_12X16_LINE_COUNT_MAX -- the renderer clips past it.
LINE_CHARS_SOFT = 26  # box is x=40..~280 at ~10px/glyph; warn past this.

PLACEHOLDER = "(no text"  # "(no text - timing only, leave PT blank)"


# --------------------------------------------------------------------------
# Tokenizer
# --------------------------------------------------------------------------
def tokenize(s, readable):
    """Split an engine (readable=False) or translator-facing (readable=True)
    string into ('code', text) / ('text', text) / ('layout', text) tokens.

    A code token carries its argument byte (and the '(n.n)' group for ~J). In
    readable mode the pad byte for D/E/H/M/N may be missing at end-of-line, and
    layout runs do not exist -- spaces are word spaces."""
    out = []
    i, n = 0, len(s)
    buf = []

    def flush_text():
        if buf:
            out.append(("text", "".join(buf)))
            buf.clear()

    while i < n:
        c = s[i]
        if c == "~" and i + 1 < n:
            flush_text()
            letter = s[i + 1]
            i += 2
            arg = ""
            if letter in DIGIT_CODES:
                if i < n and s[i].isdigit():
                    arg = s[i]
                    i += 1
            elif letter in PAD_CODES:
                # Engine strings always carry the pad; readable ones may not.
                if i < n and s[i] in " \t\n":
                    arg = s[i]
                    i += 1
                else:
                    arg = " "
            group = ""
            if letter == "J" and i < n and s[i] == "(":
                j = s.find(")", i)
                if j >= 0:
                    group = s[i:j + 1]
                    i = j + 1
            out.append(("code", "~" + letter + arg + group))
            continue

        if not readable and c in " \t\n":
            flush_text()
            j = i
            while j < n and s[j] in " \t\n":
                j += 1
            out.append(("layout", s[i:j]))
            i = j
            continue

        buf.append(c)
        i += 1

    flush_text()
    return out


def code_letter(tok):
    return tok[1]


def normalize_code(tok):
    """Pad bytes for D/E/H/M/N are ' ', '\\t' or '\\n' interchangeably -- all
    three are skipped by the drawer, so they must not count as a difference."""
    letter = code_letter(tok)
    if letter in PAD_CODES:
        return "~" + letter
    return tok


def code_signature(tokens):
    """Non-~N code sequence -- what must be preserved across a translation."""
    return [normalize_code(t[1]) for t in tokens
            if t[0] == "code" and code_letter(t[1]) != "N"]


def split_segments(tokens, split_on_n):
    """Split a token stream on codes. Returns (segments, seps): segments[i] is
    the run of tokens before seps[i].

    Splitting on every code (split_on_n=True) gives the tightest alignment and
    is used whenever the translation kept the source's ~N count. Otherwise the
    split ignores ~N, so a translation free to add its own line breaks still
    lines up on the codes that matter."""
    segs = [[]]
    seps = []
    for tok in tokens:
        if tok[0] == "code" and (split_on_n or code_letter(tok[1]) != "N"):
            seps.append(tok)
            segs.append([])
        else:
            segs[-1].append(tok)
    return segs, seps


def wide_gap(text, space_char):
    """A run of 3+ rendered spaces is deliberate layout (the save prompt's
    'Yes____No' selector, a note's indent), not typography. readable()
    collapsed it to one space, so the translation cannot carry it."""
    best = None
    for m in re.finditer(re.escape(space_char) + r"{3,}", text):
        if best is None or len(m.group(0)) > len(best.group(0)):
            best = m
    return best


def apply_wide_gap(core, raw_text, space_char):
    """Re-open the source's wide gap in a translated run, sized so the run
    keeps the source's overall width (that width is what positions the
    on-screen selector)."""
    m = wide_gap(raw_text, space_char)
    if m is None or space_char not in core:
        return core

    # Gap sits at the same relative position as in the source.
    frac = m.start() / max(len(raw_text), 1)
    gaps = [i for i, ch in enumerate(core) if ch == space_char]
    at = min(gaps, key=lambda i: abs(i / max(len(core), 1) - frac))

    left, right = core[:at], core[at + 1:]
    width = len(raw_text) - len(left) - len(right)
    return left + space_char * max(width, 2) + right


def segment_edge_spaces(seg, space_char):
    """Rendered spaces ('_') at the very start / end of a source segment."""
    texts = [t[1] for t in seg if t[0] == "text"]
    if not texts:
        return "", ""
    lead = len(texts[0]) - len(texts[0].lstrip(space_char))
    tail = len(texts[-1]) - len(texts[-1].rstrip(space_char))
    if len(texts) == 1 and texts[0].strip(space_char) == "":
        tail = 0  # all-underscore run: do not double-count
    return space_char * lead, space_char * tail


def raw_layout_after(raw_toks, code_tok):
    """The skipped whitespace the source puts after a code (indent art, and
    the '\\t' every ~J0(n) cue carries). Both parsers ignore it, but keeping
    it makes the pack diff cleanly against the original."""
    for idx, tok in enumerate(raw_toks):
        if tok is code_tok:
            nxt = raw_toks[idx + 1] if idx + 1 < len(raw_toks) else None
            return nxt[1] if nxt and nxt[0] == "layout" else ""
    return ""


# --------------------------------------------------------------------------
# Re-encode
# --------------------------------------------------------------------------
def rendered_len(text):
    """Glyph count of a text run (combining marks and '_' aside)."""
    return len(text)


def wrap_plain(raw, translated, space_char, budget=None):
    """Menu / item text goes through Gfx_StringDraw, where a literal '\\n' is
    the line break ('~N' is map-message-only, and no MENU/ITEM/MISC source
    string contains a '~' at all). readable() flattened those breaks to
    spaces, so re-wrap to the source's own line budget.

    Returns (encoded, [problem, ...])."""
    problems = []
    src_lines = [ln.replace("\t", "") for ln in raw.split("\n")]
    lead = raw[0] if raw[:1] in ("\x07",) else ""
    body = translated[1:] if lead and translated[:1] == lead else translated
    body = body.strip()

    if len(src_lines) == 1:
        core = body.replace(" ", space_char)
        core = apply_wide_gap(core, raw[len(lead):], space_char)
        width = len(raw) - len(lead)
        if len(core) > width + 4:
            problems.append("longer than the English (%d vs %d chars) on a "
                            "single-line string" % (len(core), width))
        return lead + core, problems

    # Wrap to the widest line the game itself ships for this kind of string,
    # not this entry's own longest line -- Polish runs longer than English and
    # extra lines cost more than extra width inside the same box.
    budget = max(budget or 0, max(len(ln) for ln in src_lines))
    words = body.split()
    lines, cur = [], ""
    for w in words:
        cand = w if not cur else cur + " " + w
        if len(cand) > budget and cur:
            lines.append(cur)
            cur = w
        else:
            cur = cand
    if cur:
        lines.append(cur)

    if len(lines) > len(src_lines):
        problems.append("needs %d lines, the English uses %d (budget %d chars)"
                        % (len(lines), len(src_lines), budget))

    return lead + "\n".join(ln.replace(" ", space_char) for ln in lines), problems


def category_budgets(raw_map):
    """Widest line the game itself ships per key class -- the evidence for how
    much room the box actually has (ITEM_DESC reaches 32 chars, ITEM_NAME 22)."""
    budgets = {}
    for key, val in raw_map.items():
        if "~" in val:
            continue
        cat = key.split(".")[0]
        for line in val.split("\n"):
            budgets[cat] = max(budgets.get(cat, 0), len(line.replace("\t", "")))
    return budgets


def encode_entry(raw, translated, space_char, budget=None):
    """Rebuild engine format from a translated readable line, borrowing the
    source's layout whitespace so indent art and ~J pads survive.

    Returns (encoded, [problem, ...])."""
    if "~" not in raw:
        return wrap_plain(raw, translated, space_char, budget)

    problems = []
    raw_toks = tokenize(raw, readable=False)
    tr_toks = tokenize(translated, readable=True)

    # A dropped trailing ~E is the one unambiguous omission: it is always last
    # and always present in the source, so repair it rather than losing the
    # whole message to English.
    raw_sig, tr_sig = code_signature(raw_toks), code_signature(tr_toks)
    if raw_sig and raw_sig[-1] == "~E" and "~E" not in tr_sig:
        tr_toks.append(("code", "~E "))
        problems.append("translation dropped the trailing ~E -- re-added")
        tr_sig = code_signature(tr_toks)
    if raw_sig != tr_sig:
        problems.append("control codes differ: source %s vs translation %s"
                        % (" ".join(raw_sig) or "(none)", " ".join(tr_sig) or "(none)"))
        return None, problems

    # Codes are always flanked by skipped whitespace in the source, so a
    # RENDERED space touching a code (the "a_ ~C2 Health_drink" gap before an
    # item name) survives only as '_'. readable() collapsed '_ ' to ' ', so
    # the translator's file cannot express it -- recover it from the source.
    tight = (sum(1 for t in raw_toks if t[0] == "code") ==
             sum(1 for t in tr_toks if t[0] == "code"))
    raw_segs, raw_seps = split_segments(raw_toks, tight)
    tr_segs, _tr_seps = split_segments(tr_toks, tight)

    out = []

    def sep():
        """One skipped space before a code -- unless the output already ends
        in skipped whitespace, or nothing has been emitted yet."""
        if out and not out[-1].endswith((" ", "\t", "\n")):
            out.append(" ")

    for i, seg in enumerate(tr_segs):
        pad_lead, pad_tail = ("", "")
        if i < len(raw_segs):
            pad_lead, pad_tail = segment_edge_spaces(raw_segs[i], space_char)

        text_positions = [j for j, t in enumerate(seg) if t[0] == "text"]
        for j, tok in enumerate(seg):
            kind, text = tok
            if kind == "code":  # only ~N reaches here
                sep()
                out.append(text)
            elif kind == "text":
                core = text.strip().replace(" ", space_char)
                # Only when the segment is a single run on both sides is the
                # gap's position unambiguous; otherwise reproducing it would
                # smear the gap across every line of the segment.
                if i < len(raw_segs) and len(text_positions) == 1:
                    raw_texts = [t[1] for t in raw_segs[i] if t[0] == "text"]
                    if len(raw_texts) == 1:
                        core = apply_wide_gap(core, raw_texts[0], space_char)
                    elif any(wide_gap(t, space_char) for t in raw_texts):
                        problems.append("source has a wide layout gap this "
                                        "translation cannot place")
                if j == text_positions[0]:
                    core = pad_lead + core
                if j == text_positions[-1]:
                    core = core + pad_tail
                out.append(core)

        if i < len(raw_seps):
            sep()
            out.append(raw_seps[i][1])
            out.append(raw_layout_after(raw_toks, raw_seps[i]))

    encoded = "".join(out)

    # The drawer breaks lines on ~N only -- there is no auto-wrap.
    lines = re.split(r"~N", encoded)
    if len(lines) > MSG_LINES_MAX:
        problems.append("%d lines > MSG_LINES_MAX %d (tail will be clipped)"
                        % (len(lines), MSG_LINES_MAX))
    for ln in lines:
        visible = re.sub(r"~.[0-9]?(\([0-9.]*\))?", "", ln)
        visible = visible.replace("\t", "").replace("\n", "").strip()
        if rendered_len(visible) > LINE_CHARS_SOFT:
            problems.append("line runs off-screen (%d chars): %s"
                            % (rendered_len(visible), visible[:40]))
            break

    return encoded, problems


def space_char_for(raw):
    """Word-space convention of this entry. Four of 1646 source strings use a
    literal space instead of '_' (MISC.0/1, MAP0_S01.65, MAP7_S02.133)."""
    stripped = re.sub(r"~.[0-9]?(\([0-9.]*\))?", "", raw)
    if "_" in stripped:
        return "_"
    if re.search(r"[A-Za-z0-9,.] [A-Za-z0-9]", stripped):
        return " "
    return "_"


# --------------------------------------------------------------------------
# Translation-file parser
# --------------------------------------------------------------------------
def parse_translation(path):
    """Blocks of [KEY] / EN: / PT:. A handful of MENU keys embed a newline
    (the save/load prompts are two-line literals), so a key runs from the
    opening '[' to the first line ending in ']'."""
    entries = {}
    key = None
    pending = None  # accumulating a multi-line key
    with open(path, encoding="utf-8") as f:
        for lineno, line in enumerate(f, 1):
            line = line.rstrip("\r\n")

            if pending is not None:
                pending.append(line)
                if line.endswith("]"):
                    key = "\n".join(pending)[1:-1]
                    pending = None
                continue

            if line.startswith("[") and "." in line.split("]")[0]:
                if line.endswith("]"):
                    key = line[1:-1]
                else:
                    pending = [line]
                continue

            if key is not None and line.startswith("PT:"):
                entries[key] = (line[3:].strip(), lineno)
                key = None
    return entries


def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except AttributeError:
        pass
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="src", required=True, help="translated .txt")
    ap.add_argument("--code", required=True, help="language id, e.g. pl")
    ap.add_argument("--name", required=True, help="English name, e.g. Polish")
    ap.add_argument("--menu", default=None, help="options-menu label (default: --name)")
    ap.add_argument("--out", default=None, help="output .lang path")
    args = ap.parse_args()

    out_path = args.out or os.path.join(
        HERE, "..", "assets", "gamedata", "lang", args.code + ".lang")
    out_path = os.path.abspath(out_path)

    with open(RAW_JSON, encoding="utf-8") as f:
        raw = json.load(f)

    budgets = category_budgets(raw)
    tr = parse_translation(args.src)
    print("source entries : %d" % len(raw))
    print("translated file: %d keys" % len(tr))

    missing = [k for k in raw if k not in tr]
    unknown = [k for k in tr if k not in raw]
    if missing:
        print("  WARNING: %d key(s) absent from the translation" % len(missing))
    if unknown:
        print("  WARNING: %d unknown key(s) ignored: %s"
              % (len(unknown), ", ".join(unknown[:5])))

    encoded = {}
    problem_keys = []
    skipped = 0
    charset = set()

    for key, source in raw.items():
        got = tr.get(key)
        if got is None:
            skipped += 1
            continue
        text, lineno = got
        if not text or text.startswith(PLACEHOLDER):
            skipped += 1
            continue

        enc, problems = encode_entry(source, text, space_char_for(source),
                                     budgets.get(key.split(".")[0]))
        if problems:
            problem_keys.append((key, lineno, problems))
        if enc is None:
            continue
        if enc == source:
            # Untranslated (left as English) -- no point shipping it.
            skipped += 1
            continue
        encoded[key] = enc
        for ch in enc:
            if ord(ch) > 0x7F:
                charset.add(ch)

    print("encoded        : %d" % len(encoded))
    print("skipped        : %d (blank / placeholder / identical to English)" % skipped)

    fatal = [p for p in problem_keys if any("control codes differ" in x for x in p[2])]
    warn = [p for p in problem_keys if p not in fatal]
    if fatal:
        print("\nERRORS -- these entries stay ENGLISH (%d):" % len(fatal))
        for key, lineno, problems in fatal[:40]:
            print("  %-18s (line %d)" % (key, lineno))
            for p in problems:
                print("      %s" % p)
        if len(fatal) > 40:
            print("  ... and %d more" % (len(fatal) - 40))
    if warn:
        by_kind = {}
        for key, lineno, problems in warn:
            for p in problems:
                kind = p.split("(")[0].split(":")[0].strip()
                by_kind.setdefault(kind, []).append((key, lineno, p))
        print("\nWARNINGS (%d entries) -- imported, but check on screen:" % len(warn))
        for kind, items in sorted(by_kind.items(), key=lambda x: -len(x[1])):
            print("  %-32s %d" % (kind, len(items)))
            for key, lineno, p in items[:6]:
                print("      %-18s (line %d) %s" % (key, lineno, p))
            if len(items) > 6:
                print("      ... and %d more" % (len(items) - 6))

        # Baseline: how many English source lines already exceed the same
        # budget, so an over-long count is judged against the original.
        over_en = 0
        for key in encoded:
            for ln in re.split(r"~N", raw[key]):
                vis = re.sub(r"~.[0-9]?(\([0-9.]*\))?", "", ln)
                vis = vis.replace("\t", "").replace("\n", "").strip()
                if len(vis) > LINE_CHARS_SOFT:
                    over_en += 1
                    break
        print("  (English baseline over %d chars, same entries: %d)"
              % (LINE_CHARS_SOFT, over_en))

    print("\nnon-ASCII characters used (%d):" % len(charset))
    for ch in sorted(charset):
        print("  U+%04X  %s  %s" % (ord(ch), ch.encode("utf-8").hex(),
                                    unicodedata.name(ch, "?")))

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("# Silent Hill PC -- language pack (generated by import_translation.py)\n")
        f.write("# Engine format: '_' is a rendered space; every ~X code carries its\n")
        f.write("# argument byte. Do not hand-edit unless you know both rules.\n")
        f.write("!code=%s\n" % args.code)
        f.write("!name=%s\n" % args.name)
        f.write("!menu=%s\n" % (args.menu or args.name))
        for key in raw:  # source order keeps diffs readable
            if key in encoded:
                f.write("%s=%s\n" % (key, encoded[key].replace("\n", "\\n").replace("\t", "\\t")))

    print("\nwrote %s (%d entries)" % (out_path, len(encoded)))
    return 1 if fatal else 0


if __name__ == "__main__":
    sys.exit(main())
