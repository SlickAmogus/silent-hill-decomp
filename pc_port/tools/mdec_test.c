/* mdec_test.c — standalone tester for mdec.c
 *
 * Usage:
 *   mdec_test <bitstream.bin> <width> <height> <output.ppm>
 *
 * The bitstream is expected to be a contiguous u16 array of MDEC RLE
 * data (no STR sector wrapping, no frame header). To extract one from
 * a real STR file you can use jPSXdec or DuckStation's "Save Frame"
 * feature. For pure code verification you can also feed it a synthetic
 * bitstream (e.g. the 0xFE00 padding + a few DC-only coefficients).
 *
 * Output is a binary PPM (P6) for easy visual inspection in any image
 * viewer.
 *
 * Build (standalone):
 *   cc -O2 -o mdec_test mdec_test.c ../src/fmv/mdec.c
 */

#include "../src/fmv/mdec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int usage(const char* argv0)
{
    fprintf(stderr,
        "usage: %s <bitstream.bin> <width> <height> <output.ppm>\n"
        "  bitstream.bin  raw u16 MDEC bitstream (no STR header)\n"
        "  width/height   frame size in pixels (multiples of 16)\n"
        "  output.ppm     output P6 image\n",
        argv0);
    return 1;
}

int main(int argc, char** argv)
{
    if (argc != 5) return usage(argv[0]);

    const char* in_path  = argv[1];
    int         width    = atoi(argv[2]);
    int         height   = atoi(argv[3]);
    const char* out_path = argv[4];

    if (width <= 0 || height <= 0 || (width & 15) || (height & 15)) {
        fprintf(stderr, "error: width/height must be positive multiples of 16\n");
        return 1;
    }

    /* Read the bitstream file fully into memory. */
    FILE* fin = fopen(in_path, "rb");
    if (!fin) { perror(in_path); return 1; }
    if (fseek(fin, 0, SEEK_END) != 0) { perror("fseek"); return 1; }
    long sz = ftell(fin);
    if (sz < 0) { perror("ftell"); return 1; }
    rewind(fin);

    if (sz & 1) {
        fprintf(stderr, "error: bitstream size %ld is odd; must be u16-aligned\n", sz);
        fclose(fin);
        return 1;
    }
    size_t bs_halfwords = (size_t)sz / 2;
    uint16_t* bs = (uint16_t*)malloc((size_t)sz);
    if (!bs) { perror("malloc"); fclose(fin); return 1; }
    if (fread(bs, 1, (size_t)sz, fin) != (size_t)sz) {
        perror("fread");
        free(bs);
        fclose(fin);
        return 1;
    }
    fclose(fin);

    fprintf(stderr, "[mdec_test] loaded %zu halfwords from %s\n", bs_halfwords, in_path);

    /* Decode. */
    mdec_ctx_t ctx;
    mdec_init(&ctx, width, height);
    /* (Default quant table + scale matrix from mdec_init match what most
     *  STR FMVs use.  If your test bitstream needs different tables, tweak
     *  ctx.iq_y / ctx.iq_uv / ctx.scale_table before this call.) */

    size_t out_bytes = (size_t)width * (size_t)height * 3u;
    uint8_t* rgb = (uint8_t*)malloc(out_bytes);
    if (!rgb) { perror("malloc rgb"); free(bs); return 1; }
    memset(rgb, 0, out_bytes);

    int mb_done = mdec_decode_frame(&ctx, bs, bs_halfwords, rgb);
    if (mb_done < 0) {
        fprintf(stderr, "[mdec_test] decode failed (bitstream exhausted)\n");
        /* Still write what we have so we can inspect partial output. */
    } else {
        fprintf(stderr, "[mdec_test] decoded %d macroblocks (%dx%d)\n",
                mb_done, width, height);
    }

    /* Write PPM. */
    FILE* fout = fopen(out_path, "wb");
    if (!fout) { perror(out_path); free(rgb); free(bs); return 1; }
    fprintf(fout, "P6\n%d %d\n255\n", width, height);
    fwrite(rgb, 1, out_bytes, fout);
    fclose(fout);

    fprintf(stderr, "[mdec_test] wrote %s\n", out_path);

    free(rgb);
    free(bs);
    return 0;
}
