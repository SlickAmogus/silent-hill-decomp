#ifndef FONT_REGION_H
#define FONT_REGION_H

/* Region-aware FONT16 layout (versions are data, no #ifdef PAL in game code).
 *
 * US disc: 84 glyphs as a 1024x16-texel strip at VRAM (0,496); the renderer
 * treats it as one 21-glyph row per texture page (tpages 16-19, v=240) with
 * CLUT (304,511).
 * PAL disc (SLES-01514): 120 glyphs (cells 0-83 byte-identical to US, cells
 * 84-119 accents) as a 21x6 grid in tpage 12 at VRAM (768,128), CLUT
 * (816,255). All values recovered from the decrypted EUR BODYPROG — see
 * pc_port/tools/decrypt_eur_overlay.py.
 *
 * All draw-site math in text_draw.c routes through g_FontLayout; with the USA
 * layout installed the computed prim words are bit-identical to the original
 * hardcoded constants, so US rendering is unchanged. */

typedef struct {
    int                  glyphCount;  /* valid atlas cells (84 US, 120 EUR) */
    int                  vBase;       /* atlas v of grid row 0 (240 US, 128 EUR) */
    int                  rowsPerPage; /* grid rows per tpage (1 US: row index selects the tpage; 6 EUR: single page) */
    unsigned int         tpageBase;   /* 0x10 US (tpages 16-19), 0x0C EUR */
    unsigned int         packedClut;  /* getClut(): 0x7FD3 US (304,511), 0x3FF3 EUR (816,255) */
    int                  hiResGlyphBottom; /* hi-res FT4 quad bottom offset: (posY*2)+30 US, +31 retail EUR */
    const unsigned char* glyphWidths; /* kerning table, glyphCount entries */
} s_FontLayout;

/* One glyph emission. PAL uppercase accents draw as a zero-advance combining
 * mark (cell 114 diaeresis / 119 acute) 3px above the line, then the base
 * letter at the same X — mirroring the retail EUR renderer. */
typedef struct {
    int cell;
    int dy;
    int advance;
} s_GlyphEmit;

extern const s_FontLayout* g_FontLayout;

/* Map a text byte (already '!'/'&'-remapped by the caller) to 0..2 glyph
 * emissions. Bytes >= 0x80 resolve through the retail EUR accent scheme and
 * yield nothing on the US layout. */
int Font_MapChar(unsigned int charCode, s_GlyphEmit emits[2]);

/* Install the detected region's font layout + FONT16 VRAM descriptor and the
 * EUR STRING_COLORS[6] tint. Call once after Fs_InitFileTableForRegion. */
void Font_ApplyRegionPatches(void);

/* Implemented in text_draw.c (the color table is file-local there): replaces
 * StringColorId_LightGrey — retail EUR dims it (100,100,100)->(64,64,64). */
void Gfx_StringLightGreyColorPatch(unsigned char r, unsigned char g, unsigned char b);

#endif
