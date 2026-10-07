#!/usr/bin/env python3
"""Pre-fill the translation template with the Team Raccoon Russian text.

Reads the ViT Co / Metallist / Team Raccoon patched US disc (an in-place
patch: same file table, same overlay link base, BODYPROG still encrypted and
US-linked) and writes SilentHill_RU_for_review.txt: the normal template with
an RU: line already holding the disc's own Russian wherever the disc has it.

  * Story text: every VIN/MAPx_Sxx.BIN message table, by the same index the
    English uses (the patch keeps every table's size).
  * Items: BODYPROG's name/description pointer arrays.
  * Original menus: the English literal is found in the retail US file it
    lives in, and the pointer (or lui/addiu pair) that references it is
    followed in the patched copy of the same file. The patch rewrote those
    references when a Russian string did not fit the English slot, so reading
    the old offset returns the tail of some other string.
  * Menus the disc does not carry (the retail exe was never touched) fall back
    to the port's own Russian table, pc_port/src/lang_ru_menu.inc, and say so.
  * PC-port menus are left blank for the translator.

The disc font has Cyrillic painted over the Latin cells, so its bytes decode
through the same 'vitco' table pc_port/src/lang_ru.c encodes with.

Team Raccoon is a Russian DUB and blanks the subtitle of every voiced line;
any further discs given (the same team's text-only release) fill those gaps.

Usage:
  python make_ru_template.py "<gamedata>/Silent Hill (ViT Co_Metallist) (Team Raccoon).bin" \\
                             "<gamedata>/Silent Hill (USA).bin" \\
                             "<gamedata>/Silent Hill RU ViT Co Metallist text.bin"
"""
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(REPO, 'pc_port', 'tools'))

import gen_zh_pack as disc        # noqa: E402  (raw-sector reader, overlay LCG)
import extract_text as tmpl       # noqa: E402  (records, sections, writer)

USA_OVL_BASE   = 0x800C9578
USA_BODY_VRAM  = 0x80024B60
USA_ITEM_NAME  = 0x800ADB60
USA_ITEM_DESC  = 0x800ADE6C
SCREEN_OVL     = 0x801E2600       # OPTION / SAVELOAD / STF_ROLL link base

# lang_ru.c s_Charset_ViTCo, in the same 33-letter order (Ё after Е).
ALPHABET = 'АБВГДЕЁЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯ'
VITCO_UP = [0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x00, 0x47, 0x33, 0x49, 0x00,
            0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55,
            0x56, 0x57, 0x58, 0x59, 0x00, 0x00, 0x5B, 0x4A, 0x3C, 0x3D, 0x3E]
VITCO_LO = [0x27, 0x2A, 0x5E, 0x3B, 0x60, 0x48, 0x5A, 0x61, 0x62, 0x63, 0x64,
            0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F,
            0x78, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x70, 0x79, 0x7A]
DECODE = {}
for _i, _b in enumerate(VITCO_UP):
    if _b:
        DECODE[_b] = ALPHABET[_i]
for _i, _b in enumerate(VITCO_LO):
    DECODE[_b] = ALPHABET[_i].lower()
DIGIT_3 = 0x33   # the translators' capital З; a digit when no letter touches it


def decode(bs):
    """Disc bytes -> Unicode, with ~X control codes (and their argument byte,
    plus ~J's '(seconds)') copied through untouched."""
    out, i, n = [], 0, len(bs)
    while i < n:
        c = bs[i]
        if c == 0x7E and i + 1 < n:
            j = i + 3
            if bs[i + 1] == ord('J') and j < n and bs[j] == ord('('):
                k = bs.find(b')', j)
                j = k + 1 if k >= 0 else j
            out.append(bs[i:j].decode('latin-1'))
            i = j
            continue
        if c == DIGIT_3:
            near = (i + 1 < n and bs[i + 1] in DECODE and bs[i + 1] != DIGIT_3) or \
                   (i > 0 and bs[i - 1] in DECODE and bs[i - 1] != DIGIT_3)
            out.append('З' if near else '3')
        elif c in DECODE:
            out.append(DECODE[c])
        else:
            out.append(chr(c))
        i += 1
    # The patch pads some wrapped item descriptions with this run where the
    # English indents with tabs; it is layout, not text.
    return ''.join(out).replace('{09$}', '')


def readable_ru(bs):
    # '[' is the Ы cell in this font, so the patch writes both brackets as ']'
    # ("]Льва]"); give the reviewer the [Lion] form the English uses.
    return re.sub(r'\]([^\]~]*)\]', r'[\1]', tmpl.readable(decode(bs)))


# --------------------------------------------------------------------------
# Disc access
# --------------------------------------------------------------------------
FILETABLE = open(os.path.join(REPO, 'src', 'main', 'filetable.c.USA.inc'),
                 encoding='utf-8', errors='replace').read()


def ft_file(path, tag):
    m = re.search(r'\{\s*(0x[0-9a-fA-F]+),\s*(\d+),.*?//\s*%s' % re.escape(tag), FILETABLE)
    return disc.read_disc_file(path, int(m.group(1), 16), int(m.group(2)) * 256)


def menu_files(path):
    return {
        'BODY':     (disc.fs_decrypt_overlay(ft_file(path, '1ST/BODYPROG.BIN')), USA_BODY_VRAM),
        'OPTION':   (ft_file(path, 'VIN/OPTION.BIN'), SCREEN_OVL),
        'SAVELOAD': (ft_file(path, 'VIN/SAVELOAD.BIN'), SCREEN_OVL),
    }


def cstr(d, off):
    e = d.find(b'\0', off)
    return d[off:e] if e >= 0 else b''


def references(d, addr):
    """Positions in `d` that produce `addr`: data words and lui/addiu pairs."""
    refs = []
    w = struct.pack('<I', addr)
    i = d.find(w)
    while i >= 0:
        if i % 4 == 0:
            refs.append(('ptr', i))
        i = d.find(w, i + 1)
    lo = addr & 0xFFFF
    hi = ((addr >> 16) + (1 if lo >= 0x8000 else 0)) & 0xFFFF
    for p in range(0, len(d) - 3, 4):
        ins = struct.unpack_from('<I', d, p)[0]
        if (ins >> 26) != 9 or (ins & 0xFFFF) != lo:      # addiu
            continue
        rs = (ins >> 21) & 31
        for q in range(p - 4, max(-1, p - 64), -4):
            j = struct.unpack_from('<I', d, q)[0]
            if (j >> 26) == 15 and ((j >> 16) & 31) == rs and (j & 0xFFFF) == hi:   # lui
                refs.append(('lui', (q, p)))
                break
    return refs


def follow(d, kind, pos):
    if kind == 'ptr':
        return struct.unpack_from('<I', d, pos)[0]
    q, p = pos
    hi = struct.unpack_from('<I', d, q)[0] & 0xFFFF
    lo = struct.unpack_from('<I', d, p)[0] & 0xFFFF
    return ((hi << 16) + (lo - 0x10000 if lo >= 0x8000 else lo)) & 0xFFFFFFFF


def disc_menu_text(us_files, ru_files, literal):
    """Patched bytes for one US menu literal, or None."""
    needle = literal + b'\0'
    for name, (d, base) in us_files.items():
        # A string starts after a NUL, or word-aligned straight after data
        # (the results table follows a pointer array with no padding).
        off = d.find(needle)
        while off > 0 and d[off - 1] != 0 and off % 4 != 0:
            off = d.find(needle, off + 1)
        if off < 0:
            continue
        rd, rbase = ru_files[name]
        refs = references(d, base + off)
        for kind, pos in refs:
            a = follow(rd, kind, pos)
            if rbase <= a < rbase + len(rd):
                s = cstr(rd, a - rbase)
                if s:
                    return s
        # A reference that now leads to an empty string is the patch blanking
        # the text on purpose (==Use_OK== and friends); the old offset holds
        # the tail of whatever moved in.
        if refs:
            return None
        s = cstr(rd, off)
        if s and s != literal:
            return s
    return None


def port_ru_menu():
    """US literal -> Russian, from the port's own table."""
    text = open(os.path.join(REPO, 'pc_port', 'src', 'lang_ru_menu.inc'), encoding='utf-8').read()
    out = {}
    for m in re.finditer(r'^\s*\{\s*((?:"(?:[^"\\]|\\.)*"\s*)+),\s*((?:"(?:[^"\\]|\\.)*"\s*)+)\}', text, re.M):
        k = tmpl.parse_c_entries(m.group(1))
        v = tmpl.parse_c_entries(m.group(2))
        if k and v:
            out[k[0][1]] = v[0][1]
    return out


# --------------------------------------------------------------------------

LEGEND = """\
================================================================================
 SILENT HILL (1999)  -  RUSSIAN TRANSLATION FOR REVIEW  (PC port)
================================================================================
 This is the full English script of the PC port with a Russian line under
 every entry. The Russian is PRE-FILLED from the Team Raccoon / ViT Co /
 Metallist fan translation ("Silent Hill (ViT Co_Metallist) (Team
 Raccoon)"), read straight off that disc.  Total entries: {total}.

 WHAT TO DO
 ----------
 1. Check and correct the pre-filled RU: lines.
 2. Fill in the EMPTY RU: lines. These are:
      - text the PC port added (PC Options, the F10 Quick Options menu,
        the Controls panel, confirm boxes), and
      - lines the disc left blank (some voiced cutscene lines in the intro
        have no text on the disc) and a few it never translated.
 3. Leave the [KEY] and EN: lines alone. Send the file back when done.

 KNOWN PROBLEMS IN THE PRE-FILLED TEXT
 -------------------------------------
 * The disc's font has no capital Й, Щ, Ъ or Ё, so words in CAPITALS use
   substitutes (НАИДЕН for НАЙДЕН, ОБЬЕКТ for ОБЪЕКТ). Please restore the
   real letters - write normal Russian, the game handles the font.
 * Team Raccoon is a Russian DUB, so its disc has no subtitles for voiced
   lines. Those subtitles come from the same team's text-only release and
   are marked "NOTE: from the ViT Co / Metallist text release".
 * Entries marked "NOTE: not on the disc" were written by the port team,
   not by Team Raccoon. Please check them too.
 * Line breaks: the pre-filled text shows the disc's own line breaks
   only where it used ~N. You may move ~N freely (see below).

 FORMAT
 ------
 * Each entry:
       [KEY]        <- an ID. DO NOT change it.
       NOTE: ...    <- (some entries) where the text appears / length limit.
       EN: ...      <- the English text.
       RU: ...      <- the Russian. Edit or fill in this line.
 * Save as UTF-8.

 CONTROL CODES (story text, prompts, items) - keep unchanged, in place:
 * ~N        line break. You MAY add or move ~N if a line runs long.
 * ~E        end-of-message marker. Always keep it at the very end.
 * ~C2 ... ~C7   colour on / off around a highlighted word (item name).
 * ~S4       a Yes/No choice prompt. Keep as-is.
 * ~J0(1.2), ~J1(3.8), ...   display time in seconds. Keep code AND number,
                 and do not add new ~J codes (they are timed to the voice).
 * ~D        a short leading marker on some map/notice lines. Keep as-is.
 * {{n}}, {{key}}, {{name}}, {{action}}, {{camera}}  (PC menus) - filled in by
   the game. Keep them, spelled exactly the same, anywhere in the sentence.

 LENGTH
 ------
 * Story text has no automatic word wrap: keep lines to about 20-24
   characters and break them with ~N. At most 9 lines per message.
 * Original menus and PC Options: label and value share one line; NOTE:
   lines give a character budget. Abbreviate if you must.
 * Original menus and PC Options draw with the game's own font, which on a
   Russian disc has NO Latin letters: write "ВСинх" rather than "VSync",
   "ФПС" rather than "FPS". Digits and punctuation are fine.
 * Quick Options and the Controls panel use a normal PC font, so Latin
   letters and key names (Esc, PgUp, F10) are fine there.
================================================================================

"""


def has_text(t):
    return re.search(r'\w', re.sub(r'~[A-Z][0-9]?(\([0-9.]*\))?', '', t)) is not None


def disc_text(ru_disc, us_files):
    """KEY -> readable Russian for everything one patched disc carries."""
    tr = {}

    # Story text + COMMON (the shared 0-14 block, read from map0_s00).
    maps, body = disc.load_filetable('filetable.c.USA.inc')
    disc.JPN_OVL_BASE = USA_OVL_BASE
    ovl_msgs = {}
    for name, (lba, size) in maps.items():
        data = disc.read_disc_file(ru_disc, lba, size * 256)
        if data:
            ovl_msgs[name] = disc.overlay_messages(data, size * 256)
    for sec, key, raw, rd, note in tmpl.records:
        if key.startswith('COMMON.'):
            msgs, idx = ovl_msgs.get('map0_s00'), int(key.split('.')[1])
        elif key.startswith('MAP'):
            mname, idx = key.split('.')
            msgs, idx = ovl_msgs.get(mname.lower()), int(idx)
        else:
            continue
        if msgs and idx < len(msgs):
            t = readable_ru(msgs[idx])
            if has_text(t):
                tr[key] = t

    # Items.
    disc.JPN_BODY_VRAM, disc.JPN_ITEM_NAME, disc.JPN_ITEM_DESC = USA_BODY_VRAM, USA_ITEM_NAME, USA_ITEM_DESC
    names, descs = disc.item_arrays(disc.fs_decrypt_overlay(
        disc.read_disc_file(ru_disc, body[0], body[1] * 256)))
    for arr, prefix in ((names, 'ITEM_NAME.'), (descs, 'ITEM_DESC.')):
        for i, s in enumerate(arr):
            if s:
                t = readable_ru(s)
                if has_text(t):
                    tr[prefix + str(i)] = t

    # Original menus.
    ru_files = menu_files(ru_disc)
    for sec, key, raw, rd, note in tmpl.records:
        if sec not in ('MENU', 'RESULTS', 'PCOPT'):
            continue
        s = disc_menu_text(us_files, ru_files, raw.encode('latin-1'))
        if s:
            t = readable_ru(s)
            if has_text(t):
                tr[key] = t
    return tr


def main():
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except AttributeError:
        pass
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    ru_disc, us_disc, fill_discs = sys.argv[1], sys.argv[2], sys.argv[3:]

    us_files = menu_files(us_disc)
    tr = disc_text(ru_disc, us_files)
    src = {k: 'disc' for k in tr}

    # The Team Raccoon release is a Russian DUB: it blanks the subtitle of
    # every voiced line. The same translators' text-only releases keep them.
    fill_diff = 0
    for fd in fill_discs:
        other = disc_text(fd, us_files)
        for k, t in other.items():
            if k not in tr:
                tr[k], src[k] = t, 'fill'
            elif src[k] == 'disc' and tr[k] != t:
                fill_diff += 1

    # The port's own Russian menus, for menu text no disc carries. PC
    # Options stay blank unless a disc itself has the word (On/Off/Map...).
    port = port_ru_menu()
    for sec, key, raw, rd, note in tmpl.records:
        if sec == 'MENU' and key not in tr and raw in port:
            tr[key], src[key] = tmpl.readable(port[raw]), 'port'

    # Annotate port-written entries in the NOTE line.
    records = []
    for sec, key, raw, rd, note in tmpl.records:
        if src.get(key) == 'port':
            note = (note + '; ' if note else '') + 'not on the disc, written by the port team'
        elif src.get(key) == 'fill':
            note = (note + '; ' if note else '') + 'from the ViT Co / Metallist text release'
        records.append((sec, key, raw, rd, note))
    tmpl.groups.clear()
    for rec in records:
        tmpl.groups.setdefault(rec[0], []).append(rec)

    out = os.path.join(HERE, 'SilentHill_RU_for_review.txt')
    tmpl.write_template(out, tr_label='RU', tr_text=tr,
                        legend=LEGEND.format(total=len(records)))

    from collections import Counter
    filled = Counter((r[0].split(' ')[0], src.get(r[1], 'blank')) for r in records)
    by_sec = {}
    for (sec, s), n in filled.items():
        by_sec.setdefault(sec, {})[s] = n
    for sec, c in sorted(by_sec.items()):
        print('  %-10s %s' % (sec, ', '.join('%s %d' % kv for kv in sorted(c.items()))))
    print('pre-filled %d of %d (disc %d, text release %d, port %d)'
          % (len(tr), len(records), sum(1 for v in src.values() if v == 'disc'),
             sum(1 for v in src.values() if v == 'fill'),
             sum(1 for v in src.values() if v == 'port')))
    if fill_discs:
        print('entries worded differently on the text release (Team Raccoon kept): %d' % fill_diff)
    print('WROTE:', out)


if __name__ == '__main__':
    main()
