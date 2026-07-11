#include "pc_kanji.h"

#include <string.h>

#include "kanji_font.inc"

/* Atlas geometry. Cells are 12x16 pixels = 3 VRAM halfwords wide at 4bpp.
 * A cell never straddles a 64-halfword texture page: each page column holds
 * 21 cells (63 halfwords, 1 wasted).
 *   strip 0: y=16,  page columns 0..3 (x 0..255)  = 84 cells; (256,16) = CLUT
 *   strip 1: y=480, page columns 0..4 (x 0..319)  = 105 cells
 * Both strips sit in the framebuffer margins: display buffers are (0,32) and
 * (0,256), each 320x224 (448 interlaced from y=32), so rows 16..31 and
 * 480..495 are never displayed, never framebuffer-stored, and were used for
 * exactly this purpose by the retail JP game. */
#define CELL_W_HW        3
#define CELLS_PER_PAGE   21
#define STRIP0_CELLS     (4 * CELLS_PER_PAGE)
#define CELL_COUNT       (STRIP0_CELLS + 5 * CELLS_PER_PAGE)
#define STRIP0_Y         16
#define STRIP1_Y         480
#define CLUT_X           256
#define CLUT_Y           16
/* getClut(256,16) */
#define KANJI_CLUT_ID    ((CLUT_Y << 6) | (CLUT_X >> 4))

extern void GR_CopyVRAM(unsigned short* src, int x, int y, int w, int h, int dst_x, int dst_y);

static unsigned short s_CellSjis[CELL_COUNT];
static int            s_CellCount = 0;
static int            s_ClutUploaded = 0;

int Pc_KanjiIsLead(unsigned char c)
{
    return (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xEF);
}

static int SjisToKuTen(unsigned short sjis, int* ku, int* ten)
{
    int s1 = sjis >> 8;
    int s2 = sjis & 0xFF;
    int k, t;

    if (!Pc_KanjiIsLead((unsigned char)s1) || s2 < 0x40 || s2 > 0xFC || s2 == 0x7F)
        return 0;

    k = (s1 - ((s1 <= 0x9F) ? 0x81 : 0xC1)) * 2 + 1;
    if (s2 >= 0x9F)
    {
        k += 1;
        t  = s2 - 0x9F + 1;
    }
    else
    {
        t = s2 - 0x40 + 1 - ((s2 > 0x7F) ? 1 : 0);
    }

    *ku  = k;
    *ten = t;
    return k >= 1 && k <= 94 && t >= 1 && t <= 94;
}

static const unsigned char* GlyphBits(unsigned short sjis)
{
    int          ku, ten;
    unsigned int g;

    if (!SjisToKuTen(sjis, &ku, &ten))
        return 0;

    g = KANJI_FONT_IDX[(ku - 1) * 94 + (ten - 1)];
    if (g == 0xFFFF)
        return 0;

    return &KANJI_FONT_BITS[g * 32];
}

static void CellCoords(int cell, int* vramX, int* vramY, unsigned int* page, int* u, int* v)
{
    int pageCol, inPage, stripY;

    if (cell < STRIP0_CELLS)
    {
        stripY  = STRIP0_Y;
        pageCol = cell / CELLS_PER_PAGE;
        inPage  = cell % CELLS_PER_PAGE;
        *page   = (unsigned int)pageCol; /* y<256: bit4 clear, 4bpp */
    }
    else
    {
        stripY  = STRIP1_Y;
        pageCol = (cell - STRIP0_CELLS) / CELLS_PER_PAGE;
        inPage  = (cell - STRIP0_CELLS) % CELLS_PER_PAGE;
        *page   = 0x10u | (unsigned int)pageCol; /* y>=256 bank */
    }

    *vramX = pageCol * 64 + inPage * CELL_W_HW;
    *vramY = stripY;
    *u     = inPage * CELL_W_HW * 4; /* 4bpp pixels within the page */
    *v     = stripY & 0xFF;          /* 16, or 480-256=224 */
}

static void UploadClut(void)
{
    unsigned short clut[16];
    int            i;

    clut[0] = 0x0000; /* index 0 = transparent */
    for (i = 1; i < 16; i++)
        clut[i] = 0x7FFF; /* opaque white; prims tint via RGB modulation */

    GR_CopyVRAM(clut, 0, 0, 16, 1, CLUT_X, CLUT_Y);
    s_ClutUploaded = 1;
}

/* Rasterize the 16x16 1bpp glyph into a 12x16 4bpp cell. The 16->12 fold ORs
 * neighbouring source columns (every third output column merges two) so
 * 1px-wide kanji strokes survive — dropping columns instead erases them.
 * Matches the narrow look of the retail JP renderer (12px advance). */
static void RasterizeCell(const unsigned char* bits, int vramX, int vramY)
{
    unsigned short buf[CELL_W_HW * 16];
    int            row, j;

    memset(buf, 0, sizeof(buf));

    for (row = 0; row < 16; row++)
    {
        unsigned int src = ((unsigned int)bits[row * 2] << 8) | bits[row * 2 + 1];

        for (j = 0; j < 12; j++)
        {
            int b0 = (j * 16) / 12;
            int b1 = ((j + 1) * 16) / 12;
            int on = 0;
            int s;

            for (s = b0; s < b1; s++)
                on |= (src >> (15 - s)) & 1;

            if (on)
                buf[row * CELL_W_HW + (j >> 2)] |= (unsigned short)(1u << ((j & 3) * 4));
        }
    }

    GR_CopyVRAM(buf, 0, 0, CELL_W_HW, 16, vramX, vramY);
}

int Pc_KanjiCell(unsigned short sjis, unsigned int* outPage, int* outU, int* outV,
                 unsigned short* outClut)
{
    const unsigned char* bits;
    int                  cell, vramX, vramY;
    int                  i;

    if (!s_ClutUploaded)
        UploadClut();

    for (i = 0; i < s_CellCount; i++)
    {
        if (s_CellSjis[i] == sjis)
        {
            CellCoords(i, &vramX, &vramY, outPage, outU, outV);
            *outClut = KANJI_CLUT_ID;
            return 1;
        }
    }

    bits = GlyphBits(sjis);
    if (bits == 0)
        return 0;

    if (s_CellCount >= CELL_COUNT)
    {
        /* Atlas full: drop everything. Worst-case a message re-rasterizes its
         * glyphs next frame; a single screen never needs all 189 cells. */
        s_CellCount = 0;
    }

    cell               = s_CellCount++;
    s_CellSjis[cell]   = sjis;
    CellCoords(cell, &vramX, &vramY, outPage, outU, outV);
    RasterizeCell(bits, vramX, vramY);
    *outClut = KANJI_CLUT_ID;
    return 1;
}

void Pc_KanjiAtlasReset(void)
{
    s_CellCount = 0;
    UploadClut();
}
