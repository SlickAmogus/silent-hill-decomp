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

/* Rendered-line cap of the map-message renderer (Gfx_MapMsg_CalculateWidths /
 * Gfx_MapMsg_StringDraw). One PC binary serves every region, so retail's
 * per-region constant has to be a runtime value. 9 on NTSC-U and NTSC-J; retail
 * PAL lays messages out over TEN lines and its localizations use that tenth line
 * (the tallest single entry on the PAL disc is exactly 10). Read out of the
 * decrypted retail overlays (EUR sha1 f748528af6da66184978a08c4bcbf924a306eaba ==
 * configs/EUR/bodyprog.yaml, USA sha1 eb118537b0c3c1e5cccfa2c5d283b3119c5ec7a3 ==
 * configs/USA/bodyprog.yaml):
 *   CalculateWidths bound  EUR 0x8004ACFC 28C2000A  USA 0x8004AF04 28C20009
 *   widths[] clear         EUR 0x8004AA40 a2=9,+36  USA 0x8004ACF4 a2=8,+32
 *   StringDraw bound       EUR 0x8004A42C 241E000A (beq $s0,$fp)
 *   StringDraw {E}/{S}     EUR 0x8004A58C/0x8004A5CC 2410000A
 * Rendered at nine, a ten-line PAL page's parse ends on the line bound before it
 * reaches the page's ~E or NUL — the only three writers of the renderer's return
 * code — so Gfx_MapMsg_Draw never reaches FINISH_MAP_MSG and the document cannot
 * be dismissed at all (GitHub #85).
 * NOT applied to the positionIdx-4 box anchor (text_draw.c): that constant is
 * retail PAL's centre in a 256-scanline field, and the port renders PAL content
 * in the NTSC 240-line frame with its own g_PsxMsgVShift compensation. */
extern int g_PcMapMsgLineMax;

/* Map a text byte (already '!'/'&'-remapped by the caller) to 0..2 glyph
 * emissions. Bytes >= 0x80 resolve through the retail EUR accent scheme and
 * yield nothing on the US layout. */
int Font_MapChar(unsigned int charCode, s_GlyphEmit emits[2]);

/* Install the detected region's font layout + FONT16 VRAM descriptor and the
 * EUR STRING_COLORS[6] tint. Call once after Fs_InitFileTableForRegion. */
void Font_ApplyRegionPatches(void);

/* Polish ships as a PC-side language pack rather than on a disc, so its
 * letterforms are built into the FONT16 pixel block just before upload (all
 * reload sites covered) and selected by a widened layout. Both are no-ops
 * unless the Polish pack is the active language. */
void Font_PatchPolishGlyphs(void* pixels, int widthWords, int height);
void Font_UsePolishLayout(void);

/* Replace the active layout's kerning table (fan-translation discs retune the
 * BODYPROG widths to match their repainted FONT16 glyphs). Copies the first
 * glyphCount entries; call after Font_ApplyRegionPatches. */
void Font_SetGlyphWidths(const unsigned char* widths);

/* Implemented in text_draw.c (the color table is file-local there): replaces
 * StringColorId_LightGrey — retail EUR dims it (100,100,100)->(64,64,64). */
void Gfx_StringLightGreyColorPatch(unsigned char r, unsigned char g, unsigned char b);

#endif
