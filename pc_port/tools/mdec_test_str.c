/* mdec_test_str.c — end-to-end STR → MDEC → PPM tester.
 *
 * Reads N raw 2352-byte sectors from a disc image starting at a given
 * disc-absolute sector, demuxes one MDEC frame, decodes it, and writes
 * a PPM. Use this to validate decoder output against a known STR file
 * on the Silent Hill BIN.
 *
 * Usage:
 *   mdec_test_str <bin_path> <base_sector> <num_sectors> [<frame_idx>] <output.ppm>
 *
 * Build (standalone):
 *   cc -O2 -o mdec_test_str mdec_test_str.c \
 *       ../src/fmv/mdec.c ../src/fmv/str_demux.c
 */

#include "../src/fmv/mdec.h"
#include "../src/fmv/str_demux.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int usage(const char* a0) {
    fprintf(stderr,
        "usage: %s <bin_path> <base_sector> <num_sectors> [<frame_idx>] <output.ppm>\n"
        "  bin_path     CD image (.bin)\n"
        "  base_sector  disc-absolute sector where the STR file starts\n"
        "  num_sectors  number of sectors in the STR file\n"
        "  frame_idx    optional 0-based frame to decode (default 0)\n"
        "  output.ppm   output P6 image\n",
        a0);
    return 1;
}

int main(int argc, char** argv) {
    if (argc != 5 && argc != 6) return usage(argv[0]);
    const char* bin_path     = argv[1];
    uint32_t    base_sector  = (uint32_t)strtoul(argv[2], NULL, 0);
    uint32_t    num_sectors  = (uint32_t)strtoul(argv[3], NULL, 0);
    uint32_t    frame_idx    = (argc == 6) ? (uint32_t)strtoul(argv[4], NULL, 0) : 0;
    const char* out_path     = argv[(argc == 6) ? 5 : 4];

    FILE* bin = fopen(bin_path, "rb");
    if (!bin) { perror(bin_path); return 1; }

    str_stream_t s;
    if (!str_open(&s, bin, base_sector, num_sectors)) {
        fprintf(stderr, "str_open failed\n");
        fclose(bin);
        return 1;
    }

    uint16_t bs[STR_FRAME_BS_MAX_HALFWORDS];
    str_frame_info_t info;
    size_t bs_halfwords;

    /* Skip frame_idx frames, then decode the next one. */
    for (uint32_t i = 0; i <= frame_idx; i++) {
        int r = str_read_frame(&s, bs, &info, &bs_halfwords);
        if (r <= 0) {
            fprintf(stderr,
                "[mdec_test_str] str_read_frame returned %d at frame %u (wanted %u)\n",
                r, i, frame_idx);
            fclose(bin);
            return 1;
        }
        if (i < frame_idx) {
            /* Skip this frame's bitstream; loop reads the next one. */
            continue;
        }
        fprintf(stderr,
            "[mdec_test_str] frame %u: %dx%d, %d sectors, frame_size=%u, %zu halfwords\n",
            info.frame_count, info.width, info.height,
            info.n_sectors, info.frame_size, bs_halfwords);
    }

    mdec_ctx_t ctx;
    mdec_init(&ctx, info.width, info.height);

    size_t rgb_bytes = (size_t)info.width * (size_t)info.height * 3u;
    uint8_t* rgb = (uint8_t*)calloc(1, rgb_bytes);
    if (!rgb) { perror("malloc rgb"); fclose(bin); return 1; }

    /* mdec_decode_frame takes raw bytes (the VLC bitstream). bs is a
     * uint16_t array but we treat it as bytes — bs_halfwords * 2 = bytes. */
    int mb_done = mdec_decode_frame(&ctx,
                                    (const uint8_t*)bs,
                                    bs_halfwords * 2,
                                    rgb);
    fprintf(stderr, "[mdec_test_str] decoded %d macroblocks\n", mb_done);

    FILE* fout = fopen(out_path, "wb");
    if (!fout) { perror(out_path); free(rgb); fclose(bin); return 1; }
    fprintf(fout, "P6\n%d %d\n255\n", info.width, info.height);
    fwrite(rgb, 1, rgb_bytes, fout);
    fclose(fout);
    fprintf(stderr, "[mdec_test_str] wrote %s\n", out_path);

    free(rgb);
    fclose(bin);
    return 0;
}
