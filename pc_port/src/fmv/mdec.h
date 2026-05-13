/* mdec.h — PSX MDEC (Motion Decoder) for STR FMV decoding.
 *
 * Decodes a complete VLC-compressed MDEC bitstream (as produced by
 * str_read_frame() — i.e., the raw bytes after the per-sector CDSECTOR
 * header) into a tightly packed RGB24 frame buffer.
 *
 * The bitstream format is essentially intra-only MPEG-1:
 *   bytes 0..3   preamble (last 2 bytes are 0x3800 marker)
 *   bytes 4..5   qscale (uint16, big-endian after halfword bswap)
 *   bytes 6..7   version (uint16; 1 or 2 supported, 3+ uses different DC)
 *   bytes 8..    VLC-coded macroblocks (MPEG-1 intra RL VLC + escapes + EOB)
 *
 * Macroblock layout: 6 blocks per macroblock (Cr, Cb, Y0, Y1, Y2, Y3),
 * decoded in column-major order (top-to-bottom first).
 *
 * Reference: FFmpeg libavcodec/mdec.c (LGPL) and the PSX-spec docs at
 * psx-spx.consoledev.net.
 */
#ifndef MDEC_H
#define MDEC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int width;          /* frame width in pixels (multiple of 16) */
    int height;         /* frame height in pixels (multiple of 16) */
    int output_signed;  /* 0 = unsigned 0..255, 1 = signed -128..127 */
} mdec_ctx_t;

/* Initialize a frame context. Width/height must be multiples of 16. */
void mdec_init(mdec_ctx_t* ctx, int width, int height);

/* Decode one full VLC-encoded MDEC frame into rgb_out (3 bytes/pixel,
 * R, G, B). Returns number of macroblocks decoded, or -1 on bitstream
 * error. */
int mdec_decode_frame(mdec_ctx_t* ctx,
                      const uint8_t* bytes, size_t n_bytes,
                      uint8_t* rgb_out);

#ifdef __cplusplus
}
#endif

#endif /* MDEC_H */
