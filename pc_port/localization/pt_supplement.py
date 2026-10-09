#!/usr/bin/env python3
"""Brazilian Portuguese for what the contributed file left blank.

SilentHill_PT_translation.txt is the translator's work and is not edited here.
Almost everything they left is text that should not change, and that is now
recorded rather than looking like an oversight -- see SAME below.

What genuinely needed translating was the Language row's own names, which only
became reachable after Portuguese shipped.
"""

# --- Entries where English is already the Portuguese -----------------------
SAME = {
    # Character names, and the engine placeholder players never see.
    'MAP0_S00.19', 'MAP1_S04.15', 'MAP2_S00.69', 'MAP2_S03.15', 'MAP3_S04.18',
    'MAP3_S05.16', 'MAP4_S01.17', 'MAP4_S01.98', 'MAP4_S01.99', 'MAP4_S03.16',
    'MAP4_S04.48', 'MAP4_S04.79', 'MAP6_S01.15', 'MAP6_S01.51', 'MAP6_S04.17',
    'MAP6_S04.19', 'MAP7_S01.17', 'MAP7_S02.111', 'MAP7_S03.86', 'MAP7_S03.90',
    # "Hmmm..." is written the same way in Portuguese.
    'MAP0_S01.34',
    # Words Portuguese keeps as they are, and the graphics terms games here
    # ship untranslated.
    'MENU.NORMAL', 'MENU.Normal', 'MENU.Status', 'MENU.VSync', 'MENU.PGXP',
    'MENU.Antialiasing', 'MENU.OTS_FOV', 'MENU.Dither', 'MENU.Bilinear',
    'MENU.Trilinear', 'MENU.Aniso_2x', 'MENU.Aniso_4x', 'MENU.Aniso_8x',
    'MENU.Aniso_16x', 'MENU.CRT', 'MENU.Scanlines', 'MENU.PSX_Retro',
    'MENU.Reinhard', 'MENU.ACES', 'MENU.30_Hz', 'MENU.60_Hz',
    'QUICK.HUD', 'QUICK.FOV', 'QUICK.Quad', 'QUICK.HRTF', 'QUICK.Noclip',
    # Proper nouns.
    'ITEM_NAME.68', 'ITEM_NAME.93', 'ITEM_NAME.102', 'ITEM_NAME.131',
}

# --- The Language row, which Portuguese can now appear on ------------------
# Written with their accents: ê is on the retail PAL atlas and ã is one of the
# two this language builds, so "Alemão" is also a live test of that.
MENU = {
    'English':  'Inglês',
    'German':   'Alemão',
    'French':   'Francês',
    'Spanish':  'Espanhol',
    'Italian':  'Italiano',
    'Japanese': 'Japonês',
    'Chinese':  'Chinês',
}
