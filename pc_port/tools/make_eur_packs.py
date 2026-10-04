#!/usr/bin/env python3
"""Lift German, French, Spanish and Italian off a PAL disc into language packs.

Those four live on the PAL disc only: their story text is a whole second set of
map overlays (VIN2..VIN5, ~3.5MB each) and their item text is VIN/ITEM_*.BIN.
That is why they could only ever be selected on a PAL disc. The text itself is
tiny though -- about 100KB a language -- so extracting it into the pack format
lets any disc offer them, the same way Polish and Russian already work.

Nothing is translated here: the disc's strings are ALREADY in engine format
('_' for a rendered space, ~N / ~E / ~C codes intact), so they are copied
verbatim. Only two conversions happen:
  * disc bytes are Latin-1, the pack file is UTF-8, so bytes >= 0x80 are
    re-encoded (lang_pack.c's transcode turns them back on load);
  * real newlines and tabs become \\n and \\t, since a pack is one entry a line.

Menu text does not come off the disc at all -- retail PAL shipped ENGLISH menus
in every language -- so it comes from the port's own table in lang_menu.c, the
same strings those languages already draw today.

Usage:
    python make_eur_packs.py "<gamedata>/Silent Hill (Europe) (En,Fr,De,Es,It).bin"
"""
import io
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gen_zh_pack as disc  # noqa: E402

REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
OUT_DIR = os.path.join(HERE, '..', 'assets', 'gamedata', 'lang')

EUR_OVL_BASE = 0x800CB370
ITEM_TEXT_BASE = 0x800C8B68
ITEM_TEXT_COUNT = 195
MSG_COUNT_MAX = 176
COMMON_MSG_COUNT = 15

# config id, options-menu label, VINn path index, ITEM_*.BIN, s_MenuTr column
LANGS = [
    # PAL English is its own retranslation, not the US script ("Take them?" for
    # "Take it?"), so it is worth having on an NTSC disc as a language of its
    # own. It lives in the base VIN set, like the disc's default.
    ('en_pal', 'PAL English', 1, 'ITEM_ENG', -1),
    ('de', 'German',  2, 'ITEM_GER', 0),
    ('fr', 'French',  3, 'ITEM_FRN', 1),
    ('es', 'Spanish', 4, 'ITEM_SPN', 2),
    ('it', 'Italian', 5, 'ITEM_ITL', 3),
]

# What the disc's item files do not carry, keyed by the file they belong to.
SUPPLEMENT = {
    'ITEM_ENG': {
        'ITEM_NAME.32': 'Lobby key',
        'ITEM_DESC.32': "The key to open the door to the lobby."
                        " I found it in the locker room.",
    },
    'ITEM_GER': {
        'ITEM_NAME.32': 'Lobbyschlüssel',
        'ITEM_DESC.32': 'Der Schlüssel für die Tür zur Lobby.'
                        ' Ich habe ihn im Umkleideraum gefunden.',
    },
    'ITEM_FRN': {
        'ITEM_NAME.32': 'Clé du hall',
        'ITEM_DESC.32': 'La clé qui ouvre la porte du hall.'
                        " Je l'ai trouvée dans le vestiaire.",
    },
    'ITEM_SPN': {
        'ITEM_NAME.32': 'Llave del vestíbulo',
        'ITEM_DESC.32': 'La llave para abrir la puerta del vestíbulo.'
                        ' La encontré en el vestuario.',
    },
    'ITEM_ITL': {
        'ITEM_NAME.32': 'Chiave della hall',
        'ITEM_DESC.32': 'La chiave per aprire la porta della hall.'
                        " L'ho trovata negli spogliatoi.",
    },
}

FT = io.open(os.path.join(REPO, 'src', 'main', 'filetable.c.EUR.inc'),
             encoding='utf-8', errors='surrogateescape').read()


def ft_find(tag):
    m = re.search(r'\{\s*(0x[0-9a-fA-F]+),\s*(\d+),.*?//\s*%s\b' % re.escape(tag), FT)
    return (int(m.group(1), 16), int(m.group(2))) if m else None


def map_order():
    path = os.path.join(REPO, 'include', 'bodyprog', 'map', 'map.h')
    text = io.open(path, encoding='utf-8', errors='surrogateescape').read()
    pairs = re.findall(r'MapIdx_(MAP\w+?_S\d+)\s*=\s*(\d+)', text)
    top = max(int(v) for _, v in pairs)
    out = [None] * (top + 1)
    for name, val in pairs:
        out[int(val)] = name.upper()
    return out


def overlay_messages(ovl, size):
    tp = struct.unpack_from('<I', ovl, 0x34)[0]
    if tp < EUR_OVL_BASE or tp - EUR_OVL_BASE >= size:
        return None
    off, out = tp - EUR_OVL_BASE, []
    while off + (len(out) + 1) * 4 <= size and len(out) < MSG_COUNT_MAX:
        p = struct.unpack_from('<I', ovl, off + len(out) * 4)[0]
        if p <= EUR_OVL_BASE or p - EUR_OVL_BASE >= size:
            break
        s = p - EUR_OVL_BASE
        e = ovl.find(b'\0', s)
        if e < 0:
            break
        out.append(ovl[s:e])
    return out



# --- the runtime's own PAL->US conversion, mirrored exactly ----------------
# lang_text.c TranslateMapMsg: {X..} -> ~X.. (a one-letter code gets the arg-byte
# pad the US parser always consumes, and ~J gets the tab its forward scan needs),
# newline -> "~N ", tabs dropped, space -> '_', bytes >= 0x80 left alone.
def translate_map_msg(src):
    out, i, n = bytearray(), 0, len(src)
    while i < n:
        c = src[i]
        if c == 0x7B:                      # '{'
            out.append(0x7E)               # '~'
            i += 1
            code_len, code_first = 0, 0
            while i < n and src[i] != 0x7D:
                if code_len == 0:
                    code_first = src[i]
                out.append(src[i])
                code_len += 1
                i += 1
            if i >= n:
                break
            i += 1                         # past '}'
            if code_len == 1:
                out.append(0x20)
            if code_first == 0x4A:         # 'J'
                out.append(0x09)
            continue
        if c == 0x0A:
            out += b'~N '
        elif c == 0x09:
            pass
        elif c == 0x20:
            out.append(0x5F)               # '_'
        else:
            out.append(c)
        i += 1
    return bytes(out)


# lang_text.c s_MsgSplits: (mapIdx, language, usIdx) -> how many PAL messages
# make up that one US message. US index 2 is always two parts.
MSG_SPLITS = {
    ('MAP1_S01', 1, 23): 2, ('MAP1_S01', 2, 23): 2,
    ('MAP1_S01', 4, 23): 3, ('MAP1_S01', 4, 24): 2, ('MAP1_S01', 4, 25): 2,
    ('MAP1_S03', 3, 22): 2,
    ('MAP5_S02', 4, 43): 2,
}


def install(mapname, lang, msgs):
    """PAL message list -> {us index: engine-format bytes}, as the game does."""
    out, us, src = {}, 0, 0
    while us < MSG_COUNT_MAX and src < len(msgs):
        parts = 2 if us == 2 else MSG_SPLITS.get((mapname, lang, us), 1)
        buf = b''
        for p in range(parts):
            if src >= len(msgs):
                break
            if p:
                buf += b'~N '
            buf += translate_map_msg(msgs[src])
            src += 1
        out[us] = buf
        us += 1
    return out


def item_text(path, binname):
    ent = ft_find('VIN/%s.BIN' % binname)
    if ent is None:
        return {}
    data = disc.read_disc_file(path, ent[0], ent[1] * 256)
    size = ent[1] * 256
    out = {}
    for i in range(ITEM_TEXT_COUNT):
        for key, at in (('ITEM_NAME', 4 + i * 8), ('ITEM_DESC', 8 + i * 8)):
            ptr = struct.unpack_from('<I', data, at)[0]
            if ptr <= ITEM_TEXT_BASE or ptr - ITEM_TEXT_BASE >= size:
                continue
            s = ptr - ITEM_TEXT_BASE
            e = data.find(b'\0', s)
            if e < 0:
                continue
            # The runtime turns a literal space into the drawer's '_'.
            out['%s.%d' % (key, i)] = data[s:e].replace(b' ', b'_')  # same as TranslateItemText

    # Item 32 is a hole in every language's item file on the disc -- the Lobby
    # key is there in English and nowhere else -- so it is written here. The
    # Russian pack fills the same hole from its own supplement.
    for key, text in SUPPLEMENT.get(binname, {}).items():
        out.setdefault(key, text.replace(' ', '_').encode('latin-1'))
    return out


def menu_text(column):
    """The port's own PAL menu table, column 0..3 = de/fr/es/it."""
    path = os.path.join(REPO, 'pc_port', 'src', 'lang_menu.c')
    text = io.open(path, encoding='utf-8', errors='surrogateescape').read()
    body = text[text.index('s_MenuTr[] = {'):]
    body = body[:body.index('\n};')]
    out = {}
    # { "US", { "de", "fr", "es", "it" } }, with NULL for "keep English".
    for m in re.finditer(r'\{\s*((?:"(?:[^"\\]|\\.)*"\s*)+),\s*\{(.*?)\}\s*\}', body, re.S):
        us = ''.join(re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1)))
        cols = re.findall(r'"((?:[^"\\]|\\.)*)"|\bNULL\b', m.group(2))
        if len(cols) != 4:
            continue
        v = cols[column]
        if not v:
            continue
        out[us] = v
    return out


def c_unescape(s):
    """C source escapes -> the bytes the compiler would produce."""
    out, i = bytearray(), 0
    while i < len(s):
        if s[i] == '\\' and i + 1 < len(s):
            c = s[i + 1]
            if c == 'x':
                j = i + 2
                h = ''
                while j < len(s) and s[j] in '0123456789abcdefABCDEF' and len(h) < 2:
                    h += s[j]
                    j += 1
                out.append(int(h, 16))
                i = j
                continue
            out.append({'n': 10, 't': 9, 'r': 13, '\\': 92, '"': 34, "'": 39, '0': 0}.get(c, ord(c)))
            i += 2
            continue
        out += s[i].encode('latin-1', 'replace')
        i += 1
    return bytes(out)


def menu_key(lit):
    """Same key the loader builds: drop \\x01, trim padding, '=' -> '-'."""
    k = lit.replace('\x01', '').strip('_ \t\n\r')
    return 'MENU.' + k.replace('=', '-')


def esc(b):
    """Latin-1 disc bytes -> a UTF-8 pack line.

    The loader resolves these in KEYS as well as values, and it has to: one
    menu string is "Too dark to look at\n\t\tthe item here.", so its key holds
    a real newline and would otherwise split the entry across two lines and be
    lost. Backslash first, or escaping would double-escape its own output."""
    return (b.decode('latin-1').replace('\\', '\\\\')
            .replace('\n', '\\n').replace('\t', '\\t'))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    path = sys.argv[1]
    order = map_order()

    for code, name, vin, binname, column in LANGS:
        entries = {}

        for idx, mapname in enumerate(order):
            if mapname is None:
                continue
            # VIN/MAP0_S00 -> VIN2/MAP0_S10: the language digit is char 6.
            # VIN/MAP0_S00 -> VINn/MAP0_S<lang>0: the language digit is char 6.
            # English is the base set, where the name is unchanged.
            disc_name = mapname if vin == 1 else mapname[:6] + str(vin - 1) + mapname[7:]
            ent = ft_find(('VIN/%s' if vin == 1 else 'VIN%d/%%s' % vin) % disc_name
                          if vin == 1 else ('VIN%d/%s' % (vin, disc_name)))
            if ent is None:
                continue
            if ent[1] == 0:
                continue          # the unused test maps have no overlay
            ovl = disc.read_disc_file(path, ent[0], ent[1] * 256)
            if ovl is None or len(ovl) < 0x40:
                continue
            msgs = overlay_messages(ovl, ent[1] * 256)
            if not msgs:
                continue
            for i, m in install(mapname, vin - 1, msgs).items():
                if not m:
                    continue
                key = ('COMMON.%d' % i) if i < COMMON_MSG_COUNT else ('%s.%d' % (mapname, i))
                entries.setdefault(key, m)

        items = item_text(path, binname)
        entries.update(items)

        menus = menu_text(column) if column >= 0 else {}
        for us, v in menus.items():
            entries[menu_key(c_unescape(us).decode('latin-1'))] = c_unescape(v)

        out = os.path.join(OUT_DIR, code + '.lang')
        with io.open(out, 'w', encoding='utf-8', newline='\n') as f:
            f.write('# Silent Hill PC -- language pack (generated by make_eur_packs.py)\n')
            f.write('# %s, lifted from the PAL disc so any disc can offer it.\n' % name)
            # !menu is DRAWN by the game, whose space character is '_'.
            f.write('!font=latin\n!code=%s\n!name=%s\n!menu=%s\n'
                    % (code, name, name.replace(' ', '_')))
            for k, v in entries.items():
                f.write('%s=%s\n' % (esc(k if isinstance(k, bytes)
                                         else k.encode('latin-1')), esc(v)))
        story = sum(1 for k in entries if k.startswith(('MAP', 'COMMON')))
        print('%-3s %-8s %5d entries (%d story, %d item, %d menu)  %s KB'
              % (code, name, len(entries), story, len(items), len(menus),
                 '{:,}'.format(os.path.getsize(out) // 1024)))


if __name__ == '__main__':
    main()
