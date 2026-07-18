#ifndef PC_KANJI_H
#define PC_KANJI_H

/* NTSC-J kanji rendering. The retail JP game rasterizes SJIS glyphs from the
 * PSX BIOS kanji ROM into VRAM per message line; the PC port replaces the ROM
 * with an embedded Shinonome JIS X 0208 bitmap font (public domain) and
 * rasterizes glyphs on demand into 12x16 4bpp atlas cells living in the
 * framebuffer margin strips (rows 16..31 and 480..495, x < 320 halfwords) —
 * the same VRAM band retail used, outside both display rects and the PC
 * framebuffer store/restore paths. */

/* True for an SJIS lead byte (0x81-0x9F / 0xE0-0xEF). */
int Pc_KanjiIsLead(unsigned char c);

/* Atlas cell for an SJIS code, rasterizing it into VRAM on first use.
 * Fills the DR_TPAGE page value, the within-page pixel u/v of the 12x16
 * cell, and the packed CLUT id. Returns 0 when the font has no such glyph
 * (caller advances without drawing). */
int Pc_KanjiCell(unsigned short sjis, unsigned int* outPage, int* outU, int* outV,
                 unsigned short* outClut);

/* Drop every cached cell and re-upload the atlas CLUT. Call at map load —
 * cheap, and guards against anything having scribbled the margin strips. */
void Pc_KanjiAtlasReset(void);

#endif /* PC_KANJI_H */
