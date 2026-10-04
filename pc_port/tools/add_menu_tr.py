#!/usr/bin/env python3
"""Append the untranslated s_MenuTr rows for German, French, Spanish, Italian.

Retail PAL localized the story and the item text and left the MENUS in English
-- VIN/SAVELOAD.BIN exists once, in English, and the German strings on the disc
are all in VIN2/MAP*.BIN (dialogue) and VIN/ITEM_GER.BIN (items). So there is
nothing to extract for these rows and s_MenuTr is where they have to be
written, which is why that table exists at all.

Rows are emitted in the table's own conventions:
  * '_' is the rendered space.
  * Source stays ASCII: an accented letter becomes a \\xNN escape, and the
    string is split when the next character is a hex digit, so "Zur\\xFC" "ck"
    cannot read as one long escape. That is the existing style in the file.
  * NULL keeps English, which is right for a term that does not translate.

Character budgets come from the NOTE lines in
localization/SilentHill_EN_for_translation.txt and are checked before writing.
"""
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MENU_C = os.path.join(HERE, '..', 'src', 'lang_menu.c')

# us literal -> (de, fr, es, it). None = keep English.
# Budgets noted per block; German is the one that runs long.
ROWS = [
    ('--- Language row values ---', [
        ('English',  ('Englisch',     'Anglais',    'Inglés',   'Inglese')),
        ('German',   ('Deutsch',      'Allemand',   'Alemán',   'Tedesco')),
        ('French',   ('Französisch', 'Français', 'Francés', 'Francese')),
        ('Spanish',  ('Spanisch',     'Espagnol',   'Español',  'Spagnolo')),
        ('Italian',  ('Italienisch',  'Italien',    'Italiano',      'Italiano')),
        ('Japanese', ('Japanisch',    'Japonais',   'Japonés',  'Giapponese')),
        ('Chinese',  ('Chinesisch',   'Chinois',    'Chino',         'Cinese')),
    ]),
    ('--- Inventory: whether the item can be used here (max ~10) ---', [
        ('==Use_OK==',  ('==Geht==',  '==OK==',   '==Sí==',  '==Sì==')),
        ('==Use_OK?==', ('==Geht?==', '==OK?==',  '==Sí?==', '==Sì?==')),
        ('==Use_NG==',  ('==Nein==',  '==Non==',  '==No==',       '==No==')),
    ]),
    ('--- Inventory + pause ---', [
        ('Equip',          ('Anlegen',   'Équiper',  'Equipar',   'Equipaggia')),
        ('Equipment',      ('Ausrüstung', 'Équipement', 'Equipo', 'Equipaggiam.')),
        ('Detail',         ('Details',   'Détails',  'Detalles',  'Dettagli')),
        ('Status',         (None,        'Statut',        'Estado',    'Stato')),
        ('Exit',           ('Zurück', 'Retour',      'Atrás', 'Indietro')),
        ('Press',          ('Drücken', 'Appuyez',    'Pulsa',     'Premi')),
        ('PAUSE',          (None,        None,            'PAUSA',     'PAUSA')),
        ('PAUSED',         ('PAUSIERT',  'EN_PAUSE',      'EN_PAUSA',  'IN_PAUSA')),
        ('NEXT_GAME_MODE', ('NÄCHSTER_MODUS', 'MODE_SUIVANT', 'MODO_SIGUIENTE', 'MODO_SUCCESSIVO')),
        ('Auto_Aiming',    ('Auto-Zielen', 'Visée_auto', 'Apuntado_auto.', 'Mira_auto.')),
    ]),
    ('--- Memory card messages ---', [
        ('Is_it_OK_to_overwrite?',      ('Überschreiben?', 'Remplacer_?', '¿Sobrescribir?', 'Sovrascrivere?')),
        ('Is_it_OK_to_format?',         ('Formatieren?', 'Formater_?', '¿Formatear?', 'Formattare?')),
        ('You_removed_the_MEMORY_CARD!', ('MEMORY_CARD_entfernt!', 'MEMORY_CARD_retirée_!', '¡MEMORY_CARD_retirada!', 'MEMORY_CARD_rimossa!')),
        ('Now_formatting...',           ('Formatiere...', 'Formatage...', 'Formateando...', 'Formattazione...')),
        ('Unable_to_create_a_new_file.', ('Datei_nicht_erstellbar.', 'Création_impossible.', 'No_se_puede_crear.', 'Impossibile_creare.')),
        ('Finished_saving.',            ('Gespeichert.', 'Sauvegardé.', 'Guardado.', 'Salvato.')),
        ('Failed_to_save!',             ('Speichern_fehlgeschlagen!', 'Échec_de_sauvegarde_!', '¡Error_al_guardar!', 'Salvataggio_fallito!')),
        ('The_data_is_not_found!',      ('Keine_Daten_gefunden!', 'Données_introuvables_!', '¡Datos_no_encontrados!', 'Dati_non_trovati!')),
        ('The_data_is_damaged!',        ('Daten_beschädigt!', 'Données_corrompues_!', '¡Datos_dañados!', 'Dati_danneggiati!')),
        ('Failed_to_load!',             ('Laden_fehlgeschlagen!', 'Échec_du_chargement_!', '¡Error_al_cargar!', 'Caricamento_fallito!')),
        ('Finished_loading.',           ('Geladen.', 'Chargé.', 'Cargado.', 'Caricato.')),
        ('Now_loading...',              ('Lade...', 'Chargement...', 'Cargando...', 'Caricamento...')),
    ]),
    ('--- Save-point names (retail PAL shows these in English) ---', [
        ('Next_fear',    ('Nächste_Angst', 'Peur_suivante', 'Siguiente_miedo', 'Prossima_paura')),
        ('Infirmary',    ('Krankenzimmer', 'Infirmerie', 'Enfermería', 'Infermeria')),
        ('Church',       ('Kirche',    'Église',   'Iglesia',   'Chiesa')),
        ('Police',       ('Polizei',   'Police',        'Policía', 'Polizia')),
        ('Reception',    ('Rezeption', 'Réception', 'Recepción', 'Reception')),
        ('Jewelry_shop', ('Juwelier',  'Bijouterie',    'Joyería', 'Gioielleria')),
        ('Antique_shop', ('Antiquitäten', 'Antiquaire', 'Anticuario', 'Antiquariato')),
        ('Bridge',       ('Brücke', 'Pont',         'Puente',    'Ponte')),
        ('Sewer',        ('Kanal',     'Égout',     'Alcantarilla', 'Fogna')),
    ]),
    ('--- Results screen (a value follows each label) ---', [
        ('Walking_distance',           ('Gehstrecke', 'Distance', 'Distancia', 'Distanza')),          # ~16
        ('Defeated_enemy_by_fighting', ('Im_Kampf_besiegt', 'Vaincus_au_combat', 'Vencidos_luchando', 'Sconfitti_in_lotta')),  # ~23
        ('Short_range_shots',          ('Nahschüsse', 'Tirs_proches', 'Tiros_cercanos', 'Colpi_vicini')),   # ~19
        ('Middle_range_shots',         ('Mittelschüsse', 'Tirs_moyens', 'Tiros_medios', 'Colpi_medi')),
        ('Long_range_shots',           ('Fernschüsse', 'Tirs_lointains', 'Tiros_lejanos', 'Colpi_lontani')),
    ]),
    ('--- PC Options rows (max ~14..17) ---', [
        ('Next_Page',      ('Nächste', 'Suivant',  'Siguiente', 'Avanti')),
        ('Prev_Page',      ('Zurück',  'Précédent', 'Anterior', 'Indietro')),
        ('Back',           ('Zurück',  'Retour',   'Atrás', 'Indietro')),
        ('Preload_Chunks', ('Vorladen',  'Préchargement', 'Precarga', 'Precarica')),
        ('Bullet_Decals',  ('Einschlüsse', 'Impacts', 'Impactos', 'Fori_colpi')),
        ('Aim_Assist',     ('Zielhilfe', 'Aide_visée', 'Ayuda_mira', 'Aiuto_mira')),
        ('Aim_Zoom',       ('Zielzoom',  'Zoom_visée', 'Zoom_mira', 'Zoom_mira')),
        ('OTS_Aim_Zoom',   ('OTS-Zielzoom', 'Zoom_OTS', 'Zoom_OTS', 'Zoom_OTS')),
        ('OTS_Aim_In_TPS', ('OTS_in_TPS', 'OTS_en_TPS', 'OTS_en_TPS', 'OTS_in_TPS')),
        ('Minimap_Scale',  ('Kartengröße', 'Échelle_carte', 'Escala_mapa', 'Scala_mappa')),
        ('Crosshair',      ('Fadenkreuz', 'Viseur',   'Reticícula', 'Mirino')),
        ('Crosshair_Size', ('Kreuzgröße', 'Taille_viseur', 'Tamaño_mira', 'Dim._mirino')),
        ('Text_Size',      ('Textgröße', 'Taille_texte', 'Tamaño_texto', 'Dim._testo')),
        ('[R]_Reset',      ('[R]_Zurücksetzen', '[R]_Réinit.', '[R]_Reiniciar', '[R]_Ripristina')),
    ]),
    ('--- Setting values (max ~11). A technical term keeps its English. ---', [
        ('Fullscreen', ('Vollbild', 'Plein_écran', 'Completa', 'Schermo_int.')),
        ('Bilinear',   (None,       'Bilinéaire', 'Bilineal', 'Bilineare')),
        ('Trilinear',  (None,       'Trilinéaire', 'Trilineal', 'Trilineare')),
        ('Vignette',   (None,       None,       'Viñeta',  'Vignettatura')),
        ('VSync',      (None, None, None, None)),
        ('PGXP',       (None, None, None, None)),
        ('Aniso_2x',   (None, None, None, None)),
        ('Aniso_4x',   (None, None, None, None)),
        ('Aniso_8x',   (None, None, None, None)),
        ('Aniso_16x',  (None, None, None, None)),
        ('CRT',        (None, None, None, None)),
        ('PSX_Retro',  (None, None, None, None)),
        ('Reinhard',   (None, None, None, None)),
        ('ACES',       (None, None, None, None)),
        ('30_Hz',      (None, None, None, None)),
        ('60_Hz',      (None, None, None, None)),
    ]),
]

HEX = set('0123456789abcdefABCDEF')


def c_str(s):
    """UTF-8 -> an ASCII C literal of the Latin-1 bytes, escaping accents.

    A \\xNN escape swallows every following hex digit, so the literal is split
    after one whose next character is a hex digit -- the file's own style."""
    if s is None:
        return 'NULL'
    out = ['"']
    for ch in s:
        b = ch.encode('latin-1')[0]
        if b < 0x80:
            if ch == '"' or ch == '\\':
                out.append('\\' + ch)
            else:
                out.append(ch)
        else:
            out.append('\\x%02X' % b)
            out.append('SPLIT')
    text = ''.join(out) + '"'
    # Apply the splits only where the next character is a hex digit.
    parts = text.split('SPLIT')
    res = parts[0]
    for nxt in parts[1:]:
        if nxt and nxt[0] in HEX:
            res += '" "' + nxt
        else:
            res += nxt
    return res


def visible(s):
    return len(s.replace('_', ' ')) if s else 0


def main():
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass

    src = io.open(MENU_C, encoding='utf-8', errors='surrogateescape').read()
    have = set(re.findall(r'\{\s*"((?:[^"\\]|\\.)*)"\s*,\s*\{', src))

    lines = []
    added = 0
    for title, rows in ROWS:
        body = []
        for us, tr in rows:
            if us in have:
                print('  skip (already present): %s' % us)
                continue
            if all(t is None for t in tr):
                body.append('    { %-34s { NULL, NULL, NULL, NULL } },' % (c_str(us) + ','))
            else:
                cols = ', '.join(c_str(t) for t in tr)
                body.append('    { %s,\n        { %s } },' % (c_str(us), cols))
            added += 1
        if body:
            lines.append('\n    /* %s */' % title)
            lines.extend(body)

    if not added:
        print('nothing to add')
        return

    marker = '\n};'
    at = src.rindex(marker, 0, src.index('\n};') + 3) if False else src.index(
        '\n};', src.index('s_MenuTr[] = {'))
    src = src[:at] + '\n' + '\n'.join(lines) + src[at:]
    io.open(MENU_C, 'w', encoding='utf-8', newline='\n').write(src)
    print('added %d rows to s_MenuTr' % added)

    # Budgets, so a long German label is caught here and not on screen.
    print('\nlongest per column (visible characters):')
    for i, name in enumerate(('de', 'fr', 'es', 'it')):
        worst = max(((visible(t[i]), us) for _h, rows in ROWS for us, t in rows if t[i]),
                    default=(0, ''))
        print('  %s  %2d  %s' % (name, worst[0], worst[1]))


if __name__ == '__main__':
    main()
