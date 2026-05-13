/* str_demux.h — Silent Hill FMV stream demuxer.
 *
 * Silent Hill stores its FMV files (C1, C2, M1..ME, Z1..ZZ) inside the
 * disc image as data sectors (Mode 2 Form 2, submode 0x48) rather than
 * using the standard PSX "video" subheader bit. Each video sector has a
 * 32-byte CDSECTOR header (see stream.h) before the MDEC bitstream chunk.
 *
 * Per logical 2336-byte sector (after skipping 16-byte sync from the
 * 2352-byte raw sector):
 *   0..7     subheader (file/ch/submode/coding, ×2 ECC copies)
 *   8..39    CDSECTOR header (32 bytes)
 *   40..2331 MDEC bitstream chunk (2292 bytes)
 *   2332..2335 EDC
 *
 * CDSECTOR header layout (little-endian, 32 bytes):
 *   0x00  u16 id           — 0x0160
 *   0x02  u16 type         — 0x8001
 *   0x04  u16 secCount     — index of this sector within current frame
 *   0x06  u16 nSectors     — total sectors per frame
 *   0x08  u32 frameCount   — frame number (1-based in this game)
 *   0x0C  u32 frameSize    — total bytes of MDEC bitstream for this frame
 *   0x10  u16 width        — pixels
 *   0x12  u16 height       — pixels
 *   0x14  u32 headm        — magic / per-frame info
 *   0x18  u32 headv        — magic / per-frame info
 *   0x1C  u32 user
 *
 * The demuxer reads consecutive sectors from a base offset in the BIN and
 * reassembles each frame's bitstream by concatenating the `nSectors` chunks
 * that share the same frameCount.
 */
#ifndef STR_DEMUX_H
#define STR_DEMUX_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int      width;
    int      height;
    int      n_sectors;     /* sectors per frame */
    uint32_t frame_count;
    uint32_t frame_size;    /* total MDEC bitstream bytes for this frame */
} str_frame_info_t;

typedef struct {
    FILE*    bin;
    uint32_t base_sector;
    uint32_t total_sectors;
    uint32_t cur_sector;
} str_stream_t;

#define STR_FRAME_BS_MAX_HALFWORDS 16384

#define STR_RAW_SECTOR_BYTES       2352
#define STR_HDR_SKIP_BYTES           16
#define STR_LOGICAL_SECTOR_BYTES   2336
#define STR_SUBHEADER_SIZE            8
#define STR_CDSECTOR_HEADER_SIZE     32
#define STR_VIDEO_CHUNK_BYTES       (STR_LOGICAL_SECTOR_BYTES - STR_SUBHEADER_SIZE - STR_CDSECTOR_HEADER_SIZE - 4)
/* = 2336 - 8 - 32 - 4 (EDC) = 2292 */

int str_open(str_stream_t* s, FILE* bin, uint32_t base_sector, uint32_t total_sectors);

/* Read the next complete frame. Returns 1 = success, 0 = EOF, -1 = error. */
int str_read_frame(str_stream_t* s,
                   uint16_t* bs_out,
                   str_frame_info_t* info_out,
                   size_t* bs_halfwords_out);

#ifdef __cplusplus
}
#endif

#endif /* STR_DEMUX_H */
