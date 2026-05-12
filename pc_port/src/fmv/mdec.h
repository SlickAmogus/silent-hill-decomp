/* mdec.h - PSX Motion Decoder for STR FMV decoding.
 *
 * Ported from DuckStation's src/core/mdec.cpp (Old/scalar paths).
 * Hardware-emulation state stripped; input is a raw MDEC bitstream
 * (sequence of u16 halfwords) and output is a 24-bit RGB frame.
 *
 * Usage:
 *     mdec_ctx_t ctx;
 *     mdec_init(&ctx, 320, 240);
 *     mdec_set_quant(&ctx, q_table);
 *     mdec_set_scale_matrix(&ctx, scale_matrix);
 *     mdec_decode_frame(&ctx, bitstream, bs_halfwords, rgb_out);
 */
#ifndef MDEC_H
#define MDEC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int     width;             /* frame width in pixels (multiple of 16) */
    int     height;            /* frame height in pixels (multiple of 16) */
    uint8_t iq_y[64];          /* Y quantization table */
    uint8_t iq_uv[64];         /* Chroma quantization table */
    int16_t scale_table[64];   /* IDCT scale matrix */
    int     output_signed;     /* 0=unsigned 0..255, 1=signed -128..127 */
} mdec_ctx_t;

/* Initialize with default tables (standard STR quant + IDCT cosine matrix).
 * Width/height must be multiples of 16. */
void mdec_init(mdec_ctx_t* ctx, int width, int height);

/* Override quant tables. Most STR frames use the standard table set by
 * mdec_init; only override if your bitstream has custom ones. */
void mdec_set_quant_y(mdec_ctx_t* ctx, const uint8_t qt[64]);
void mdec_set_quant_uv(mdec_ctx_t* ctx, const uint8_t qt[64]);
void mdec_set_scale_matrix(mdec_ctx_t* ctx, const int16_t st[64]);

/* Decode a complete frame from the bitstream into rgb_out.
 *
 *   bs              — bitstream halfword array (after any STR frame header)
 *   bs_halfwords    — length of bs in u16 units
 *   rgb_out         — output buffer, 3 bytes/pixel (R,G,B), tightly packed,
 *                     size = width * height * 3 bytes
 *
 * Returns the number of macroblocks decoded, or -1 on bitstream error
 * (truncated / malformed). */
int mdec_decode_frame(const mdec_ctx_t* ctx,
                      const uint16_t* bs, size_t bs_halfwords,
                      uint8_t* rgb_out);

#ifdef __cplusplus
}
#endif

#endif /* MDEC_H */
