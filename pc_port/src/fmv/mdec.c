/* mdec.c — PSX MDEC decoder (intra-only MPEG-1 variant).
 *
 * Pipeline per macroblock:
 *
 *     6 blocks (Cr, Cb, Y0, Y1, Y2, Y3) — each 8x8 DCT block
 *         │
 *         ▼  decode_block_intra: variable-length Huffman (MPEG-1 RL VLC)
 *         │  → run/level pairs → dequantized DCT coefficients
 *         ▼
 *     idct_block: inverse DCT (separable 1D twice via cosine matrix)
 *         │
 *         ▼  yuv_to_rgb_tile: BT.601 YCbCr → RGB w/ 4:2:0 chroma upsampling
 *         ▼
 *     16x16 RGB tile, copied to the output frame at (mb_x*16, mb_y*16)
 *
 * Bitstream is MSB-first within little-endian halfwords. We byte-swap the
 * incoming bytes to big-endian halfwords once, then read bits MSB-first.
 *
 * Adapted from FFmpeg libavcodec/mdec.c (LGPL) with the MPEG-1 RL VLC
 * tables from libavcodec/mpeg12data.c. */

#include "mdec.h"

#include <string.h>
#include <stdlib.h>

/* Decode tracing: 0 = off, 1 = per-coefficient, 2 = errors only. */
#define MDEC_TRACE 2

#if MDEC_TRACE
#  include <stdio.h>
#  include <stdarg.h>
extern FILE* g_ShDebugLog;
static inline void mdec_trace(const char* fmt, ...) {
    if (!g_ShDebugLog) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(g_ShDebugLog, fmt, ap);
    va_end(ap);
    fflush(g_ShDebugLog);
}
#endif

#if MDEC_TRACE == 1
#  define TRACE(...) mdec_trace(__VA_ARGS__)
#  define TRACE_ERR(...) mdec_trace(__VA_ARGS__)
#elif MDEC_TRACE == 2
#  define TRACE(...) ((void)0)
#  define TRACE_ERR(...) mdec_trace(__VA_ARGS__)
#else
#  define TRACE(...) ((void)0)
#  define TRACE_ERR(...) ((void)0)
#endif

/* =====================================================================
 *  Static tables (MPEG-1 standard)
 * ===================================================================== */

/* Zigzag scan order for an 8x8 block. */
static const uint8_t s_zigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

/* MPEG-1 default LUMA intra quantization matrix (natural order). */
static const uint8_t s_intra_matrix[64] = {
     8, 16, 19, 22, 26, 27, 29, 34,
    16, 16, 22, 24, 27, 29, 34, 37,
    19, 22, 26, 27, 29, 34, 34, 38,
    22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48,
    26, 27, 29, 32, 35, 40, 48, 58,
    26, 27, 29, 34, 38, 46, 56, 69,
    27, 29, 35, 38, 46, 56, 69, 83
};

/* MPEG-1 default CHROMA intra quantization matrix (all 16s). PSX BIOS
 * DecDCTReset(0) loads this as the iq_uv table; without using it, chroma
 * coefficients are dequantized using the luma matrix (8..83), which makes
 * high-frequency chroma 2-5x stronger than intended and produces visible
 * color-blocking artifacts on natural-image FMVs. */
static const uint8_t s_chroma_matrix[64] = {
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
};

/* IDCT cosine matrix (Q15 scale, separable 1D used twice). This is the
 * TRANSPOSE of the standard forward-DCT matrix `M` — DuckStation's
 * IDCT_Old computes `SCALE * BLK * SCALE^T`, which for that to equal the
 * standard inverse `M^T * F * M` requires `SCALE = M^T`.
 *
 * Storing `M` directly (rows of basis cosines indexed by frequency k)
 * makes DC-only blocks decode to a non-uniform block — each pixel is
 * scaled by `M[y,0] * M[x,0]`, which only equals `a(0)^2` at the (0,0)
 * corner. That mismatch shows up as the "tiny squares with bright
 * highlight per cell" pattern. Transposing fixes it.
 *
 * Row k of M^T = column k of M = the basis vector for frequency k:
 *   M^T[k, n] = a(k) * cos((2n+1)*k*pi/16)
 * with `a(0) = 1/sqrt(2)`, `a(k>0) = 1`, scaled to Q15 (×32768).
 * (Note: row 0 stays constant 23170; only rows 1..7 differ from the
 * forward matrix, since column k of M = a(k) * cos((2*0+1)*k*pi/16),
 * a(0)*cos((2*1+1)*k*pi/16), … instead of M[k, j] for fixed k.) */
static const int16_t s_scale_table[64] = {
    /* M^T[k, n] for k=0..7, n=0..7 */
     23170,  32138,  30274,  27246,  23170,  18205,  12540,   6393,
     23170,  27246,  12540,  -6393, -23170, -32138, -30274, -18205,
     23170,  18205, -12540, -32138, -23170,   6393,  30274,  27246,
     23170,   6393, -30274, -18205,  23170,  27246, -12540, -32138,
     23170,  -6393, -30274,  18205,  23170, -27246, -12540,  32138,
     23170, -18205, -12540,  32138, -23170,  -6393,  30274, -27246,
     23170, -27246,  12540,   6393, -23170,  32138, -30274,  18205,
     23170, -32138,  30274, -27246,  23170, -18205,  12540,  -6393
};

/* MPEG-1 run/level/code/bits tables for intra blocks.
 * 111 normal entries + 1 escape + 1 EOB = 113 entries.
 *
 * Source: FFmpeg libavcodec/mpeg12data.c (ff_mpeg12_run / ff_mpeg12_level
 * / ff_mpeg1_vlc_table). */
#define MPEG12_RL_NB_ELEMS 111

static const uint16_t s_vlc_code[MPEG12_RL_NB_ELEMS + 2][2] = {
    { 0x3,  2 }, { 0x4,  4 }, { 0x5,  5 }, { 0x6,  7 },
    { 0x26, 8 }, { 0x21, 8 }, { 0xa, 10 }, { 0x1d, 12 },
    { 0x18,12 }, { 0x13,12 }, { 0x10,12 }, { 0x1a, 13 },
    { 0x19,13 }, { 0x18,13 }, { 0x17,13 }, { 0x1f, 14 },
    { 0x1e,14 }, { 0x1d,14 }, { 0x1c,14 }, { 0x1b, 14 },
    { 0x1a,14 }, { 0x19,14 }, { 0x18,14 }, { 0x17, 14 },
    { 0x16,14 }, { 0x15,14 }, { 0x14,14 }, { 0x13, 14 },
    { 0x12,14 }, { 0x11,14 }, { 0x10,14 }, { 0x18, 15 },
    { 0x17,15 }, { 0x16,15 }, { 0x15,15 }, { 0x14, 15 },
    { 0x13,15 }, { 0x12,15 }, { 0x11,15 }, { 0x10, 15 },
    { 0x3,  3 }, { 0x6,  6 }, { 0x25, 8 }, { 0xc, 10 },
    { 0x1b,12 }, { 0x16,13 }, { 0x15,13 }, { 0x1f, 15 },
    { 0x1e,15 }, { 0x1d,15 }, { 0x1c,15 }, { 0x1b, 15 },
    { 0x1a,15 }, { 0x19,15 }, { 0x13,16 }, { 0x12, 16 },
    { 0x11,16 }, { 0x10,16 }, { 0x5,  4 }, { 0x4,  7 },
    { 0xb, 10 }, { 0x14,12 }, { 0x14,13 }, { 0x7,  5 },
    { 0x24, 8 }, { 0x1c,12 }, { 0x13,13 }, { 0x6,  5 },
    { 0xf, 10 }, { 0x12,12 }, { 0x7,  6 }, { 0x9, 10 },
    { 0x12,13 }, { 0x5,  6 }, { 0x1e,12 }, { 0x14, 16 },
    { 0x4,  6 }, { 0x15,12 }, { 0x7,  7 }, { 0x11, 12 },
    { 0x5,  7 }, { 0x11,13 }, { 0x27, 8 }, { 0x10, 13 },
    { 0x23, 8 }, { 0x1a,16 }, { 0x22, 8 }, { 0x19, 16 },
    { 0x20, 8 }, { 0x18,16 }, { 0xe, 10 }, { 0x17, 16 },
    { 0xd, 10 }, { 0x16,16 }, { 0x8, 10 }, { 0x15, 16 },
    { 0x1f,12 }, { 0x1a,12 }, { 0x19,12 }, { 0x17, 12 },
    { 0x16,12 }, { 0x1f,13 }, { 0x1e,13 }, { 0x1d, 13 },
    { 0x1c,13 }, { 0x1b,13 }, { 0x1f,16 }, { 0x1e, 16 },
    { 0x1d,16 }, { 0x1c,16 }, { 0x1b,16 },
    { 0x1,  6 }, /* index 111 = escape */
    { 0x2,  2 }, /* index 112 = EOB */
};

static const int8_t s_vlc_run[MPEG12_RL_NB_ELEMS] = {
     0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,
     1,  1,  1,  1,  1,  1,  1,  1,
     1,  1,  1,  1,  1,  1,  1,  1,
     1,  1,  2,  2,  2,  2,  2,  3,
     3,  3,  3,  4,  4,  4,  5,  5,
     5,  6,  6,  6,  7,  7,  8,  8,
     9,  9, 10, 10, 11, 11, 12, 12,
    13, 13, 14, 14, 15, 15, 16, 16,
    17, 18, 19, 20, 21, 22, 23, 24,
    25, 26, 27, 28, 29, 30, 31,
};

static const int8_t s_vlc_level[MPEG12_RL_NB_ELEMS] = {
     1,  2,  3,  4,  5,  6,  7,  8,
     9, 10, 11, 12, 13, 14, 15, 16,
    17, 18, 19, 20, 21, 22, 23, 24,
    25, 26, 27, 28, 29, 30, 31, 32,
    33, 34, 35, 36, 37, 38, 39, 40,
     1,  2,  3,  4,  5,  6,  7,  8,
     9, 10, 11, 12, 13, 14, 15, 16,
    17, 18,  1,  2,  3,  4,  5,  1,
     2,  3,  4,  1,  2,  3,  1,  2,
     3,  1,  2,  3,  1,  2,  1,  2,
     1,  2,  1,  2,  1,  2,  1,  2,
     1,  2,  1,  2,  1,  2,  1,  2,
     1,  1,  1,  1,  1,  1,  1,  1,
     1,  1,  1,  1,  1,  1,  1,
};

/* =====================================================================
 *  VLC lookup table — built lazily once, 16-bit window
 *
 *  Index = next 16 bits of stream (MSB first). Result encodes either:
 *   - normal entry: run+1 in entry[0], level in entry[1], bits in entry[2]
 *   - escape:       entry[3] bit 0 set
 *   - EOB:          entry[3] bit 1 set
 *
 *  Codes are prefix-free, so a single 16-bit lookup is unambiguous.
 * ===================================================================== */

typedef struct {
    int8_t  run;
    int8_t  level;
    uint8_t bits;
    uint8_t flags;  /* bit0=escape, bit1=EOB, bit7=invalid */
} vlc_lut_entry_t;

static vlc_lut_entry_t s_vlc_lut[65536];
static int s_vlc_lut_built = 0;

/* Diagnostic: nonzero = we already reported a desync this frame. */
static int s_desync_logged_this_frame;
static int s_frame_index;

static void build_vlc_lut(void) {
    if (s_vlc_lut_built) return;
    for (int i = 0; i < 65536; i++) s_vlc_lut[i].flags = 0x80; /* invalid */

    for (int e = 0; e < MPEG12_RL_NB_ELEMS + 2; e++) {
        int code = s_vlc_code[e][0];
        int bits = s_vlc_code[e][1];
        int prefix    = code << (16 - bits);
        int wild_low  = (1 << (16 - bits)) - 1;
        for (int low = 0; low <= wild_low; low++) {
            int idx = prefix | low;
            if (!(s_vlc_lut[idx].flags & 0x80)) continue; /* already set by a longer code? */
            s_vlc_lut[idx].bits  = (uint8_t)bits;
            s_vlc_lut[idx].flags = 0;
            if (e == MPEG12_RL_NB_ELEMS) {
                s_vlc_lut[idx].flags = 0x01; /* escape */
            } else if (e == MPEG12_RL_NB_ELEMS + 1) {
                s_vlc_lut[idx].flags = 0x02; /* EOB */
            } else {
                s_vlc_lut[idx].run   = s_vlc_run[e];
                s_vlc_lut[idx].level = s_vlc_level[e];
            }
        }
    }
    s_vlc_lut_built = 1;

    /* Sanity dump of a few specific entries we care about. */
    TRACE("[mdec-vlc] lut[0x0501] flags=0x%02x run=%d level=%d bits=%d\n",
          s_vlc_lut[0x0501].flags, s_vlc_lut[0x0501].run,
          s_vlc_lut[0x0501].level, s_vlc_lut[0x0501].bits);
    TRACE("[mdec-vlc] lut[0x4000] flags=0x%02x bits=%d (escape range start)\n",
          s_vlc_lut[0x4000].flags, s_vlc_lut[0x4000].bits);
    TRACE("[mdec-vlc] lut[0xC000] flags=0x%02x run=%d level=%d bits=%d (code 0b11)\n",
          s_vlc_lut[0xC000].flags, s_vlc_lut[0xC000].run,
          s_vlc_lut[0xC000].level, s_vlc_lut[0xC000].bits);
}

/* =====================================================================
 *  Bit reader  (MSB-first within byteswapped halfwords)
 *
 *  The PSX MDEC stores its bitstream as little-endian uint16_t values
 *  with bits packed MSB-first WITHIN each halfword. So in raw byte
 *  form, byte order is { byte1, byte0, byte3, byte2, ... }. We pre-swap
 *  the bytes once at frame open so we can read MSB-first sequentially.
 * ===================================================================== */

typedef struct {
    const uint8_t* data;
    size_t         len_bytes;
    size_t         byte_pos;
    uint32_t       cache;
    int            cache_bits;
} bitreader_t;

static void br_init(bitreader_t* br, const uint8_t* data, size_t len) {
    br->data       = data;
    br->len_bytes  = len;
    br->byte_pos   = 0;
    br->cache      = 0;
    br->cache_bits = 0;
}

static void br_fill_cache(bitreader_t* br) {
    while (br->cache_bits <= 24 && br->byte_pos < br->len_bytes) {
        br->cache |= (uint32_t)br->data[br->byte_pos++] << (24 - br->cache_bits);
        br->cache_bits += 8;
    }
}

/* Peek the next 16 bits (MSB-first), padding with zeros if past EOF. */
static uint32_t br_peek16(bitreader_t* br) {
    br_fill_cache(br);
    return br->cache >> 16;
}

/* Shifting a uint32_t by 32 is undefined in C and on x86-64 silently turns
 * into a no-op (count masked to 5 bits) — which leaves stale preamble bits
 * in the cache and corrupts the next read. Use 64-bit math for the shift,
 * then truncate back to 32 bits. */
static inline uint32_t shl32_safe(uint32_t v, int n) {
    return (uint32_t)(((uint64_t)v << n) & 0xFFFFFFFFu);
}

static void br_skip(bitreader_t* br, int n) {
    br->cache = shl32_safe(br->cache, n);
    br->cache_bits -= n;
}

/* Read n bits (1..32) unsigned. */
static uint32_t br_get_ubits(bitreader_t* br, int n) {
    br_fill_cache(br);
    uint32_t v = (n == 32) ? br->cache : (br->cache >> (32 - n));
    br->cache = shl32_safe(br->cache, n);
    br->cache_bits -= n;
    return v;
}

/* Read n bits as signed two's-complement. */
static int32_t br_get_sbits(bitreader_t* br, int n) {
    uint32_t u = br_get_ubits(br, n);
    int32_t shift = 32 - n;
    return (int32_t)(u << shift) >> shift;
}

/* =====================================================================
 *  Block decode (FFmpeg-style)
 * ===================================================================== */

static inline int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Decode one 8x8 block: read DC, then AC coefficients via VLC, fill blk[64]
 * with dequantized DCT coefficients in NATURAL (not zigzag) order.
 * `qt` is the quant table (luma or chroma) for this block.
 *
 * Returns 0 on success, -1 on bitstream error / damaged stream. */
static int decode_block_intra(bitreader_t* br, int16_t* blk, int qscale, int version,
                              const uint8_t* qt)
{
    memset(blk, 0, 64 * sizeof(int16_t));

    /* DC coefficient (PSX MDEC v1/v2). FFmpeg's formula is
     *   block[0] = 2 * sext10 + 1024
     * which centers DC at 1024 (mid-gray). Subtracting 1024 gives us a
     * value centered at 0 (matching our DuckStation-style IDCT which
     * outputs signed -128..127). This produces visually correct output
     * matching jpsxdec — the DuckStation-style `sext10 * qt[0]` formula
     * over-saturates mid-dark Y values to pure black. */
    if (version <= 2) {
        int dc = 2 * (int)br_get_sbits(br, 10);
        blk[0] = (int16_t)dc;
    } else {
        /* Version 3+ uses a differential DC encoding we don't currently
         * support — fail rather than mis-decode. */
        return -1;
    }

    TRACE("  [blk] DC raw_sext10 read, blk[0]=%d (br.byte_pos=%zu cache_bits=%d)\n",
          blk[0], br->byte_pos, br->cache_bits);

    /* AC coefficients via VLC. */
    int coef = 0;
    int ac_count = 0;
    for (;;) {
        uint32_t peek = br_peek16(br);
        vlc_lut_entry_t e = s_vlc_lut[peek];
        if (e.flags & 0x80) {
            if (!s_desync_logged_this_frame) {
                TRACE_ERR("[mdec] DESYNC frame=%d peek=0x%04x byte_pos=%zu cache_bits=%d ac=%d coef=%d\n",
                          s_frame_index, peek, br->byte_pos, br->cache_bits, ac_count, coef);
                s_desync_logged_this_frame = 1;
            }
            break;
        }
        ac_count++;

        br_skip(br, e.bits);

        if (e.flags & 0x02) {
            TRACE("    EOB (peek=0x%04x bits=%d)\n", (unsigned)peek, e.bits);
            break;
        }

        int run;
        int level;
        int is_escape = (e.flags & 0x01) != 0;

        if (is_escape) {
            /* Escape: 6 unsigned bits run, then signed 10-bit level (no
             * separate sign bit; the 10-bit field is signed). */
            run   = (int)br_get_ubits(br, 6) + 1;
            level = br_get_sbits(br, 10);
            TRACE("    AC#%d ESCAPE run=%d level=%d (peek was 0x%04x)\n",
                  ac_count, run, level, (unsigned)peek);
            /* level=0 is rare but legal on PSX — FFmpeg dequants it to -1. */
        } else {
            run   = e.run + 1;
            level = e.level;
            int sign = (int)br_get_ubits(br, 1);
            if (sign) level = -level;
            TRACE("    AC#%d run=%d level=%d (peek=0x%04x bits=%d)\n",
                  ac_count, run, level, (unsigned)peek, e.bits);
        }

        coef += run;
        if (coef > 63) {
            /* PSX MDEC hardware (per DuckStation::DecodeRLE_Old) treats
             * coefficient-position overflow as an implicit end-of-block
             * rather than a hard error. Drop this overflowing coefficient
             * and exit the block cleanly. */
            TRACE("    -> coef overflow %d treated as EOB (ac#%d)\n", coef, ac_count);
            break;
        }

        int j = s_zigzag[coef];

        /* Dequantize. The escape path uses round-to-odd to match the
         * MPEG-1 standard; the regular path just multiplies and shifts.
         * Both paths clamp to 11-bit signed before storing — PSX MDEC
         * hardware saturates the dequantized AC to [-1024, 1023], and
         * skipping the clamp causes our 9-bit-truncating IDCT to wrap
         * (high-quantization escape values overflow 9 bits, sign-extend
         * to negative, and produce the "green tile" garbage that an
         * unsaturated decoder shows on the right half of the frame). */
        int dq;
        if (is_escape) {
            int aq;
            if (level < 0) {
                aq = (-level * qscale * (int)qt[j]) >> 3;
                aq = (aq - 1) | 1;
                dq = -aq;
            } else {
                aq = (level * qscale * (int)qt[j]) >> 3;
                aq = (aq - 1) | 1;
                dq = aq;
            }
        } else {
            dq = (level * qscale * (int)qt[j]) >> 3;
        }
        if (dq < -1024) dq = -1024;
        if (dq >  1023) dq =  1023;
        blk[j] = (int16_t)dq;
    }
    return 0;
}

/* Separable IDCT, scalar. Output: 9-bit signed wrap, then clamp to
 * [-128, 127]. This matches DuckStation::MDEC::IDCT_Old exactly — the
 * 9-bit wrap is hardware-accurate behavior. */
static void idct_block(int16_t* blk)
{
    int64_t temp[64];
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            int64_t sum = 0;
            for (int u = 0; u < 8; u++)
                sum += (int32_t)blk[u * 8 + x] * (int32_t)s_scale_table[y * 8 + u];
            temp[x + y * 8] = sum;
        }
    }
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            int64_t sum = 0;
            for (int u = 0; u < 8; u++)
                sum += temp[u + y * 8] * (int32_t)s_scale_table[x * 8 + u];

            int32_t v = (int32_t)((sum >> 32) + ((sum >> 31) & 1));
            v = (v << 23) >> 23;  /* 9-bit sign-extend (hardware wrap) */
            blk[x + y * 8] = (int16_t)clampi(v, -128, 127);
        }
    }
}

/* Convert one 8x8 luma block + co-sited 4:2:0 chroma to an 8x8 RGB tile
 * written into block_rgb at offset (xx, yy) within the 16x16 macroblock. */
static void yuv_to_rgb_tile(unsigned xx, unsigned yy,
                            const int16_t* Crblk, const int16_t* Cbblk,
                            const int16_t* Yblk,
                            uint32_t* block_rgb, int output_signed)
{
    int addval = output_signed ? 0 : 0x80;
    for (unsigned y = 0; y < 8; y++) {
        for (unsigned x = 0; x < 8; x++) {
            int idx = ((x + xx) / 2) + ((y + yy) / 2) * 8;
            int R0 = Crblk[idx];
            int B0 = Cbblk[idx];
            int G0 = (int)(-0.3437f * (float)B0 + -0.7143f * (float)R0);
            R0     = (int)( 1.4020f * (float)R0);
            B0     = (int)( 1.7720f * (float)B0);

            int Y = Yblk[x + y * 8];
            int R = clampi(Y + R0, -128, 127) + addval;
            int G = clampi(Y + G0, -128, 127) + addval;
            int B = clampi(Y + B0, -128, 127) + addval;
            block_rgb[(x + xx) + ((y + yy) * 16)] =
                ((uint32_t)(uint8_t)R)        |
                ((uint32_t)(uint8_t)G <<  8)  |
                ((uint32_t)(uint8_t)B << 16);
        }
    }
}

/* =====================================================================
 *  Public API
 * ===================================================================== */

void mdec_init(mdec_ctx_t* ctx, int width, int height) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->width  = width;
    ctx->height = height;
    ctx->output_signed = 0;
    build_vlc_lut();
}

int mdec_decode_frame(mdec_ctx_t* ctx,
                      const uint8_t* bytes, size_t n_bytes,
                      uint8_t* rgb_out)
{
    s_desync_logged_this_frame = 0;
    s_frame_index++;
    if (!ctx || !bytes || !rgb_out) {
        TRACE_ERR("[mdec] null arg: ctx=%p bytes=%p rgb=%p\n",
                  (void*)ctx, (void*)bytes, (void*)rgb_out);
        return -1;
    }
    if (n_bytes < 8) {
        TRACE_ERR("[mdec] n_bytes too small: %zu\n", n_bytes);
        return -1;
    }
    build_vlc_lut();

    /* The bitstream is stored as little-endian uint16, but bits within
     * each halfword are MSB-first. Byte-swap pairs once so the bit
     * reader can consume bytes left-to-right. */
    uint8_t* swap = (uint8_t*)malloc(n_bytes);
    if (!swap) return -1;
    size_t pairs = n_bytes / 2;
    for (size_t i = 0; i < pairs; i++) {
        swap[i*2 + 0] = bytes[i*2 + 1];
        swap[i*2 + 1] = bytes[i*2 + 0];
    }
    if (n_bytes & 1) swap[n_bytes - 1] = 0;

    bitreader_t br;
    br_init(&br, swap, n_bytes);

    /* Skip 4 preamble bytes (typically XX XX 00 38). */
    br_get_ubits(&br, 32);

    int qscale = (int)br_get_ubits(&br, 16);
    int version = (int)br_get_ubits(&br, 16);
    {
        static int s_loggedHeader = 0;
        if (s_loggedHeader < 3) {
            TRACE_ERR("[mdec] hdr n_bytes=%zu first8=%02x %02x %02x %02x %02x %02x %02x %02x qscale=%d version=%d ctx=%dx%d\n",
                      n_bytes,
                      swap[0], swap[1], swap[2], swap[3],
                      swap[4], swap[5], swap[6], swap[7],
                      qscale, version, ctx->width, ctx->height);
            s_loggedHeader++;
        }
    }
    if (qscale <= 0 || version > 2) {
        TRACE_ERR("[mdec] bad header: qscale=%d version=%d\n", qscale, version);
        free(swap);
        return -1;
    }

    int mb_w = ctx->width  / 16;
    int mb_h = ctx->height / 16;
    int mb_done = 0;

    /* Column-major macroblock order. */
    for (int mb_x = 0; mb_x < mb_w; mb_x++) {
        for (int mb_y = 0; mb_y < mb_h; mb_y++) {
            int16_t blocks[6][64];
            /* Decode order: Cr, Cb, Y0, Y1, Y2, Y3 */
            static const int decode_order[6] = { 5, 4, 0, 1, 2, 3 };
            for (int i = 0; i < 6; i++) {
                int b = decode_order[i];
                /* Use the same matrix for Y and chroma — matches DuckStation
                 * default and produces visually correct output (separate
                 * chroma matrix tested but made colors more wrong). */
                const uint8_t* qt = s_intra_matrix;
                if (decode_block_intra(&br, blocks[b], qscale, version, qt) < 0) {
                    static int s_loggedBlockFail = 0;
                    if (s_loggedBlockFail < 3) {
                        TRACE_ERR("[mdec] decode_block_intra failed mb=(%d,%d) i=%d b=%d byte_pos=%zu\n",
                                  mb_x, mb_y, i, b, br.byte_pos);
                        s_loggedBlockFail++;
                    }
                    free(swap);
                    return -1;
                }
                idct_block(blocks[b]);
            }

            uint32_t block_rgb[256];
            yuv_to_rgb_tile(0, 0, blocks[5], blocks[4], blocks[0], block_rgb, ctx->output_signed);
            yuv_to_rgb_tile(8, 0, blocks[5], blocks[4], blocks[1], block_rgb, ctx->output_signed);
            yuv_to_rgb_tile(0, 8, blocks[5], blocks[4], blocks[2], block_rgb, ctx->output_signed);
            yuv_to_rgb_tile(8, 8, blocks[5], blocks[4], blocks[3], block_rgb, ctx->output_signed);

            int base_x = mb_x * 16;
            int base_y = mb_y * 16;
            for (int y = 0; y < 16; y++) {
                uint8_t* dst = rgb_out + ((base_y + y) * ctx->width + base_x) * 3;
                for (int x = 0; x < 16; x++) {
                    uint32_t px = block_rgb[x + y * 16];
                    dst[x * 3 + 0] = (uint8_t)( px        & 0xFF);
                    dst[x * 3 + 1] = (uint8_t)((px >> 8 ) & 0xFF);
                    dst[x * 3 + 2] = (uint8_t)((px >> 16) & 0xFF);
                }
            }
            mb_done++;
        }
    }

    free(swap);
    return mb_done;
}
