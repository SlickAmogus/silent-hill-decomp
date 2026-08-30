/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * ghost_art_dump.c - render the online client's generated textures to PNG.
 *
 * The ghost silhouette and the marker sigil are rasterized from a distance
 * field at runtime (sh_net_art.c), which means nobody can see them without
 * launching the game, connecting to a server and finding another player. This
 * links the same code and writes the same pixels to a file, so "does it read
 * as a person" is a question you answer by opening a picture.
 *
 * It also writes CONTACT SHEETS: the two images composited over a mid-grey
 * ground at the sizes they are actually seen at, because a 64x128 contour that
 * looks fine at 1:1 can vanish at the twenty pixels a distant ghost occupies.
 *
 *   gcc -O2 -I../pc_port/include ghost_art_dump.c ../pc_port/src/net/sh_net_art.c -o ghost_art_dump.exe -lm
 *   ghost_art_dump           (writes ghost.png, marker.png, contact.png)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sh_net_art.h"

/* ------------------------------------------------------------------ */
/* Minimal PNG writer                                                  */
/* ------------------------------------------------------------------ */
/* Hand-rolled rather than vendoring stb_image_write for a dev tool. PNG's
 * deflate stream is allowed to be all STORED blocks, so no compressor is
 * needed - only CRC32 for the chunks and Adler32 for the zlib wrapper. */

static unsigned PngCrcTable[256];
static int      PngCrcReady;

static unsigned PngCrc(const unsigned char* p, size_t n, unsigned crc)
{
    size_t i;
    if (!PngCrcReady)
    {
        unsigned k, j;
        for (k = 0; k < 256; k++)
        {
            unsigned c = k;
            for (j = 0; j < 8; j++)
            {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            PngCrcTable[k] = c;
        }
        PngCrcReady = 1;
    }
    for (i = 0; i < n; i++)
    {
        crc = PngCrcTable[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc;
}

static void PngPutU32(FILE* f, unsigned v)
{
    fputc((int)((v >> 24) & 0xFF), f);
    fputc((int)((v >> 16) & 0xFF), f);
    fputc((int)((v >> 8) & 0xFF), f);
    fputc((int)(v & 0xFF), f);
}

static void PngChunk(FILE* f, const char* tag, const unsigned char* data, size_t n)
{
    unsigned crc = 0xFFFFFFFFu;
    PngPutU32(f, (unsigned)n);
    fwrite(tag, 1, 4, f);
    if (n) fwrite(data, 1, n, f);
    crc = PngCrc((const unsigned char*)tag, 4, crc);
    if (n) crc = PngCrc(data, n, crc);
    PngPutU32(f, crc ^ 0xFFFFFFFFu);
}

/* rgb is w*h*3. */
static int PngWriteRgb(const char* path, const unsigned char* rgb, int w, int h)
{
    FILE*          f = fopen(path, "wb");
    unsigned char  hdr[13];
    unsigned char* raw;
    unsigned char* z;
    size_t         rawLen = (size_t)h * (1 + (size_t)w * 3);
    size_t         zLen, zi = 0;
    size_t         off = 0;
    int            y;
    unsigned       a = 1, b = 0;
    size_t         i;

    if (!f) return 0;
    {
        static const unsigned char sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
        fwrite(sig, 1, 8, f);
    }

    hdr[0] = (unsigned char)((w >> 24) & 0xFF); hdr[1] = (unsigned char)((w >> 16) & 0xFF);
    hdr[2] = (unsigned char)((w >> 8) & 0xFF);  hdr[3] = (unsigned char)(w & 0xFF);
    hdr[4] = (unsigned char)((h >> 24) & 0xFF); hdr[5] = (unsigned char)((h >> 16) & 0xFF);
    hdr[6] = (unsigned char)((h >> 8) & 0xFF);  hdr[7] = (unsigned char)(h & 0xFF);
    hdr[8] = 8;  /* bit depth */
    hdr[9] = 2;  /* truecolour */
    hdr[10] = 0; hdr[11] = 0; hdr[12] = 0;
    PngChunk(f, "IHDR", hdr, sizeof(hdr));

    raw = (unsigned char*)malloc(rawLen);
    if (!raw) { fclose(f); return 0; }
    for (y = 0; y < h; y++)
    {
        raw[off++] = 0; /* filter: none */
        memcpy(raw + off, rgb + (size_t)y * w * 3, (size_t)w * 3);
        off += (size_t)w * 3;
    }

    /* zlib: 2-byte header, stored deflate blocks of at most 65535, adler32. */
    zLen = 2 + ((rawLen + 65534) / 65535) * 5 + rawLen + 4;
    z = (unsigned char*)malloc(zLen);
    if (!z) { free(raw); fclose(f); return 0; }
    z[zi++] = 0x78;
    z[zi++] = 0x01;
    off = 0;
    while (off < rawLen)
    {
        size_t n    = (rawLen - off > 65535) ? 65535 : (rawLen - off);
        int    last = (off + n >= rawLen);
        z[zi++] = (unsigned char)(last ? 1 : 0);
        z[zi++] = (unsigned char)(n & 0xFF);
        z[zi++] = (unsigned char)((n >> 8) & 0xFF);
        z[zi++] = (unsigned char)((~n) & 0xFF);
        z[zi++] = (unsigned char)(((~n) >> 8) & 0xFF);
        memcpy(z + zi, raw + off, n);
        zi  += n;
        off += n;
    }
    for (i = 0; i < rawLen; i++)
    {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    z[zi++] = (unsigned char)((b >> 8) & 0xFF);
    z[zi++] = (unsigned char)(b & 0xFF);
    z[zi++] = (unsigned char)((a >> 8) & 0xFF);
    z[zi++] = (unsigned char)(a & 0xFF);

    PngChunk(f, "IDAT", z, zi);
    PngChunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
    return 1;
}

/* Composite premultiplied-by-alpha white art over a background colour, so the
 * saved PNG shows what the prim actually produces rather than an alpha channel
 * no image viewer agrees on how to display. */
static void Blit(unsigned char* dst, int dw, int dh, int dx, int dy,
                 const unsigned char* src, int sw, int sh, int scale,
                 int cr, int cg, int cb)
{
    int x, y;
    for (y = 0; y < sh * scale; y++)
    {
        for (x = 0; x < sw * scale; x++)
        {
            int px = dx + x, py = dy + y;
            int a;
            int so;
            if (px < 0 || py < 0 || px >= dw || py >= dh)
            {
                continue;
            }
            so = ((y / scale) * sw + (x / scale)) * 4;
            a  = src[so + 3];
            {
                int o = (py * dw + px) * 3;
                dst[o + 0] = (unsigned char)((dst[o + 0] * (255 - a) + cr * a) / 255);
                dst[o + 1] = (unsigned char)((dst[o + 1] * (255 - a) + cg * a) / 255);
                dst[o + 2] = (unsigned char)((dst[o + 2] * (255 - a) + cb * a) / 255);
            }
        }
    }
}

/* Nearest-neighbour downscale with alpha averaging, matching what the GPU does
 * to a ghost quad that only covers a handful of pixels. */
static unsigned char* Shrink(const unsigned char* src, int sw, int sh, int dw, int dh)
{
    unsigned char* out = (unsigned char*)calloc((size_t)dw * dh, 4);
    int x, y;
    if (!out)
    {
        return NULL;
    }
    for (y = 0; y < dh; y++)
    {
        for (x = 0; x < dw; x++)
        {
            int x0 = x * sw / dw, x1 = (x + 1) * sw / dw;
            int y0 = y * sh / dh, y1 = (y + 1) * sh / dh;
            int sx, sy, n = 0, acc = 0;
            if (x1 <= x0) x1 = x0 + 1;
            if (y1 <= y0) y1 = y0 + 1;
            for (sy = y0; sy < y1 && sy < sh; sy++)
            {
                for (sx = x0; sx < x1 && sx < sw; sx++)
                {
                    acc += src[(sy * sw + sx) * 4 + 3];
                    n++;
                }
            }
            {
                int o = (y * dw + x) * 4;
                out[o + 0] = 255;
                out[o + 1] = 255;
                out[o + 2] = 255;
                out[o + 3] = (unsigned char)(n ? acc / n : 0);
            }
        }
    }
    return out;
}

static void WriteOver(const char* path, const unsigned char* art, int w, int h,
                      int bgR, int bgG, int bgB, int cr, int cg, int cb)
{
    unsigned char* img = (unsigned char*)malloc((size_t)w * h * 3);
    int i;
    if (!img)
    {
        return;
    }
    for (i = 0; i < w * h; i++)
    {
        img[i * 3 + 0] = (unsigned char)bgR;
        img[i * 3 + 1] = (unsigned char)bgG;
        img[i * 3 + 2] = (unsigned char)bgB;
    }
    Blit(img, w, h, 0, 0, art, w, h, 1, cr, cg, cb);
    PngWriteRgb(path, img, w, h);
    free(img);
    printf("wrote %s (%dx%d)\n", path, w, h);
}

int main(void)
{
    unsigned char* ghost  = ShNetArt_MakeGhost();
    unsigned char* marker = ShNetArt_MakeMarker();

    if (!ghost || !marker)
    {
        fprintf(stderr, "art generation failed\n");
        return 1;
    }

    /* Silent Hill's fog is a pale grey; a ghost that only reads against black
     * would be invisible where it is actually seen. */
    WriteOver("ghost.png", ghost, SHNET_ART_GHOST_W, SHNET_ART_GHOST_H,
              118, 118, 116, 150, 158, 160);
    WriteOver("marker.png", marker, SHNET_ART_MEMO_W, SHNET_ART_MEMO_H,
              52, 50, 46, 158, 142, 96);

    /* Contact sheet: the ghost at the sizes it is really drawn at, over both a
     * fog-grey and a dark-interior ground. */
    {
        const int steps[] = { 128, 96, 64, 40, 24, 14 };
        const int nsteps  = (int)(sizeof(steps) / sizeof(steps[0]));
        const int W = 640, H = 340;
        unsigned char* img = (unsigned char*)malloc((size_t)W * H * 3);
        int i, x, y;

        if (!img)
        {
            return 1;
        }
        for (y = 0; y < H; y++)
        {
            for (x = 0; x < W; x++)
            {
                int o = (y * W + x) * 3;
                int top = (y < H / 2);
                img[o + 0] = (unsigned char)(top ? 122 : 34);
                img[o + 1] = (unsigned char)(top ? 122 : 33);
                img[o + 2] = (unsigned char)(top ? 119 : 31);
            }
        }

        x = 14;
        for (i = 0; i < nsteps; i++)
        {
            int h = steps[i];
            int w = h / 2;
            unsigned char* small = Shrink(ghost, SHNET_ART_GHOST_W, SHNET_ART_GHOST_H, w, h);
            if (small)
            {
                Blit(img, W, H, x, 10 + (140 - h), small, w, h, 1, 150, 158, 160);
                Blit(img, W, H, x, 180 + (140 - h), small, w, h, 1, 150, 158, 160);
                free(small);
            }
            x += w + 22;
        }
        {
            unsigned char* m = Shrink(marker, SHNET_ART_MEMO_W, SHNET_ART_MEMO_H, 56, 56);
            if (m)
            {
                Blit(img, W, H, W - 130, 60, m, 56, 56, 1, 158, 142, 96);
                Blit(img, W, H, W - 130, 230, m, 56, 56, 1, 150, 40, 44);
                free(m);
            }
        }
        PngWriteRgb("contact.png", img, W, H);
        printf("wrote contact.png (%dx%d) - top half fog, bottom half interior\n", W, H);
        free(img);
    }

    free(ghost);
    free(marker);
    return 0;
}
