#include "font_region.h"

#include <string.h> /* memcpy */

#include "game.h"
#include "bodyprog/bodyprog.h"           /* g_Font16AtlasImg */
#include "bodyprog/text/text_draw.h"     /* GLYPH_TABLE_ASCII_OFFSET, FONT_12X16_* */
#include "main/fileinfo.h"               /* g_GameRegion */
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

static const s_FontLayout s_FontLayout_USA    = { 84,  240, 1, 0x10, 0x7FD3, 30, s_GlyphWidths_USA };
static const s_FontLayout s_FontLayout_EUR    = { 120, 128, 6, 0x0C, 0x3FF3, 31, s_GlyphWidths_EUR };
static const s_FontLayout s_FontLayout_EUR_PL = { 126, 128, 6, 0x0C, 0x3FF3, 31, s_GlyphWidths_EUR_PL };

const s_FontLayout* g_FontLayout = &s_FontLayout_USA;

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
 * full-width 'á'). Swap in the disc's widths, everything else unchanged. */
void Font_SetGlyphWidths(const unsigned char* widths)
{
    static s_FontLayout  s_overrideLayout;
    static unsigned char s_overrideWidths[120];
    int                  count = g_FontLayout->glyphCount;

    memcpy(s_overrideWidths, widths, (count <= 120) ? count : 120);
    s_overrideLayout             = *g_FontLayout;
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
            default: return 0;
        }
        cell -= GLYPH_TABLE_ASCII_OFFSET;
    }
    else
    {
        /* Polish, when its pack is active. Ahead of the retail scheme because
         * these bytes would otherwise fall into the 0x80-0xBF arithmetic and
         * be dropped. */
        if (layout->glyphCount > 120 && charCode >= 0xA2 && charCode <= 0xB3)
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
    if (g_FontLayout == &s_FontLayout_EUR)
    {
        g_FontLayout = &s_FontLayout_EUR_PL;
    }
}

void Font_ApplyRegionPatches(void)
{
    if (g_GameRegion != Region_EUR)
    {
        return;
    }

    g_FontLayout = &s_FontLayout_EUR;

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
    g_TitleImg.tPage[0] = 0;
    g_TitleImg.tPage[1] = 14;
    g_TitleImg.u        = 0;
    g_TitleImg.v        = 0;
    g_TitleImg.clutX    = 224;
    g_TitleImg.clutY    = 15;

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
