#!/usr/bin/env python3
"""Append the save/load and brightness rows to s_MenuTr for de/fr/es/it.

These are raw literals at their draw site rather than table rows, so the
extractor never saw them and no translator could reach them -- which is why the
save/load screen stayed English in every language. They are in the template
now; this fills the four the compiled table serves, which covers both a PAL
disc and the packs of the same name.

Layout is load-bearing in several of them. The leading underscores on the
save/load rows and the trailing ones on the brightness labels are padding the
screen depends on, and the gap in "Yes          No" sets where the two words
sit, so each translation keeps the same shape.
"""
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MENU_C = os.path.join(HERE, '..', 'src', 'lang_menu.c')

# us literal -> (de, fr, es, it). None keeps the English.
ROWS = [
    ('--- Save/load screen (raw literals, not table rows) ---', [
        ('\x07________\x01New_save',
         ('\x07______Neu_speichern', '\x07_____Nouvelle_partie',
          '\x07_____Nueva_partida',  '\x07____Nuovo_salvataggio')),
        ('\x07____Crea\x01t\x01e_\x01n\x01e\x01w_\x01fi\x01le',
         ('\x07____Datei_erstellen', '\x07____Créer_un_fichier',
          '\x07____Crear_archivo',   '\x07____Crea_file')),
        ('\x07____Fil\x01e_\x01\x01is_\x01\x01da\x01ma\x01g\x01ed',
         ('\x07____Datei_beschädigt', '\x07____Fichier_endommagé',
          '\x07____Archivo_dañado',   '\x07____File_danneggiato')),
        ('\x07Out_of_blocks',
         ('\x07Keine_Blöcke_frei', '\x07Plus_de_blocs',
          '\x07Sin_bloques_libres',     '\x07Blocchi_esauriti')),
        ('\x07No_data_file',
         ('\x07Keine_Datei', '\x07Aucun_fichier',
          '\x07Sin_archivo', '\x07Nessun_file')),
    ]),
    ('--- Save-point name ---', [
        ('Bus', ('Bus', 'Bus', 'Autobús', 'Autobus')),
    ]),
    ('--- Brightness screen. The trailing spaces are the layout. ---', [
        ('BRIGHTNESS_',  ('HELLIGKEIT_', 'LUMINOSITE_', 'BRILLO_', 'LUMINOSITA_')),
        ('CONTRAST_____', ('KONTRAST_____', 'CONTRASTE____', 'CONTRASTE____',
                           'CONTRASTO____')),
        ('SATURATION_',  ('SATTIGUNG_', 'SATURATION_', 'SATURACION_', 'SATURAZIONE_')),
    ]),
    ('--- Controller presets ---', [
        ('USER',   ('BENUTZER', 'PERSO', 'USUARIO', 'UTENTE')),
        ('TYPE_1', ('TYP_1', 'TYPE_1', 'TIPO_1', 'TIPO_1')),
        ('TYPE_2', ('TYP_2', 'TYPE_2', 'TIPO_2', 'TIPO_2')),
        ('TYPE_3', ('TYP_3', 'TYPE_3', 'TIPO_3', 'TIPO_3')),
    ]),
]

HEX = set('0123456789abcdefABCDEF')


def c_str(s):
    """A C literal of the Latin-1 bytes, split after a \\xNN escape whose next
    character is a hex digit -- the file's own style."""
    out = []
    for ch in s:
        b = ch.encode('latin-1')[0]
        if 0x20 <= b < 0x7F and ch not in '"\\':
            out.append(ch)
        elif ch == '"' or ch == '\\':
            out.append('\\' + ch)
        else:
            out.append('\\x%02X' % b)
            out.append('SPLIT')
    text = ''.join(out)
    parts = text.split('SPLIT')
    res = parts[0]
    for nxt in parts[1:]:
        res += ('" "' + nxt) if (nxt and nxt[0] in HEX) else nxt
    return '"' + res + '"'


def main():
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass

    src = io.open(MENU_C, encoding='utf-8', errors='surrogateescape').read()
    have = set(re.findall(r'\{\s*"((?:[^"\\]|\\.)*)"\s*,\s*\{', src))

    lines, added = [], 0
    for title, rows in ROWS:
        body = []
        for us, tr in rows:
            if c_str(us)[1:-1] in have:
                print('  skip (already present): %r' % us)
                continue
            body.append('    { %s,\n        { %s } },'
                        % (c_str(us), ', '.join(c_str(t) for t in tr)))
            added += 1
        if body:
            lines.append('\n    /* %s */' % title)
            lines.extend(body)

    if not added:
        print('nothing to add')
        return
    at = src.index('\n};', src.index('s_MenuTr[] = {'))
    src = src[:at] + '\n' + '\n'.join(lines) + src[at:]
    io.open(MENU_C, 'w', encoding='utf-8', newline='\n').write(src)
    print('added %d rows to s_MenuTr' % added)


if __name__ == '__main__':
    main()
