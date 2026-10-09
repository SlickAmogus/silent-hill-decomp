/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "font_region.h"
#include "lang_text.h"
#include "main/fsqueue.h"
#include <stdlib.h>
#include "lang_ru.h"
#include "pc_title_style.h"

#include <string.h> /* memcpy */

#include "game.h"
#include "bodyprog/bodyprog.h"           /* g_Font16AtlasImg */
#include "bodyprog/text/text_draw.h"     /* GLYPH_TABLE_ASCII_OFFSET, FONT_12X16_* */
#include "main/fileinfo.h"               /* g_GameRegion */
#include "pc_config.h"
#include "sh_log.h"

/* US kerning table — must stay byte-identical to FONT_12X16_GLYPH_WIDTHS in
 * text_draw.c (the draw sites read it through g_FontLayout on PC). */
static const unsigned char s_GlyphWidths_USA[84] = {
    3,  7,  7,  11, 11, 4,  10, 4,  6,  10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 4,  4,
    10, 11, 10, 8,  13, 12, 12, 12, 13, 11, 11, 13, 12, 9,  9,  12, 12, 13, 12, 13, 11,
    13, 12, 10, 11, 13, 12, 12, 12, 11, 12, 6,  4,  6,  8,  0,  3,  9,  10, 9,  9,  9,
    7,  11, 11, 6,  6,  10, 6,  13, 11, 10, 11, 10, 8,  8,  7,  10, 10, 12, 10, 10, 9
};

/* EUR: cells 0-83 identical to US; 84-119 accents, extracted from the
 * decrypted SLES-01514 BODYPROG at 0x8002689C (widths 0 = unused cells and
 * the zero-advance combining marks 114/119). */
static const unsigned char s_GlyphWidths_EUR[120] = {
    3,  7,  7,  11, 11, 4,  10, 4,  6,  10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 4,  4,
    10, 11, 10, 8,  13, 12, 12, 12, 13, 11, 11, 13, 12, 9,  9,  12, 12, 13, 12, 13, 11,
    13, 12, 10, 11, 13, 12, 12, 12, 11, 12, 6,  4,  6,  8,  0,  3,  9,  10, 9,  9,  9,
    7,  11, 11, 6,  6,  10, 6,  13, 11, 10, 11, 10, 8,  8,  7,  10, 10, 12, 10, 10, 9,
    11, 9,  9,  9,  9,  9,  0,  0,  9,  9,  9,  9,  9,  6,  6,  6,  6,  0,  11, 10,
    10, 10, 10, 10, 11, 0,  10, 10, 10, 10, 0,  8,  7,  12, 12, 0
};

/* Polish (pc_port/assets/gamedata/lang/pl.lang) is on no retail disc, so its
 * glyphs are built into the FONT16 atlas at load time — see
 * Font_PatchPolishGlyphs. Four blank accent cells (90/91/101/109, width 0 in
 * retail) and four of the six cells past the 120-glyph count but inside the
 * 21x6 grid take the new letterforms; the rest of Polish composes from the
 * existing acute mark. Widths mirror each glyph's base letter. This layout is
 * installed only while the Polish pack is active, so every other language
 * keeps the retail table byte-for-byte. */
static const unsigned char s_GlyphWidths_EUR_PL[126] = {
    3,  7,  7,  11, 11, 4,  10, 4,  6,  10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 4,  4,
    10, 11, 10, 8,  13, 12, 12, 12, 13, 11, 11, 13, 12, 9,  9,  12, 12, 13, 12, 13, 11,
    13, 12, 10, 11, 13, 12, 12, 12, 11, 12, 6,  4,  6,  8,  0,  3,  9,  10, 9,  9,  9,
    7,  11, 11, 6,  6,  10, 6,  13, 11, 10, 11, 10, 8,  8,  7,  10, 10, 12, 10, 10, 9,
    11, 9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  6,  6,  6,  6,  6,  11, 10,
    10, 10, 10, 10, 11, 9,  10, 10, 10, 10, 0,  8,  7,  12, 12, 0,
    /* 120-125: A-ogonek, E-ogonek, L-stroke, combining dot, spare, spare. */
    12, 11, 12, 0,  0,  0
};

/* Russian needs sixty-six letterforms, which no amount of free cells in the
 * retail atlas can hold, so its pack replaces the atlas outright with the one
 * the consolgames.ru PAL translation drew -- uppercase Cyrillic over the Latin
 * capitals, lowercase over the accent block. lang_ru.c already carries the byte
 * table for that layout, and those bytes deliberately avoid 0x96/0x9C/0xA1, the
 * three the retail EUR scheme special-cases, so Font_MapChar's existing
 * arithmetic (cell = byte - 0x27) addresses every one of them unchanged.
 * Latin CAPITALS are gone while this is active, exactly as on the fan disc; the
 * atlas is re-read on a language switch, so stepping off Russian restores it. */
#include "font_ru.inc"

static const s_FontLayout s_FontLayout_USA    = { 84,  240, 1, 0x10, 0x7FD3, 30, s_GlyphWidths_USA };
static const s_FontLayout s_FontLayout_EUR    = { 120, 128, 6, 0x0C, 0x3FF3, 31, s_GlyphWidths_EUR };
static const s_FontLayout s_FontLayout_EUR_PL = { 126, 128, 6, 0x0C, 0x3FF3, 31, s_GlyphWidths_EUR_PL };
static const s_FontLayout s_FontLayout_EUR_RU = { 126, 128, 6, 0x0C, 0x3FF3, 31, FONT_RU_WIDTHS };

const s_FontLayout* g_FontLayout = &s_FontLayout_USA;

/* The Polish pack's byte window (0xA2-0xB3) overlaps letters a Cyrillic PAL
 * repaint puts in the same range, so the pack's decode branch is keyed on the
 * pack actually being active rather than on a wide glyph count — a fan disc
 * that extends the atlas to 126 cells reaches the same count. */
static int s_PolishLayoutActive;

/* Portuguese pack active: its accented CAPITALS draw as a combining mark plus
 * the base capital, the way retail builds A-acute and E-acute. */
static int s_PtLayoutActive;
static int s_RussianLayoutActive;

/* The PAL atlas, in use on a disc that does not carry it. Its 126 cells are
 * what every pack language draws through -- the accents the four PAL languages
 * need, and the cells Polish and Russian build their letters into -- so a pack
 * on a US or Japanese disc uploads it from gamedata/font/eur16.tim rather than
 * settling for the 84-cell US strip. It is 64 VRAM units wide at (768,128),
 * which stops short of the fire texture at 832; the Konami logo does cross it,
 * so the pre-title reload PAL already does is needed here too. */
static int s_EurAtlasImported;

#define EUR_ATLAS_PATH "gamedata/font/eur16.tim"

/* == FONT_12X16_LINE_COUNT_MAX (see font_region.h). Region-independent default
 * so USA and NTSC-J evaluate every site exactly as before; Font_ApplyRegionPatches
 * raises it to retail PAL's ten. */
int g_PcMapMsgLineMax = FONT_12X16_LINE_COUNT_MAX;

/* Atlas cells the Polish patch writes. */
#define PL_CELL_a_OGONEK 90
#define PL_CELL_e_OGONEK 91
#define PL_CELL_l_STROKE 101
#define PL_CELL_z_DOT    109
#define PL_CELL_A_OGONEK 120
#define PL_CELL_E_OGONEK 121
#define PL_CELL_L_STROKE 122
#define PL_CELL_DOT_MARK 123
#define PL_CELL_ACUTE    119 /* retail combining acute, reused for c/n/s/z */

/* Portuguese capital marks. They reuse the POLISH cells: Portuguese text never
 * contains a Polish letter, and Pc_LangInit re-reads FONT16 from the disc on
 * every language switch, so each language gets a clean atlas to build into. */
#define PT_CELL_A_TILDE  90
#define PT_CELL_O_TILDE  91
#define PT_CELL_A_CIRC   101
#define PT_CELL_E_CIRC   109
#define PT_CELL_O_CIRC   120
#define PT_CELL_A_GRAVE  121
#define PT_CELL_I_ACUTE  122
#define PT_CELL_U_ACUTE  123
#define CELL_NTILDE      102

/* Polish text bytes. The pack loader emits these; 0xA2..0xB3 are bytes the
 * retail EUR drawer sends to atlas cells 130..147 — past the grid, so it
 * draws nothing — and no PAL disc string contains them. `base` 0 means the
 * cell is a finished glyph; otherwise `cell` is a combining mark drawn 3px
 * above `base`, exactly how retail builds its uppercase accents. */
typedef struct {
    unsigned char byte;
    unsigned char cell;
    char          base;
} s_PolishChar;

static const s_PolishChar s_PolishChars[] = {
    { 0xA2, PL_CELL_a_OGONEK, 0   }, /* a-ogonek */
    { 0xA3, PL_CELL_ACUTE,    'c' }, /* c-acute  */
    { 0xA4, PL_CELL_e_OGONEK, 0   }, /* e-ogonek */
    { 0xA5, PL_CELL_l_STROKE, 0   }, /* l-stroke */
    { 0xA6, PL_CELL_ACUTE,    'n' }, /* n-acute  */
    { 0xA7, 104,              0   }, /* o-acute -- retail Latin-1 cell */
    { 0xA8, PL_CELL_ACUTE,    's' }, /* s-acute  */
    { 0xA9, PL_CELL_ACUTE,    'z' }, /* z-acute  */
    { 0xAA, PL_CELL_z_DOT,    0   }, /* z-dot    */
    { 0xAB, PL_CELL_A_OGONEK, 0   }, /* A-ogonek */
    { 0xAC, PL_CELL_ACUTE,    'C' }, /* C-acute  */
    { 0xAD, PL_CELL_E_OGONEK, 0   }, /* E-ogonek */
    { 0xAE, PL_CELL_L_STROKE, 0   }, /* L-stroke */
    { 0xAF, PL_CELL_ACUTE,    'N' }, /* N-acute  */
    { 0xB0, PL_CELL_ACUTE,    'O' }, /* O-acute  */
    { 0xB1, PL_CELL_ACUTE,    'S' }, /* S-acute  */
    { 0xB2, PL_CELL_ACUTE,    'Z' }, /* Z-acute  */
    { 0xB3, PL_CELL_DOT_MARK, 'Z' }  /* Z-dot    */
};

/* Fan-translation patches repaint FONT16 glyphs on the disc and retune the
 * BODYPROG kerning table to match (e.g. the Spanish fandub turns ';' into a
 * full-width 'á'). Swap in the disc's widths, everything else unchanged.
 *
 * `count` comes from the disc too, because a patch can also make the atlas
 * BIGGER: retail PAL declares 120 glyphs but its grid is 21x6, and the Russian
 * consolgames.ru repaint fills the six spare cells (120-125) with ъ ы ь э ю я
 * and extends the kerning table to match. Rendered at the retail count those
 * six letters fail Font_MapChar's range check and vanish from every line of
 * text on the disc. */
void Font_SetGlyphWidths(const unsigned char* widths, int count)
{
    static s_FontLayout  s_overrideLayout;
    static unsigned char s_overrideWidths[FONT_ATLAS_CELL_MAX];

    if (count < 1)
        return;
    if (count > FONT_ATLAS_CELL_MAX)
        count = FONT_ATLAS_CELL_MAX;

    memcpy(s_overrideWidths, widths, (size_t)count);
    s_overrideLayout             = *g_FontLayout;
    s_overrideLayout.glyphCount  = count;
    s_overrideLayout.glyphWidths = s_overrideWidths;
    g_FontLayout                 = &s_overrideLayout;
}

int Font_MapChar(unsigned int charCode, s_GlyphEmit emits[2])
{
    const s_FontLayout* layout = g_FontLayout;
    int                 cell;

    if (charCode < 0x80)
    {
        cell = (int)charCode - GLYPH_TABLE_ASCII_OFFSET;
    }
    else if (layout->glyphCount <= FONT_12X16_GLYPH_COUNT)
    {
        /* No accent cells in the US atlas. Fan-translated USA discs unlock
         * the port's Latin-1 menu translations (lang_menu.c), so instead of
         * silently dropping the byte, render the unaccented base letter —
         * "Vibracion" beats "Vibracin". Inverted punctuation has no base and
         * is dropped. Vanilla US text never contains bytes >= 0x80, so this
         * path can't affect it. */
        switch (charCode)
        {
            case 0xE0: case 0xE1: case 0xE2: case 0xE4: cell = 'a'; break;
            case 0xE7:                                  cell = 'c'; break;
            case 0xE8: case 0xE9: case 0xEA: case 0xEB: cell = 'e'; break;
            case 0xEC: case 0xED: case 0xEE: case 0xEF: cell = 'i'; break;
            case 0xF1:                                  cell = 'n'; break;
            case 0xF2: case 0xF3: case 0xF4: case 0xF6: cell = 'o'; break;
            case 0xF9: case 0xFA: case 0xFB: case 0xFC: cell = 'u'; break;
            case 0xC0: case 0xC1: case 0xC2: case 0xC4: cell = 'A'; break;
            case 0xC7:                                  cell = 'C'; break;
            case 0xC8: case 0xC9: case 0xCA: case 0xCB: cell = 'E'; break;
            case 0xCC: case 0xCD:                       cell = 'I'; break;
            case 0xD1:                                  cell = 'N'; break;
            case 0xD2: case 0xD3: case 0xD6:            cell = 'O'; break;
            case 0xD9: case 0xDA: case 0xDC:            cell = 'U'; break;
            case 0x9C:                                  cell = 'o'; break;
            case 0x96:                                  cell = '-'; break;
            /* The PAL languages now run on this atlas too (their packs), and
             * these three are common enough that dropping them would be read
             * as missing text: Spanish opens 342 sentences with an inverted
             * question mark and German writes 191 sharp s. The inverted
             * exclamation has no home here -- the US atlas starts above '!'
             * and the drawer rewrites that byte anyway -- so it still goes. */
            case 0xBF:                                  cell = '?'; break;
            case 0x85:                                  cell = '.'; break;
            case 0xDF:
            {
                /* No sharp s in this atlas, and "ss" is what German itself
                 * writes without one. Two emissions, both advancing. */
                int ss = 's' - GLYPH_TABLE_ASCII_OFFSET;

                emits[0].cell    = ss;
                emits[0].dy      = 0;
                emits[0].advance = layout->glyphWidths[ss];
                emits[1].cell    = ss;
                emits[1].dy      = 0;
                emits[1].advance = layout->glyphWidths[ss];
                return 2;
            }
            default: return 0;
        }
        cell -= GLYPH_TABLE_ASCII_OFFSET;
    }
    else
    {
        /* Portuguese accented capitals: a combining mark plus the base
         * capital, exactly how retail draws A-acute and E-acute. Ahead of the
         * retail scheme, which would otherwise fold these into its 0x80-0xBF
         * arithmetic and lose them. */
        if (s_PtLayoutActive)
        {
            int mark = -1;
            int base = 0;

            switch (charCode)
            {
                case 0xC3: mark = PT_CELL_A_TILDE; base = 'A'; break;
                case 0xD5: mark = PT_CELL_O_TILDE; base = 'O'; break;
                case 0xC2: mark = PT_CELL_A_CIRC;  base = 'A'; break;
                case 0xCA: mark = PT_CELL_E_CIRC;  base = 'E'; break;
                case 0xD4: mark = PT_CELL_O_CIRC;  base = 'O'; break;
                case 0xC0: mark = PT_CELL_A_GRAVE; base = 'A'; break;
                case 0xCD: mark = PT_CELL_I_ACUTE; base = 'I'; break;
                case 0xDA: mark = PT_CELL_U_ACUTE; base = 'U'; break;
                default: break;
            }
            if (mark >= 0)
            {
                base             -= GLYPH_TABLE_ASCII_OFFSET;
                emits[0].cell    = mark;
                emits[0].dy      = -3; /* the lift retail gives its own A-acute */
                emits[0].advance = 0;
                emits[1].cell    = base;
                emits[1].dy      = 0;
                emits[1].advance = layout->glyphWidths[base];
                return 2;
            }
        }

        /* Polish, when its pack is active. Ahead of the retail scheme because
         * these bytes would otherwise fall into the 0x80-0xBF arithmetic and
         * be dropped. */
        if (s_PolishLayoutActive && charCode >= 0xA2 && charCode <= 0xB3)
        {
            const s_PolishChar* pc = &s_PolishChars[charCode - 0xA2];
            int                 base;

            if (pc->base == 0)
            {
                emits[0].cell    = pc->cell;
                emits[0].dy      = 0;
                emits[0].advance = layout->glyphWidths[pc->cell];
                return 1;
            }

            base             = pc->base - GLYPH_TABLE_ASCII_OFFSET;
            emits[0].cell    = pc->cell;
            /* Marks sit at the top of their own cell, so a capital (ink from
             * y=0) needs them lifted clear the way retail does it, while a
             * lowercase letter (x-height starts at y=4) wants them in place —
             * that is where retail bakes the accent on its own o-acute. */
            emits[0].dy      = (pc->base >= 'A' && pc->base <= 'Z') ? -3 : 0;
            emits[0].advance = 0;
            emits[1].cell    = base;
            emits[1].dy      = 0;
            emits[1].advance = layout->glyphWidths[base];
            return 2;
        }

        /* Retail EUR accent scheme (from the SLES BODYPROG text drawer):
         * a few cp1252/legacy pre-remaps, direct Latin-1 lowercase cells at
         * byte-0x8B, and two-emission combining marks for uppercase. */
        switch (charCode)
        {
            case 0x96: cell = '-' - GLYPH_TABLE_ASCII_OFFSET; break; /* en dash */
            case 0x9C: cell = 118; break;                           /* oe ligature */
            case 0xA1: cell = 116; break;                           /* inverted ! */
            case 0xBF: cell = 115; break;                           /* inverted ? */
            case 0xC7: cell = 117; break;                           /* C cedilla */

            default:
                if (charCode >= 0xDF && charCode != 0xFD)
                {
                    cell = (int)charCode - 0x8B;
                }
                else if (charCode >= 0xC0)
                {
                    /* 0xFD (y-acute) rides this path too — retail quirk:
                     * it degrades to diaeresis mark + '*' like the
                     * unsupported uppercase accents. */
                    /* Uppercase accent: zero-advance mark above, then the base
                     * letter (only A-acute/E-acute/A-O-U-diaeresis have real
                     * bases; the rest degrade to '*' exactly like retail). */
                    int base;

                    switch (charCode)
                    {
                        case 0xC1:
                        case 0xC4: base = 'A' - GLYPH_TABLE_ASCII_OFFSET; break;
                        case 0xC9: base = 'E' - GLYPH_TABLE_ASCII_OFFSET; break;
                        case 0xD6: base = 'O' - GLYPH_TABLE_ASCII_OFFSET; break;
                        case 0xDC: base = 'U' - GLYPH_TABLE_ASCII_OFFSET; break;
                        default:   base = '*' - GLYPH_TABLE_ASCII_OFFSET; break;
                    }

                    emits[0].cell    = (charCode == 0xC1 || charCode == 0xC9) ? 119 : 114;
                    emits[0].dy      = -3;
                    emits[0].advance = 0;
                    emits[1].cell    = base;
                    emits[1].dy      = 0;
                    emits[1].advance = layout->glyphWidths[base];
                    return 2;
                }
                else
                {
                    cell = (int)charCode - GLYPH_TABLE_ASCII_OFFSET; /* Retail arithmetic for 0x80-0xBF. */
                }
                break;
        }
    }

    if (cell < 0 || cell >= layout->glyphCount)
    {
        return 0;
    }

    emits[0].cell    = cell;
    emits[0].dy      = 0;
    emits[0].advance = layout->glyphWidths[cell];
    return 1;
}

/* One overlaid pixel of a built glyph: palette index `v` at cell-local x/y. */
typedef struct {
    unsigned char x, y, v;
} s_GlyphPix;

typedef struct {
    unsigned char cell;     /* destination atlas cell */
    short         srcCell;  /* base letter to copy first, -1 = start blank */
    unsigned char pixOff;   /* into s_PolishPix */
    unsigned char pixCount;
} s_GlyphBuild;

/* Diacritics follow the atlas's own idioms so the additions sit right next to
 * retail glyphs: the ogonek is the c-cedilla hook (cell 92) moved under the
 * letter's right side, the z-dot is the 'i' tittle (cell 66), and both keep
 * the font's 1-index drop shadow. */
static const s_GlyphPix s_PolishPix[] = {
    /* U+0105 a-ogonek */ { 7,13,0xA}, { 5,14,0xA}, { 6,14,0xA}, { 8,14,0x1}, { 6,15,0x1}, { 7,15,0x1},
    /* U+0119 e-ogonek */ { 6,13,0xA}, { 4,14,0xA}, { 5,14,0xA}, { 7,14,0x1}, { 5,15,0x1}, { 6,15,0x1},
    /* U+0142 l-stroke */ { 4, 5,0xA}, { 4, 6,0xA}, { 3, 6,0xA}, { 3, 7,0xA}, { 0, 7,0xA}, { 0, 8,0xA}, { 5, 5,0x1}, { 5, 6,0x1}, { 1, 9,0x1},
    /* U+017C z-dot    */ { 2, 1,0xA}, { 3, 1,0xA}, { 2, 2,0xB}, { 3, 2,0xB}, { 4, 2,0x1}, { 2, 3,0x1}, { 3, 3,0x1},
    /* U+0104 A-ogonek */ { 9,13,0xA}, { 7,14,0xA}, { 8,14,0xA}, {10,14,0x1}, { 8,15,0x1}, { 9,15,0x1},
    /* U+0118 E-ogonek */ { 7,13,0xA}, { 5,14,0xA}, { 6,14,0xA}, { 8,14,0x1}, { 6,15,0x1}, { 7,15,0x1},
    /* U+0141 L-stroke */ { 4, 5,0xA}, { 4, 6,0xA}, { 3, 6,0xA}, { 3, 7,0xA}, { 0, 7,0xA}, { 0, 8,0xA}, { 5, 5,0x1}, { 5, 6,0x1}, { 1, 9,0x1},
    /* combining dot   */ { 4, 0,0xA}, { 5, 0,0xA}, { 4, 1,0xB}, { 5, 1,0xB}, { 6, 1,0x1}, { 4, 2,0x1}, { 5, 2,0x1}
};

/* Copying the base letter (rather than shipping whole bitmaps) keeps the
 * letterform whatever the loaded FONT16 actually draws, so a repainted or
 * fan-patched atlas stays self-consistent. */
static const s_GlyphBuild s_PolishGlyphs[] = {
    { PL_CELL_a_OGONEK, 58, 0,  6 }, /* 'a' */
    { PL_CELL_e_OGONEK, 62, 6,  6 }, /* 'e' */
    { PL_CELL_l_STROKE, 69, 12, 9 }, /* 'l' */
    { PL_CELL_z_DOT,    83, 21, 7 }, /* 'z' */
    { PL_CELL_A_OGONEK, 26, 28, 6 }, /* 'A' */
    { PL_CELL_E_OGONEK, 30, 34, 6 }, /* 'E' */
    { PL_CELL_L_STROKE, 37, 40, 9 }, /* 'L' */
    { PL_CELL_DOT_MARK, -1, 49, 7 }
};

/* consolgames' uppercase Э, and the spare cell it is copied to. */
#define RU_CELL_E_OBERT 87
#define RU_CELL_E_ALT   90

#define ATLAS_COLS 21
#define CELL_W     12
#define CELL_H     16

static unsigned int PixGet(const unsigned char* p, int stride, int x, int y)
{
    unsigned char b = p[(y * stride) + (x >> 1)];
    return (x & 1) ? (unsigned int)(b >> 4) : (unsigned int)(b & 0xF);
}

static void PixSet(unsigned char* p, int stride, int x, int y, unsigned int v)
{
    unsigned char* b = &p[(y * stride) + (x >> 1)];

    *b = (x & 1) ? (unsigned char)((*b & 0x0F) | (v << 4))
                 : (unsigned char)((*b & 0xF0) | (v & 0xF));
}

/* ---- Portuguese letterforms, built from the atlas's own accents ----------
 *
 * Contributed with the pt translation. Portuguese needs 24 accented letters;
 * retail PAL draws all but ten. a-tilde and o-tilde have cells reserved
 * (88, 106) with real advances but ship BLANK, because no PAL language uses
 * them -- so pack text came out "N o" and "Op es". The eight accented capitals
 * degrade to a mark and a '*'.
 *
 * Every stroke is cut from the loaded atlas rather than drawn, so the ink,
 * the shadow and the palette match the font exactly and a repainted FONT16 is
 * followed for free. Cells that already hold ink are left alone, so a fan
 * repaint that fills them wins. */

static int CellBlank(const unsigned char* p, int stride, int cell)
{
    int ox = (cell % ATLAS_COLS) * CELL_W, oy = (cell / ATLAS_COLS) * CELL_H, x, y;

    for (y = 0; y < CELL_H; y++)
        for (x = 0; x < CELL_W; x++)
            if (PixGet(p, stride, ox + x, oy + y))
                return 0;
    return 1;
}

/* Twice the horizontal ink centre of a cell over rows y0.., or -1 if empty.
 * Doubled so the midpoint stays exact without a divide. */
static int CellInkCentre2(const unsigned char* p, int stride, int cell, int y0)
{
    int ox = (cell % ATLAS_COLS) * CELL_W, oy = (cell / ATLAS_COLS) * CELL_H;
    int mn = CELL_W, mx = -1, x, y;

    for (y = y0; y < CELL_H; y++)
        for (x = 0; x < CELL_W; x++)
            if (PixGet(p, stride, ox + x, oy + y))
            {
                if (x < mn) mn = x;
                if (x > mx) mx = x;
            }
    return (mx < 0) ? -1 : (mn + mx);
}

/* Ink bounding box of a cell over rows [y0, y1). Zero if those rows are empty. */
static int CellInkBox(const unsigned char* p, int stride, int cell, int y0, int y1,
                      int* x0, int* x1, int* yt, int* yb)
{
    int ox = (cell % ATLAS_COLS) * CELL_W, oy = (cell / ATLAS_COLS) * CELL_H, x, y, any = 0;

    *x0 = CELL_W; *x1 = -1; *yt = CELL_H; *yb = -1;
    for (y = y0; y < y1; y++)
        for (x = 0; x < CELL_W; x++)
            if (PixGet(p, stride, ox + x, oy + y))
            {
                any = 1;
                if (x < *x0) *x0 = x;
                if (x > *x1) *x1 = x;
                if (y < *yt) *yt = y;
                if (y > *yb) *yb = y;
            }
    return any;
}

/* a-tilde and o-tilde, built the way retail builds n-tilde: copy the base
 * letter, then lay n-tilde's own tilde over it, re-centred. The tilde is the
 * ink above the 'n' x-height, which is where the top of 'n' tells us to cut. */
static void Font_BuildTildeGlyphs(unsigned char* p, int stride)
{
    static const struct { unsigned char cell; char base; } s_Tilde[] = {
        { 88, 'a' }, { 106, 'o' }
    };
    int nCell = 'n' - GLYPH_TABLE_ASCII_OFFSET;
    int nOx   = (nCell % ATLAS_COLS) * CELL_W, nOy = (nCell / ATLAS_COLS) * CELL_H;
    int tOx   = (CELL_NTILDE % ATLAS_COLS) * CELL_W;
    int tOy   = (CELL_NTILDE / ATLAS_COLS) * CELL_H;
    int top   = CELL_H, ntC, i, x, y;

    for (y = 0; y < CELL_H && top == CELL_H; y++)
        for (x = 0; x < CELL_W; x++)
            if (PixGet(p, stride, nOx + x, nOy + y)) { top = y; break; }

    if (top < 2 || top == CELL_H || CellBlank(p, stride, CELL_NTILDE))
        return;
    ntC = CellInkCentre2(p, stride, CELL_NTILDE, top);

    for (i = 0; i < (int)(sizeof(s_Tilde) / sizeof(s_Tilde[0])); i++)
    {
        int bCell = s_Tilde[i].base - GLYPH_TABLE_ASCII_OFFSET;
        int bOx   = (bCell % ATLAS_COLS) * CELL_W, bOy = (bCell / ATLAS_COLS) * CELL_H;
        int dOx   = (s_Tilde[i].cell % ATLAS_COLS) * CELL_W;
        int dOy   = (s_Tilde[i].cell / ATLAS_COLS) * CELL_H;
        int bC, dx;

        if (!CellBlank(p, stride, s_Tilde[i].cell))
            continue;
        bC = CellInkCentre2(p, stride, bCell, top);
        if (bC < 0 || ntC < 0)
            continue;
        dx = (bC - ntC) >> 1;

        for (y = 0; y < CELL_H; y++)
            for (x = 0; x < CELL_W; x++)
                PixSet(p, stride, dOx + x, dOy + y, PixGet(p, stride, bOx + x, bOy + y));

        for (y = 0; y < top; y++)
            for (x = 0; x < CELL_W; x++)
            {
                unsigned int v = PixGet(p, stride, tOx + x, tOy + y);

                if (v && x + dx >= 0 && x + dx < CELL_W)
                    PixSet(p, stride, dOx + x + dx, dOy + y, v);
            }
    }
}

/* One combining-mark cell per accented capital, so each accent is centred on
 * its own letter rather than sharing one mark. Each is dropped so its bottom
 * row matches the retail acute's: Font_MapChar then emits it with the same
 * dy of -3 retail uses for A-acute, and it lands at exactly that height. */
static void Font_BuildPtCapitalMarks(unsigned char* p, int stride)
{
    static const struct { unsigned char dst, src; char lowerBase, capital; } s_Marks[] = {
        { PT_CELL_A_TILDE, CELL_NTILDE,   'n', 'A' },
        { PT_CELL_O_TILDE, CELL_NTILDE,   'n', 'O' },
        { PT_CELL_A_CIRC,  87,            'a', 'A' },
        { PT_CELL_E_CIRC,  87,            'a', 'E' },
        { PT_CELL_O_CIRC,  87,            'a', 'O' },
        { PT_CELL_A_GRAVE, 85,            'a', 'A' },
        { PT_CELL_I_ACUTE, PL_CELL_ACUTE, 0,   'I' },
        { PT_CELL_U_ACUTE, PL_CELL_ACUTE, 0,   'U' }
    };
    int ax0, ax1, ayt, ayb, i, x, y;

    if (!CellInkBox(p, stride, PL_CELL_ACUTE, 0, CELL_H, &ax0, &ax1, &ayt, &ayb))
        return;

    for (i = 0; i < (int)(sizeof(s_Marks) / sizeof(s_Marks[0])); i++)
    {
        int src = s_Marks[i].src, dst = s_Marks[i].dst;
        int cap = s_Marks[i].capital - GLYPH_TABLE_ASCII_OFFSET;
        int lim = CELL_H;
        int mx0, mx1, myt, myb, cx0, cx1, cyt, cyb, dx, dy;
        int sOx = (src % ATLAS_COLS) * CELL_W, sOy = (src / ATLAS_COLS) * CELL_H;
        int dOx = (dst % ATLAS_COLS) * CELL_W, dOy = (dst / ATLAS_COLS) * CELL_H;

        if (s_Marks[i].lowerBase)
        {
            int lb = s_Marks[i].lowerBase - GLYPH_TABLE_ASCII_OFFSET;
            int bx0, bx1, byt, byb;

            if (!CellInkBox(p, stride, lb, 0, CELL_H, &bx0, &bx1, &byt, &byb))
                continue;
            lim = byt; /* the accent is whatever sits above the base's x-height */
        }
        if (!CellInkBox(p, stride, src, 0, lim, &mx0, &mx1, &myt, &myb) ||
            !CellInkBox(p, stride, cap, 0, CELL_H, &cx0, &cx1, &cyt, &cyb))
            continue;

        dy = ayb - myb;
        dx = ((cx0 + cx1) - (mx0 + mx1)) >> 1;

        for (y = 0; y < CELL_H; y++)
            for (x = 0; x < CELL_W; x++)
                PixSet(p, stride, dOx + x, dOy + y, 0);

        for (y = 0; y < lim; y++)
            for (x = 0; x < CELL_W; x++)
            {
                unsigned int v = PixGet(p, stride, sOx + x, sOy + y);

                if (v && x + dx >= 0 && x + dx < CELL_W && y + dy >= 0 && y + dy < CELL_H)
                    PixSet(p, stride, dOx + x + dx, dOy + y + dy, v);
            }
    }
}

/* Portuguese: the two tildes every sentence needs, then the capital marks. */
void Font_PatchPortugueseGlyphs(void* pixels, int widthWords, int height)
{
    unsigned char* p      = (unsigned char*)pixels;
    int            stride = widthWords * 2;

    if (pixels == NULL || (widthWords * 4) < (ATLAS_COLS * CELL_W) || height < (6 * CELL_H))
        return;

    Font_BuildTildeGlyphs(p, stride);
    Font_BuildPtCapitalMarks(p, stride);
    SH_LOG("[FONT] Portuguese glyphs built into FONT16");
}

void Font_UsePortugueseLayout(void)
{
    if (g_FontLayout == &s_FontLayout_EUR || g_FontLayout == &s_FontLayout_USA)
    {
        /* The 126-cell layout, same as Polish: the capital marks live in cells
         * 120..123, past the 120 the plain PAL layout counts. */
        g_FontLayout     = &s_FontLayout_EUR_PL;
        s_PtLayoutActive = 1;
    }
}

/* Build the Polish letterforms into the freshly-read FONT16 pixel block,
 * before it is uploaded. Patching the source data rather than VRAM covers
 * every reload site (boot, Konami logo, title, save-select) for free and
 * needs no readback. `widthWords` is the TIM's 16-bit width; at 4bpp that is
 * 4 pixels per word. */
void Font_PatchPolishGlyphs(void* pixels, int widthWords, int height)
{
    unsigned char* p      = (unsigned char*)pixels;
    int            stride = widthWords * 2;
    int            i;

    /* Retail PAL FONT16 is a 21x6 grid of 12x16 cells = 252x96. */
    if (pixels == NULL || (widthWords * 4) < (ATLAS_COLS * CELL_W) || height < (6 * CELL_H))
    {
        SH_WARN("[FONT] Polish glyph patch skipped: atlas is %dx%d, need %dx%d",
                widthWords * 4, height, ATLAS_COLS * CELL_W, 6 * CELL_H);
        return;
    }

    for (i = 0; i < (int)(sizeof(s_PolishGlyphs) / sizeof(s_PolishGlyphs[0])); i++)
    {
        const s_GlyphBuild* g    = &s_PolishGlyphs[i];
        int                 dstX = (g->cell % ATLAS_COLS) * CELL_W;
        int                 dstY = (g->cell / ATLAS_COLS) * CELL_H;
        int                 x, y, j;

        for (y = 0; y < CELL_H; y++)
        {
            for (x = 0; x < CELL_W; x++)
            {
                unsigned int v = 0;

                if (g->srcCell >= 0)
                {
                    v = PixGet(p, stride,
                               ((g->srcCell % ATLAS_COLS) * CELL_W) + x,
                               ((g->srcCell / ATLAS_COLS) * CELL_H) + y);
                }
                PixSet(p, stride, dstX + x, dstY + y, v);
            }
        }

        for (j = 0; j < g->pixCount; j++)
        {
            const s_GlyphPix* q = &s_PolishPix[g->pixOff + j];

            PixSet(p, stride, dstX + q->x, dstY + q->y, q->v);
        }
    }

    SH_LOG("[FONT] Polish glyphs built into FONT16 (%d cells)",
           (int)(sizeof(s_PolishGlyphs) / sizeof(s_PolishGlyphs[0])));
}

void Font_UsePolishLayout(void)
{
    if (g_FontLayout == &s_FontLayout_EUR || g_FontLayout == &s_FontLayout_USA)
    {
        g_FontLayout         = &s_FontLayout_EUR_PL;
        s_PolishLayoutActive = 1;
    }
}

/* Point FONT16's descriptor at the PAL home and remember to feed it the PAL
 * atlas. No-op on a PAL disc, which is already there. */
void Font_UseEurAtlas(void)
{
    if (g_GameRegion == Region_EUR)
        return;

    /* Moving FONT16 to the PAL home leaves a hi-res font pack's override
     * registered against the old rect, so the pack silently stops applying.
     * Opt-in only, for that reason as much as any. */
    if (!g_PcConfig.crossRegionLanguages)
        return;

    /* Only move the descriptor if the atlas is actually there to put in it.
     * An install missing this file would otherwise point FONT16 at a VRAM home
     * nothing ever fills, and every glyph would draw from blank memory -- the
     * language would be unreadable rather than merely unaccented. Falling back
     * leaves the disc's own atlas in place, which is where this started. */
    {
        FILE* probe = fopen(EUR_ATLAS_PATH, "rb");

        if (probe == NULL)
        {
            SH_WARN("[FONT] %s is missing - keeping this disc's own atlas, so a "
                    "pack language draws base letters instead of accents",
                    EUR_ATLAS_PATH);
            return;
        }
        fclose(probe);
    }

    g_Font16AtlasImg.tPage[0] = 0;
    g_Font16AtlasImg.tPage[1] = 12;
    g_Font16AtlasImg.u        = 0;
    g_Font16AtlasImg.v        = 128;
    g_Font16AtlasImg.clutX    = 816;
    g_Font16AtlasImg.clutY    = 255;

    s_EurAtlasImported = 1;
    if (g_FontLayout == &s_FontLayout_USA)
        g_FontLayout = &s_FontLayout_EUR;
}

int Font_EurAtlasImported(void)
{
    return s_EurAtlasImported;
}

void Font_UseRussianLayout(void)
{
    if (g_FontLayout == &s_FontLayout_EUR || g_FontLayout == &s_FontLayout_USA)
    {
        g_FontLayout          = &s_FontLayout_EUR_RU;
        s_RussianLayoutActive = 1;
    }
}

/* Replace the atlas with the Cyrillic one, cell for cell. */
static void FontPatchRussianGlyphs(void* pixels, int widthWords, int height)
{
    unsigned char* p      = (unsigned char*)pixels;
    int            stride = widthWords * 2;
    int            cell;

    if (pixels == NULL || (widthWords * 4) < (ATLAS_COLS * CELL_W) || height < (6 * CELL_H))
    {
        SH_WARN("[FONT] Cyrillic atlas skipped: FONT16 is %dx%d, need %dx%d",
                widthWords * 4, height, ATLAS_COLS * CELL_W, 6 * CELL_H);
        return;
    }

    for (cell = 0; cell < (int)(sizeof(FONT_RU_ATLAS) / sizeof(FONT_RU_ATLAS[0])); cell++)
    {
        const unsigned char* src  = FONT_RU_ATLAS[cell];
        int                  dstX = (cell % ATLAS_COLS) * CELL_W;
        int                  dstY = (cell / ATLAS_COLS) * CELL_H;
        int                  x, y;

        for (y = 0; y < CELL_H; y++)
        {
            for (x = 0; x < CELL_W; x++)
            {
                unsigned char b = src[(y * CELL_W + x) >> 1];

                PixSet(p, stride, dstX + x, dstY + y, (x & 1) ? (b >> 4) : (b & 0x0F));
            }
        }
    }

    SH_LOG("[FONT] Cyrillic atlas built into FONT16 (%d cells)",
           (int)(sizeof(FONT_RU_ATLAS) / sizeof(FONT_RU_ATLAS[0])));
}

/* The FONT16 upload site calls this for whichever pack is active: the hook
 * fires for any pack at all, so it cannot assume Polish. */
void Font_PatchPackGlyphs(void* pixels, int widthWords, int height)
{
    if (s_RussianLayoutActive)
    {
        FontPatchRussianGlyphs(pixels, widthWords, height);
        return;
    }
    if (s_PtLayoutActive)
    {
        Font_PatchPortugueseGlyphs(pixels, widthWords, height);
        return;
    }
    if (s_PolishLayoutActive)
    {
        Font_PatchPolishGlyphs(pixels, widthWords, height);
        return;
    }

    /* No pack, but a Russian-patched disc whose atlas puts Э on the byte this
     * engine reads as '~'. Copy that glyph to cell 90 -- a duplicate the
     * charset never addresses -- so the encoder can write it as 0x81 instead.
     * Without this the port's own Russian menu rows lose every capital Э
     * ("ЭЛТ", "Эйсес") and eat the letter after it. */
    if (Pc_RuDiscNeedsEFix() && pixels != NULL &&
        (widthWords * 4) >= (ATLAS_COLS * CELL_W) && height >= (6 * CELL_H))
    {
        unsigned char* p      = (unsigned char*)pixels;
        int            stride = widthWords * 2;
        int            sx     = (RU_CELL_E_OBERT % ATLAS_COLS) * CELL_W;
        int            sy     = (RU_CELL_E_OBERT / ATLAS_COLS) * CELL_H;
        int            dx     = (RU_CELL_E_ALT % ATLAS_COLS) * CELL_W;
        int            dy     = (RU_CELL_E_ALT / ATLAS_COLS) * CELL_H;
        int            x, y;

        for (y = 0; y < CELL_H; y++)
            for (x = 0; x < CELL_W; x++)
                PixSet(p, stride, dx + x, dy + y, PixGet(p, stride, sx + x, sy + y));
    }
}

/* Back to the region's pristine base layout. Pc_LangInit is re-entrant (the
 * options menu re-runs it on every language step) and each run has to derive
 * fan-patch override vs Polish layout from scratch: EurFanFontInit judges the
 * disc table against the CURRENT layout's glyph count, and Font_UsePolishLayout
 * only installs over the untouched EUR base. Left as they were, stepping off
 * Polish (126 cells) made EurFanFontInit install a 120-cell override, and
 * stepping back onto Polish then refused the Polish layout -- its letters drew
 * with zero advance and folded into the next glyph until a restart. */
void Font_ResetLayout(void)
{
    s_PolishLayoutActive = 0;
    s_RussianLayoutActive = 0;
    s_PtLayoutActive = 0;
    s_EurAtlasImported = 0;
    g_FontLayout = (g_GameRegion == Region_EUR) ? &s_FontLayout_EUR : &s_FontLayout_USA;
}

void Font_ApplyRegionPatches(void)
{
    s_PolishLayoutActive = 0;
    s_RussianLayoutActive = 0;

    if (g_GameRegion != Region_EUR)
    {
        return;
    }

    g_FontLayout = &s_FontLayout_EUR;

    /* Retail PAL lays out ten message lines, not nine (see font_region.h). */
    g_PcMapMsgLineMax = 10;

    /* PAL FONT16.TIM is a 21x6 grid that cannot sit at the US strip home
     * (0,496): retail SLES places it at (768,128) in tpage 12, CLUT (816,255). */
    g_Font16AtlasImg.tPage[0] = 0;
    g_Font16AtlasImg.tPage[1] = 12;
    g_Font16AtlasImg.u        = 0;
    g_Font16AtlasImg.v        = 128;
    g_Font16AtlasImg.clutX    = 816;
    g_Font16AtlasImg.clutY    = 255;

    Gfx_StringLightGreyColorPatch(64, 64, 64);

    /* PAL TITLE_E.TIM is a 4bpp 320x96 logo+copyright block (the US one is a
     * full 8bpp 320x480 title picture) — retail SLES loads it tpage-aligned
     * at (896,0) and composes the title as black + logo + fog. The 16-entry
     * CLUT keeps the US title-CLUT home (224,15). The matching draw is
     * Pc_TitleLogoDrawEur() in title.c. */
    Pc_TitleStyleApplyDesc();

    /* Exterior tree/branch billboards (Gfx_BillboardDraw) sample BG_ETC
     * texels (0..63,128..191) — on PAL that band is resliced to
     * (128..191,0..63) and its old home is the FONT16 atlas. Move the UV
     * table (same reslice transform as the particle sprite band). */
    {
        int i;
        for (i = 0; i < 3; i++)
        {
            D_800AE4DC[i].field_8 += 128; /* u  0   -> 128 */
            D_800AE4DC[i].field_A += 128; /* u  63  -> 191 */
            D_800AE4DC[i].field_9 -= 128; /* v  128 -> 0   */
            D_800AE4DC[i].field_B -= 128; /* v  191 -> 63  */
        }
    }

    /* PAL item-model packs (IT_00x.TMD / UNQxx.TMD) bake their palette ids at
     * the EUR retail CLUT homes — retail SLES moved this whole desc family to
     * the bottom-right VRAM block because PAL's taller 256-line framebuffers
     * cover the US homes. The PAL TMDs are NOT byte-identical to US: every
     * textured prim's clut word is the US id + the home delta. Upload the
     * item palettes where those prims point or every inventory/pickup preview
     * samples an empty palette (renders black). Values byte-verified against
     * the decrypted EUR BODYPROG desc cluster. */
    {
        extern s_FsImageDesc g_InventoryKeyItemTextureImg; /* TIM01..06, per-map key items */
        extern s_FsImageDesc g_FirstAidKitItemTextureImg;  /* TIM00, common items */
        extern s_FsImageDesc D_800A9074;                   /* TIM07, always-loaded pack */
        extern s_FsImageDesc D_800A907C;                   /* FOOK, map5_s01 meat hook */

        g_InventoryKeyItemTextureImg.clutX = 912; g_InventoryKeyItemTextureImg.clutY = 480;
        g_FirstAidKitItemTextureImg.clutX  = 928; g_FirstAidKitItemTextureImg.clutY  = 480;
        D_800A9074.clutX                   = 896; D_800A9074.clutY                   = 480;
        D_800A907C.clutX                   = 896; D_800A907C.clutY                   = 488;
        /* Do NOT retarget D_800A9084 (BLD, combat-blood texture) into this family.
         * It looks like an item CLUT but its ONLY consumer is the blood-particle
         * emit (func_80060044/func_800611C0/func_80062708 in bodyprog_8005E0DC.c),
         * which HARD-CODES the CLUT word to VRAM X=304 (low-6 field 0x13) and never
         * reads the desc. Moving the upload to (944,480) — as 534b12d6b did — left the
         * emit sampling an empty (304,row) CLUT on EUR, so all spray/pool/cloud blood
         * rendered invisible (subtract/add of zero = nothing). (304,0..15) is safe on
         * the port (framebuffers start at y=32), so its US home is correct for EUR too.
         * Unlike the item descs above, whose PAL TMD data BAKES the (896..928,480) homes. */
    }

    SH_LOG("[FONT] EUR layout installed: FONT16 -> (768,128) tpage 12, clut (816,255); item CLUTs -> (896..928,480)");
}

/* Re-read FONT16 and upload it NOW, rather than queueing it.
 *
 * A language switch changes the layout (the glyph widths, and for Russian the
 * whole atlas) the instant it happens, but the queued re-read lands a frame or
 * more later -- so for those frames the old pixels are drawn at the new
 * language's advances, which is the "delayed change" a Russian switch showed.
 * Doing the read and the LoadImage inline keeps the two in step. */
static unsigned char* FontReadFile(const char* path, unsigned int* outSize)
{
    FILE*          f = fopen(path, "rb");
    long           n;
    unsigned char* buf;

    if (f == NULL)
    {
        SH_WARN("[FONT] %s is missing - the PAL atlas cannot be imported", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (n > 0 && n < (1 << 20)) ? (unsigned char*)malloc((size_t)n) : NULL;
    if (buf == NULL || fread(buf, 1, (size_t)n, f) != (size_t)n)
    {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *outSize = (unsigned int)n;
    return buf;
}

void Font_AtlasReloadNow(void)
{
    const s_FileInfo*   info = &g_FileTable[FILE_1ST_FONT16_TIM];
    unsigned int        size = (unsigned int)info->blockCount << 8;
    unsigned char*      raw;
    TIM_IMAGE           tim;
    RECT                rect;

    if (size == 0)
        return;

    raw = s_EurAtlasImported ? FontReadFile(EUR_ATLAS_PATH, &size)
                             : Pc_LangReadDiscFile(info->startSector, size);
    if (raw == NULL)
    {
        SH_WARN("[FONT] could not re-read FONT16 - the atlas keeps the old language's glyphs");
        return;
    }

    OpenTIM((u_long*)raw);
    if (ReadTIM(&tim) != NULL)
    {
        Font_PatchPackGlyphs(tim.paddr, tim.prect->w, tim.prect->h);

        rect   = *tim.prect;
        rect.x = g_Font16AtlasImg.u + ((g_Font16AtlasImg.tPage[1] & 0xF) << 6);
        rect.y = g_Font16AtlasImg.v + ((g_Font16AtlasImg.tPage[1] << 4) & 0x100);
        LoadImage(&rect, (u_long*)tim.paddr);

        if (tim.caddr != NULL)
        {
            rect   = *tim.crect;
            rect.x = g_Font16AtlasImg.clutX;
            rect.y = g_Font16AtlasImg.clutY;
            LoadImage(&rect, (u_long*)tim.caddr);
        }
        DrawSync(0);
    }
    free(raw);
}

/* Re-lay a FONT16 atlas into whatever shape the active layout expects.
 *
 * A font pack replaces 1ST/FONT16.TIM wholesale, but the two regions store the
 * same glyphs in different shapes: the US atlas is one strip 256 VRAM units
 * wide and 16 rows tall, holding four pages of 21 glyphs; the PAL one is a
 * 64x96 grid of 21 glyphs per row. The upload DESTINATION comes from the image
 * descriptor rather than from the file, so a pack built for the other region
 * is written at the wrong shape and smears across VRAM. That is what "the font
 * pack has to match your disc" has always meant.
 *
 * Nothing is lost in between. Cell N is cell N in both -- only its position
 * differs -- so a pack can simply be re-laid-out. Only the 84 cells the US
 * strip can hold are moved; converting TO the PAL grid therefore writes just
 * its first four rows, leaving rows 4 and 5 (the accents, which a US pack has
 * no opinion about) as they were in VRAM.
 *
 * A pack already in the right shape is left completely alone, so no existing
 * pack changes behaviour. Returns non-zero when it converted. */
#define FONT_CELL_UNITS 3                      /* 12px at 4bpp */
#define FONT_CELL_BYTES (FONT_CELL_UNITS * 2)
#define FONT_US_UNITS   256
#define FONT_EUR_UNITS  64
#define FONT_COLS       21

static unsigned char s_AtlasFit[FONT_EUR_UNITS * 2 * 64];

int Font_FitAtlasToLayout(void** pixels, int* w, int* h)
{
    const unsigned char* src = (const unsigned char*)*pixels;
    int  wantEur = (g_FontLayout->rowsPerPage != 1);
    int  isUs    = (*w == FONT_US_UNITS && *h == 16);
    int  isEur   = (*w == FONT_EUR_UNITS && *h >= 64);
    int  c;

    if (src == NULL || (isUs == isEur))
        return 0; /* not a shape this knows; leave it be */
    if (isUs == !wantEur)
        return 0; /* already what the layout wants -- the common case */

    for (c = 0; c < FONT_COLS * 4; c++)
    {
        int  col = c % FONT_COLS;
        int  row = c / FONT_COLS;
        int  r;

        for (r = 0; r < 16; r++)
        {
            const unsigned char* from;
            unsigned char*       to;

            if (isUs) /* strip -> grid */
            {
                from = src + (size_t)r * FONT_US_UNITS * 2
                     + (size_t)(row * 64 + col * FONT_CELL_UNITS) * 2;
                to   = s_AtlasFit + (size_t)(row * 16 + r) * FONT_EUR_UNITS * 2
                     + (size_t)col * FONT_CELL_UNITS * 2;
            }
            else /* grid -> strip */
            {
                from = src + (size_t)(row * 16 + r) * FONT_EUR_UNITS * 2
                     + (size_t)col * FONT_CELL_UNITS * 2;
                to   = s_AtlasFit + (size_t)r * FONT_US_UNITS * 2
                     + (size_t)(row * 64 + col * FONT_CELL_UNITS) * 2;
            }
            memcpy(to, from, FONT_CELL_BYTES);
        }
    }

    *pixels = s_AtlasFit;
    *w      = wantEur ? FONT_EUR_UNITS : FONT_US_UNITS;
    *h      = wantEur ? 64 : 16;
    SH_LOG("[FONT] font pack re-laid from the %s atlas shape to the %s one",
           isUs ? "US" : "PAL", wantEur ? "PAL" : "US");
    return 1;
}

/* The same re-layout for a hi-res font pack, which is an RGBA image rather
 * than a 4bpp atlas.
 *
 * A pack ships load/1ST/FONT16.png at some whole multiple of its region's
 * atlas: the EU one is 2560x960, exactly 10x the PAL atlas's 256x96. The
 * override maps that image onto the VRAM rect the descriptor names, so on a US
 * disc it was being stretched across the 1024x16 strip -- garbled, as reported.
 *
 * The shape is unambiguous because only one of the two divides evenly: 2560/256
 * and 960/96 are both 10, while 2560/1024 is 2.5. So the scale identifies the
 * pack's region, and the cells can be moved at that scale.
 *
 * Returns the scale on success and fills outW/outH plus a malloc'd image the
 * caller frees; 0 when the pack already matches the layout, or its shape is not
 * a whole multiple of either atlas. */
#define FONT_CELL_PX_W 12
#define FONT_CELL_PX_H 16
#define FONT_US_PX_W   1024
#define FONT_US_PX_H   16
#define FONT_EUR_PX_W  256
#define FONT_EUR_PX_H  96

static int AtlasScale(int w, int h, int baseW, int baseH)
{
    int s = w / baseW;

    if (s < 1 || w % baseW != 0 || h % baseH != 0 || h / baseH != s)
        return 0;
    return s;
}

int Font_FitHiresAtlasToLayout(const unsigned char* rgba, int w, int h,
                               unsigned char** out, int* outW, int* outH,
                               int* outUnits, int* outRows)
{
    int wantEur = (g_FontLayout->rowsPerPage != 1);
    int usScale = AtlasScale(w, h, FONT_US_PX_W, FONT_US_PX_H);
    int eurScale = AtlasScale(w, h, FONT_EUR_PX_W, FONT_EUR_PX_H);
    int scale, dstW, dstH, c;
    unsigned char* dst;

    if (rgba == NULL || (usScale != 0) == (eurScale != 0))
        return 0; /* neither shape, or ambiguous: leave it alone */
    if ((usScale != 0) == !wantEur)
        return 0; /* already the shape the layout wants */

    scale = usScale ? usScale : eurScale;
    dstW  = wantEur ? FONT_EUR_PX_W * scale : FONT_US_PX_W * scale;
    /* Only the 84 cells a US strip holds are carried, so converting TO the PAL
     * grid covers its first four rows and leaves the accent rows to VRAM. */
    dstH  = wantEur ? (FONT_CELL_PX_H * 4 * scale) : FONT_US_PX_H * scale;

    dst = (unsigned char*)calloc((size_t)dstW * dstH, 4);
    if (dst == NULL)
        return 0;

    for (c = 0; c < FONT_COLS * 4; c++)
    {
        int col = c % FONT_COLS;
        int row = c / FONT_COLS;
        int y;

        for (y = 0; y < FONT_CELL_PX_H * scale; y++)
        {
            size_t from, to;

            if (usScale) /* strip -> grid */
            {
                from = ((size_t)y * w
                        + (size_t)(row * FONT_EUR_PX_W + col * FONT_CELL_PX_W) * scale) * 4;
                to   = ((size_t)(row * FONT_CELL_PX_H * scale + y) * dstW
                        + (size_t)col * FONT_CELL_PX_W * scale) * 4;
            }
            else /* grid -> strip */
            {
                from = ((size_t)(row * FONT_CELL_PX_H * scale + y) * w
                        + (size_t)col * FONT_CELL_PX_W * scale) * 4;
                to   = ((size_t)y * dstW
                        + (size_t)(row * FONT_EUR_PX_W + col * FONT_CELL_PX_W) * scale) * 4;
            }
            memcpy(dst + to, rgba + from, (size_t)FONT_CELL_PX_W * scale * 4);
        }
    }

    *out  = dst;
    *outW = dstW;
    *outH = dstH;
    /* The NATIVE VRAM rect the image maps onto, which is the atlas's own size
     * and has nothing to do with the hi-res pixel count: 256 units x 16 rows
     * for the US strip, or the PAL grid's first four rows, 64 x 64. Passing
     * the hi-res size here instead is what made a converted pack invisible. */
    *outUnits = wantEur ? 64 : 256;
    *outRows  = wantEur ? 64 : 16;
    SH_LOG("[FONT] hi-res font pack re-laid from the %s shape to the %s one (%dx) ",
           usScale ? "US" : "PAL", wantEur ? "PAL" : "US", scale);
    return scale;
}
