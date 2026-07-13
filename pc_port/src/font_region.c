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

static const s_FontLayout s_FontLayout_USA = { 84,  240, 1, 0x10, 0x7FD3, 30, s_GlyphWidths_USA };
static const s_FontLayout s_FontLayout_EUR = { 120, 128, 6, 0x0C, 0x3FF3, 31, s_GlyphWidths_EUR };

const s_FontLayout* g_FontLayout = &s_FontLayout_USA;

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
        return 0; /* No accent cells in the US atlas. */
    }
    else
    {
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
        extern s_FsImageDesc D_800A9084;                   /* BLD, film/blood texture */

        g_InventoryKeyItemTextureImg.clutX = 912; g_InventoryKeyItemTextureImg.clutY = 480;
        g_FirstAidKitItemTextureImg.clutX  = 928; g_FirstAidKitItemTextureImg.clutY  = 480;
        D_800A9074.clutX                   = 896; D_800A9074.clutY                   = 480;
        D_800A907C.clutX                   = 896; D_800A907C.clutY                   = 488;
        D_800A9084.clutX                   = 944; D_800A9084.clutY                   = 480;
    }

    SH_LOG("[FONT] EUR layout installed: FONT16 -> (768,128) tpage 12, clut (816,255); item CLUTs -> (896..944,480)");
}
