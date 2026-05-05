#include "hires_override.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <PsyX/common/glad.h>

#define MAX_HIRES_OVERRIDES 256

typedef struct {
    int      vramX, vramY;       /* 16-bit cells in PSX VRAM (match key) */
    int      vramW, vramH;
    int      clutX, clutY;       /* CLUT cell coords, -1 if no CLUT (16bpp) */
    int      originalBitDepth;   /* 4, 8, 16 */
    GLuint   glTexture;
    int      hiresW, hiresH;     /* actual pixel dimensions of decoded RGBA */
    int      sourceBitDepth;     /* TIM mode of the loose file itself */
} HiresEntry;

static HiresEntry g_entries[MAX_HIRES_OVERRIDES];
static int        g_numEntries = 0;
static int        g_initialized = 0;

void HiresOverride_Init(void)
{
    if (g_initialized) return;
    g_numEntries = 0;
    g_initialized = 1;
    fprintf(stderr, "[HIRES] override system initialized (capacity=%d)\n",
            MAX_HIRES_OVERRIDES);
}

/* TIM PSX-565 -> RGBA8 expansion (BGR555 with STP bit). */
static void bgr555_to_rgba(unsigned short cx, unsigned char* out)
{
    int r5 = cx & 0x1F;
    int g5 = (cx >> 5) & 0x1F;
    int b5 = (cx >> 10) & 0x1F;
    int stp = (cx >> 15) & 1;
    /* Standard 5-to-8 bit expansion: replicate top 3 bits into low bits. */
    out[0] = (unsigned char)((r5 << 3) | (r5 >> 2));
    out[1] = (unsigned char)((g5 << 3) | (g5 >> 2));
    out[2] = (unsigned char)((b5 << 3) | (b5 >> 2));
    /* PSX semi-transparency: cx==0 means transparent, otherwise opaque
     * (STP affects blending, not whether the pixel is drawn). */
    out[3] = (cx == 0) ? 0 : 255;
    (void)stp;
}

/* Parse a TIM file. On success: sets *outRGBA to a malloc'd RGBA8 buffer,
 * fills *outW, *outH with pixel dimensions, and *outBpp with the source
 * TIM's bit depth (4, 8, 16, or 24). Caller must free *outRGBA.
 * Returns 0 on success, -1 on parse failure. */
static int parse_tim_to_rgba(const unsigned char* data, unsigned int size,
                             unsigned char** outRGBA, int* outW, int* outH,
                             int* outBpp)
{
    if (size < 12) return -1;
    /* TIM header: 4-byte magic (0x00000010) + 4-byte mode flags. */
    if (data[0] != 0x10 || data[1] != 0x00 || data[2] != 0x00 || data[3] != 0x00)
        return -1;

    unsigned int flags = (unsigned int)data[4]
                       | ((unsigned int)data[5] << 8)
                       | ((unsigned int)data[6] << 16)
                       | ((unsigned int)data[7] << 24);
    int bppCode  = (int)(flags & 0x7);   /* 0=4, 1=8, 2=16, 3=24 */
    int hasClut  = (int)((flags >> 3) & 1);
    int srcBpp;
    switch (bppCode) {
        case 0: srcBpp = 4;  break;
        case 1: srcBpp = 8;  break;
        case 2: srcBpp = 16; break;
        case 3: srcBpp = 24; break;
        default: return -1;
    }
    *outBpp = srcBpp;

    const unsigned char* p = data + 8;
    const unsigned char* end = data + size;

    /* Optional CLUT block (always 16bpp BGR555 entries). */
    const unsigned char* clutEntries = NULL;
    int                  clutW = 0, clutH = 0;
    if (hasClut)
    {
        if (p + 12 > end) return -1;
        unsigned int blockLen = (unsigned int)p[0]
                              | ((unsigned int)p[1] << 8)
                              | ((unsigned int)p[2] << 16)
                              | ((unsigned int)p[3] << 24);
        clutW = (int)((unsigned int)p[8] | ((unsigned int)p[9] << 8));
        clutH = (int)((unsigned int)p[10] | ((unsigned int)p[11] << 8));
        clutEntries = p + 12;
        if (p + blockLen > end || blockLen < 12) return -1;
        p += blockLen;
    }

    /* Pixel block. */
    if (p + 12 > end) return -1;
    /* unsigned int pixBlockLen = (unsigned int)p[0] | ... ; (unused) */
    int pixCellW = (int)((unsigned int)p[8] | ((unsigned int)p[9] << 8));
    int pixH     = (int)((unsigned int)p[10] | ((unsigned int)p[11] << 8));
    const unsigned char* pix = p + 12;

    /* Cell width is in 16-bit units. Convert to pixel width per bit depth. */
    int pixW;
    switch (srcBpp) {
        case 4:  pixW = pixCellW * 4;            break;
        case 8:  pixW = pixCellW * 2;            break;
        case 16: pixW = pixCellW;                break;
        case 24: pixW = (pixCellW * 2) / 3;      break;
        default: return -1;
    }
    if (pixW <= 0 || pixH <= 0 || pixW > 8192 || pixH > 8192) return -1;

    *outW = pixW;
    *outH = pixH;

    unsigned char* rgba = (unsigned char*)malloc((size_t)pixW * (size_t)pixH * 4);
    if (!rgba) return -1;

    if (srcBpp == 4 || srcBpp == 8)
    {
        if (!clutEntries) { free(rgba); return -1; }
        int rowCells = pixCellW;
        int rowBytes = rowCells * 2;
        int totalIndices = clutW * clutH;
        for (int y = 0; y < pixH; y++)
        {
            const unsigned char* row = pix + (size_t)y * (size_t)rowBytes;
            for (int x = 0; x < pixW; x++)
            {
                unsigned int idx;
                if (srcBpp == 4)
                {
                    int byteIdx = x >> 1;
                    if (byteIdx >= rowBytes) { idx = 0; }
                    else
                    {
                        unsigned char b = row[byteIdx];
                        idx = (x & 1) ? (unsigned int)((b >> 4) & 0xF)
                                       : (unsigned int)(b & 0xF);
                    }
                }
                else /* 8bpp */
                {
                    if (x >= rowBytes) { idx = 0; }
                    else { idx = row[x]; }
                }
                if ((int)idx >= totalIndices) idx = 0;
                unsigned short cx = (unsigned short)clutEntries[idx * 2]
                                  | ((unsigned short)clutEntries[idx * 2 + 1] << 8);
                bgr555_to_rgba(cx, &rgba[(size_t)(y * pixW + x) * 4]);
            }
        }
    }
    else if (srcBpp == 16)
    {
        for (int y = 0; y < pixH; y++)
        {
            const unsigned char* row = pix + (size_t)y * (size_t)pixCellW * 2;
            for (int x = 0; x < pixW; x++)
            {
                unsigned short cx = (unsigned short)row[x * 2]
                                  | ((unsigned short)row[x * 2 + 1] << 8);
                bgr555_to_rgba(cx, &rgba[(size_t)(y * pixW + x) * 4]);
            }
        }
    }
    else /* 24bpp */
    {
        /* 24-bit: 3 bytes/pixel, packed BGR. Width-cell math means
         * pixel data is 3 bytes per pixel, row stride = pixCellW * 2. */
        for (int y = 0; y < pixH; y++)
        {
            const unsigned char* row = pix + (size_t)y * (size_t)pixCellW * 2;
            for (int x = 0; x < pixW; x++)
            {
                unsigned char* o = &rgba[(size_t)(y * pixW + x) * 4];
                /* Most TIM 24bpp tools store as B,G,R; treat as that. */
                o[0] = row[x * 3 + 0];
                o[1] = row[x * 3 + 1];
                o[2] = row[x * 3 + 2];
                o[3] = 255;
            }
        }
    }

    *outRGBA = rgba;
    return 0;
}

int HiresOverride_RegisterFromTim(const char* timPath,
                                   const unsigned char* timData,
                                   unsigned int timSize,
                                   int targetVramX, int targetVramY,
                                   int targetVramW, int targetVramH,
                                   int targetClutX, int targetClutY,
                                   int originalBitDepth)
{
    if (!g_initialized) HiresOverride_Init();
    if (g_numEntries >= MAX_HIRES_OVERRIDES)
    {
        fprintf(stderr, "[HIRES] table full, ignoring %s\n", timPath);
        return -1;
    }

    unsigned char* rgba = NULL;
    int hiW = 0, hiH = 0, srcBpp = 0;
    if (parse_tim_to_rgba(timData, timSize, &rgba, &hiW, &hiH, &srcBpp) != 0)
    {
        fprintf(stderr, "[HIRES] failed to parse TIM %s (size=%u)\n",
                timPath, timSize);
        return -1;
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    if (tex == 0) { free(rgba); return -1; }
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, hiW, hiH, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    free(rgba);

    HiresEntry* e = &g_entries[g_numEntries++];
    e->vramX = targetVramX;
    e->vramY = targetVramY;
    e->vramW = targetVramW;
    e->vramH = targetVramH;
    e->clutX = targetClutX;
    e->clutY = targetClutY;
    e->originalBitDepth = originalBitDepth;
    e->glTexture = tex;
    e->hiresW = hiW;
    e->hiresH = hiH;
    e->sourceBitDepth = srcBpp;

    fprintf(stderr,
            "[HIRES] registered %s: vram=(%d,%d %dx%d cells, %dbpp) "
            "clut=(%d,%d) hires=%dx%d (src %dbpp) tex=%u\n",
            timPath, targetVramX, targetVramY, targetVramW, targetVramH,
            originalBitDepth, targetClutX, targetClutY,
            hiW, hiH, srcBpp, (unsigned)tex);
    return 0;
}

unsigned int HiresOverride_LookupByTpageClut(int tpage, int clut,
                                              int* outNativePixelW,
                                              int* outNativePixelH)
{
    if (g_numEntries == 0) return 0;

    int tpx = (tpage & 0xF) * 64;
    int tpy = ((tpage >> 4) & 1) * 256;
    int tpFmt = (tpage >> 7) & 0x3;
    int bd = (tpFmt == 0) ? 4 : (tpFmt == 1) ? 8 : (tpFmt == 2) ? 16 : 0;
    int cx = (clut & 0x3F) * 16;
    int cy = (clut >> 6) & 0x1FF;

    for (int i = 0; i < g_numEntries; i++)
    {
        HiresEntry* e = &g_entries[i];
        if (e->originalBitDepth != bd) continue;
        /* Tpage origin must lie inside the override's vram region. */
        if (tpx < e->vramX || tpx >= e->vramX + e->vramW) continue;
        if (tpy < e->vramY || tpy >= e->vramY + e->vramH) continue;
        if (e->clutX >= 0)
        {
            if (cx != e->clutX || cy != e->clutY) continue;
        }
        if (outNativePixelW || outNativePixelH)
        {
            int pixelW;
            switch (bd) {
                case 4:  pixelW = e->vramW * 4; break;
                case 8:  pixelW = e->vramW * 2; break;
                default: pixelW = e->vramW;     break;
            }
            if (outNativePixelW) *outNativePixelW = pixelW;
            if (outNativePixelH) *outNativePixelH = e->vramH;
        }
        return (unsigned int)e->glTexture;
    }
    return 0;
}

void HiresOverride_LogStats(void)
{
    fprintf(stderr, "[HIRES] %d active overrides\n", g_numEntries);
    for (int i = 0; i < g_numEntries; i++)
    {
        HiresEntry* e = &g_entries[i];
        fprintf(stderr, "  [%d] vram=(%d,%d %dx%d %dbpp) clut=(%d,%d) hires=%dx%d tex=%u\n",
                i, e->vramX, e->vramY, e->vramW, e->vramH,
                e->originalBitDepth, e->clutX, e->clutY,
                e->hiresW, e->hiresH, (unsigned)e->glTexture);
    }
}
