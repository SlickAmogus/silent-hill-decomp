#!/usr/bin/env python3
"""Polish for what the translator's file left blank.

SilentHill_PL_translation.txt is the translator's work and is not edited here.
This fills the entries it left empty, and -- just as importantly -- records the
ones where English IS the Polish, so "nobody looked at this" stops being
indistinguishable from "we looked and it does not change".

Most of what was left is the second kind. Of 26 story lines, 19 are names
(Harry, Cybil, Alessa, K. Gordon, Dahlia Gillespie) which are the same word in
Polish, and two are the engine's own "NO STAGE!" placeholder. Of 25 menu rows,
16 are technical terms (PGXP, ACES, Reinhard, Aniso 8x) that Polish keeps.

Fed to import_translation.py with --supplement; that tool drops a translation
identical to the English anyway, so SAME entries are documentation and the
runtime fallback produces the same text either way.
"""

# --- Entries where English is already the Polish ---------------------------
SAME = {
    # Names, and the engine placeholder that is not shown to players.
    'MAP0_S00.19', 'MAP1_S04.15', 'MAP2_S00.69', 'MAP2_S03.15', 'MAP3_S04.18',
    'MAP3_S05.16', 'MAP4_S01.17', 'MAP4_S01.98', 'MAP4_S01.99', 'MAP4_S03.16',
    'MAP4_S04.48', 'MAP4_S04.79', 'MAP6_S01.15', 'MAP6_S01.16', 'MAP6_S01.51',
    'MAP6_S04.17', 'MAP6_S04.19', 'MAP7_S01.17', 'MAP7_S02.111',
    'MAP7_S03.86', 'MAP7_S03.90',
    # Technical terms and proper nouns.
    'MENU.Stereo', 'MENU.PGXP', 'MENU.CRT', 'MENU.ACES', 'MENU.Reinhard',
    'MENU.PSX_Retro', 'MENU.Aniso_2x', 'MENU.Aniso_4x', 'MENU.Aniso_8x',
    'MENU.Aniso_16x', 'MENU.30_Hz', 'MENU.60_Hz',
    'QUICK.FOV', 'QUICK.Stereo', 'QUICK.HRTF', 'QUICK.Noclip',
    'ITEM_NAME.68', 'ITEM_NAME.93', 'ITEM_NAME.102', 'ITEM_NAME.131',
    # The translator typed these out as the English rather than leaving them
    # blank, so it is their decision and not an oversight -- and the right one:
    # these are the words Polish software uses. Not overridden here.
    'MENU.Antialiasing', 'MENU.Post_Process', 'MENU.Tone_Mapping',
    'MENU.Dither',
}

# --- Story: the interjections, which do change ----------------------------
# Codes and their spacing are kept exactly; only the word is Polish.
STORY = {
    'MAP0_S01.24': '~J0(2.0) Aha? ~E',
    'MAP0_S01.34': '~J0(2.0) Hmm... ~E',
    'MAP0_S01.42': '~J0(1.5) Hmf. ~E',
    'MAP5_S03.31': '~J1(0.9) Hmf. ~E',
    'MAP6_S04.49': '~J0(1.2) Co? ~E',
}

# --- Menus (game font; the Polish atlas carries ą ć ę ł ń ó ś ź ż) --------
# Budgets are the label column, so these stay inside the English width.
MENU = {
    'English':      'Angielski',
    'German':       'Niemiecki',
    'French':       'Francuski',
    'Spanish':      'Hiszpański',
    'Italian':      'Włoski',
    'Japanese':     'Japoński',
    'Chinese':      'Chiński',
    'EXIT':         'WYJŚCIE',
    'OTS_FOV':      'FOV_OTS',
}

# --- F10 overlay (TrueType, so any letter is fine) ------------------------
QUICK = {
    'Debug':           'Debugowanie',
    'Aspect Trim':     'Przycięcie obrazu',
    'Vertical Shift':  'Przesunięcie pionowe',
    'Cutscene Shift':  'Przesunięcie scenek',
    'Horizontal FOV':  'FOV poziomy',
    'Vertical FOV':    'FOV pionowy',
    'Pixel Aspect':    'Proporcje pikseli',
    'Quad':            'Kwadrofonia',
    'Infinite ammo':   'Nieskończona amunicja',
    'Spawn':           'Przywołanie',
}
