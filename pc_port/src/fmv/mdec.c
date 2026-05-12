/* mdec.c — PSX Motion Decoder, C port of DuckStation's mdec.cpp (Old paths).
 *
 * Pipeline for one macroblock (16x16 RGB):
 *
 *     6 blocks (Cr, Cb, Y0, Y1, Y2, Y3), each 8x8 DCT
 *         │
 *         ▼  decode_rle_block — variable-length RLE → quantized DCT coeffs
 *         │  inverse-zigzag into block, multiply by quant table + q_scale
 *         ▼
 *     idct_block — 8x8 inverse DCT via the scale matrix (separable 1D twice)
 *         │
 *         ▼  yuv_to_rgb — for each of four 8x8 luma quadrants, fetch upsampled
 *         │              4:2:0 chroma and convert YCbCr → RGB (BT.601, scalar)
 *         ▼
 *     16x16 RGB tile, copied to the output frame at (mb_x*16, mb_y*16)
 *
 * No FIFO state, no DMA, no register emulation. Takes a contiguous u16 bitstream
 * and emits a tightly packed RGB24 frame. */

#include "mdec.h"

#include <string.h>

/* ---------- helpers ---------- */

static inline int sign_extend_n(int value, int bits)
{
    int shift = 32 - bits;
    return (int)((unsigned)value << shift) >> shift;
}

static inline int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Zig-zag de-scan table (DuckStation's "zagzig"): position-in-block at
 * each sequential coefficient. Used by the legacy/scalar RLE path. */
static const uint8_t s_zagzig[64] = {
    0,  1,  8, 16,  9,  2,  3, 10,
   17, 24, 32, 25, 18, 11,  4,  5,
   12, 19, 26, 33, 40, 48, 41, 34,
   27, 20, 13,  6,  7, 14, 21, 28,
   35, 42, 49, 56, 57, 50, 43, 36,
   29, 22, 15, 23, 30, 37, 44, 51,
   58, 59, 52, 45, 38, 31, 39, 46,
   53, 60, 61, 54, 47, 55, 62, 63
};

/* ---------- default tables (standard STR) ---------- */

/* Standard MDEC quantization table — common to most PSX STR files.
 * Source: nocash psx-spec.txt §"MDEC Quant Table". This matches what
 * Konami's STR files load into MDEC via the SetQuantTable command. */
static const uint8_t s_default_quant[64] = {
     2, 16, 19, 22, 26, 27, 29, 34,
    16, 16, 22, 24, 27, 29, 34, 37,
    19, 22, 26, 27, 29, 34, 34, 38,
    22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48,
    26, 27, 29, 32, 35, 40, 48, 58,
    26, 27, 29, 34, 38, 46, 56, 69,
    27, 29, 35, 38, 46, 56, 69, 83
};

/* Standard IDCT scale matrix (Q15 cosine table the MDEC uses).
 * From nocash psx-spec.txt §"MDEC IDCT". Konami STR uses this default. */
static const int16_t s_default_scale[64] = {
     23170,  23170,  23170,  23170,  23170,  23170,  23170,  23170,
     32138,  27246,  18205,   6393,  -6393, -18205, -27246, -32138,
     30274,  12540, -12540, -30274, -30274, -12540,  12540,  30274,
     27246,  -6393, -32138, -18205,  18205,  32138,   6393, -27246,
     23170, -23170, -23170,  23170,  23170, -23170, -23170,  23170,
     18205, -32138,   6393,  27246, -27246,  -6393,  32138, -18205,
     12540, -30274,  30274, -12540, -12540,  30274, -30274,  12540,
      6393, -18205,  27246, -32138,  32138, -27246,  18205,  -6393
};

/* ---------- decode pipeline ---------- */

/* Parse one 8x8 block of RLE-encoded coefficients from the bitstream.
 *
 *   blk      — output 64 s16 (zig-zag descrambled, quantized)
 *   qt       — 64-byte quant table (iq_y or iq_uv)
 *   bs       — bitstream halfword array
 *   bs_count — total halfwords available
 *   bs_pos   — in/out cursor (advanced past block's data)
 *
 * Returns 1 on success, 0 on bitstream exhausted before block end-of-block. */
static int decode_rle_block(int16_t* blk, const uint8_t* qt,
                            const uint16_t* bs, size_t bs_count, size_t* bs_pos)
{
    memset(blk, 0, sizeof(int16_t) * 64);

    /* Skip 0xFE00 padding halfwords, then the first halfword is q_scale (top 6
     * bits) + the DC coefficient (signed 10-bit at bottom). */
    uint16_t n;
    for (;;) {
        if (*bs_pos >= bs_count) return 0;
        n = bs[(*bs_pos)++];
        if (n != 0xFE00) break;
    }

    int q_scale = (n >> 10) & 0x3F;
    int coef    = 0;

    int val = sign_extend_n((int)(n & 0x3FF), 10) * (int)qt[coef];
    if (q_scale == 0)
        val = sign_extend_n((int)(n & 0x3FF), 10) * 2;
    val = clampi(val, -0x400, 0x3FF);
    if (q_scale > 0)
        blk[s_zagzig[coef]] = (int16_t)val;
    else
        blk[coef] = (int16_t)val;

    /* Subsequent halfwords: (skip_count[15:10] | signed_coef[9:0]).
     * skip_count + 1 advances the coefficient index; the value is multiplied
     * by both the quant table and current q_scale. */
    for (;;) {
        if (*bs_pos >= bs_count) return 0;
        n = bs[(*bs_pos)++];

        coef += ((n >> 10) & 0x3F) + 1;
        if (coef < 64) {
            int v = sign_extend_n((int)(n & 0x3FF), 10);
            int val_q = (v * (int)qt[coef] * q_scale + 4) / 8;
            if (q_scale == 0)
                val_q = v * 2;
            val_q = clampi(val_q, -0x400, 0x3FF);
            if (q_scale > 0)
                blk[s_zagzig[coef]] = (int16_t)val_q;
            else
                blk[coef] = (int16_t)val_q;
        }
        if (coef >= 63)
            return 1; /* end of block */
    }
}

/* Inverse DCT, scalar (separable 1D twice). Output values are clamped to
 * 9-bit signed and stored back into blk[0..63]. */
static void idct_block(int16_t* blk, const int16_t* scale)
{
    int64_t temp[64];

    /* First pass: column-wise. temp[x + y*8] = sum_u blk[u*8+x] * scale[y*8+u] */
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            int64_t sum = 0;
            for (int u = 0; u < 8; u++)
                sum += (int32_t)blk[u * 8 + x] * (int32_t)scale[y * 8 + u];
            temp[x + y * 8] = sum;
        }
    }
    /* Second pass: row-wise on the transposed temp. */
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            int64_t sum = 0;
            for (int u = 0; u < 8; u++)
                sum += temp[u + y * 8] * (int32_t)scale[x * 8 + u];

            int32_t v = (int32_t)((sum >> 32) + ((sum >> 31) & 1));
            v = sign_extend_n(v, 9);
            blk[x + y * 8] = (int16_t)clampi(v, -128, 127);
        }
    }
}

/* Convert one 8x8 luma block (Y_blk) + its co-sited 4:2:0 chroma into an 8x8
 * RGB tile written into block_rgb at offset (xx, yy) within the 16x16
 * macroblock buffer. addval = 0x80 for unsigned output, 0 for signed. */
static void yuv_to_rgb_tile(unsigned xx, unsigned yy,
                            const int16_t* Crblk, const int16_t* Cbblk,
                            const int16_t* Yblk,
                            uint32_t* block_rgb, int output_signed)
{
    int addval = output_signed ? 0 : 0x80;
    for (unsigned y = 0; y < 8; y++) {
        for (unsigned x = 0; x < 8; x++) {
            int Cr_idx = ((x + xx) / 2) + ((y + yy) / 2) * 8;
            int R0 = Crblk[Cr_idx];
            int B0 = Cbblk[Cr_idx];
            /* BT.601 coefficients used by PSX MDEC. */
            int G0 = (int)(-0.3437f * (float)B0 + -0.7143f * (float)R0);
            R0     = (int)(1.4020f * (float)R0);
            B0     = (int)(1.7720f * (float)B0);

            int Y = Yblk[x + y * 8];
            int R = clampi(Y + R0, -128, 127) + addval;
            int G = clampi(Y + G0, -128, 127) + addval;
            int B = clampi(Y + B0, -128, 127) + addval;

            block_rgb[(x + xx) + ((y + yy) * 16)] =
                ((uint32_t)(uint8_t)R) |
                ((uint32_t)(uint8_t)G << 8) |
                ((uint32_t)(uint8_t)B << 16);
        }
    }
}

/* ---------- public API ---------- */

void mdec_init(mdec_ctx_t* ctx, int width, int height)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->width  = width;
    ctx->height = height;
    memcpy(ctx->iq_y,  s_default_quant, 64);
    memcpy(ctx->iq_uv, s_default_quant, 64);
    memcpy(ctx->scale_table, s_default_scale, sizeof(s_default_scale));
    ctx->output_signed = 0;
}

void mdec_set_quant_y(mdec_ctx_t* ctx, const uint8_t qt[64])
{
    memcpy(ctx->iq_y, qt, 64);
}

void mdec_set_quant_uv(mdec_ctx_t* ctx, const uint8_t qt[64])
{
    memcpy(ctx->iq_uv, qt, 64);
}

void mdec_set_scale_matrix(mdec_ctx_t* ctx, const int16_t st[64])
{
    memcpy(ctx->scale_table, st, sizeof(int16_t) * 64);
}

int mdec_decode_frame(const mdec_ctx_t* ctx,
                      const uint16_t* bs, size_t bs_halfwords,
                      uint8_t* rgb_out)
{
    int mb_x_count = ctx->width  / 16;
    int mb_y_count = ctx->height / 16;
    size_t bs_pos  = 0;
    int mb_done    = 0;

    /* MDEC macroblock order: top-to-bottom *first*, then left-to-right.
     * Each frame's bitstream lays them out column-by-column from left. */
    for (int mb_x = 0; mb_x < mb_x_count; mb_x++) {
        for (int mb_y = 0; mb_y < mb_y_count; mb_y++) {
            int16_t blocks[6][64]; /* Cr, Cb, Y0, Y1, Y2, Y3 */

            for (int b = 0; b < 6; b++) {
                const uint8_t* qt = (b >= 2) ? ctx->iq_y : ctx->iq_uv;
                if (!decode_rle_block(blocks[b], qt, bs, bs_halfwords, &bs_pos))
                    return -1;
                idct_block(blocks[b], ctx->scale_table);
            }

            uint32_t block_rgb[256]; /* 16x16 RGB tile */
            yuv_to_rgb_tile(0, 0, blocks[0], blocks[1], blocks[2], block_rgb, ctx->output_signed);
            yuv_to_rgb_tile(8, 0, blocks[0], blocks[1], blocks[3], block_rgb, ctx->output_signed);
            yuv_to_rgb_tile(0, 8, blocks[0], blocks[1], blocks[4], block_rgb, ctx->output_signed);
            yuv_to_rgb_tile(8, 8, blocks[0], blocks[1], blocks[5], block_rgb, ctx->output_signed);

            /* Copy the 16x16 tile into the output frame at (mb_x*16, mb_y*16). */
            int base_x = mb_x * 16;
            int base_y = mb_y * 16;
            for (int y = 0; y < 16; y++) {
                uint8_t* dst = rgb_out + ((base_y + y) * ctx->width + base_x) * 3;
                for (int x = 0; x < 16; x++) {
                    uint32_t px = block_rgb[x + y * 16];
                    dst[x * 3 + 0] = (uint8_t)(px       & 0xFF);
                    dst[x * 3 + 1] = (uint8_t)(px >> 8  & 0xFF);
                    dst[x * 3 + 2] = (uint8_t)(px >> 16 & 0xFF);
                }
            }
            mb_done++;
        }
    }
    return mb_done;
}
