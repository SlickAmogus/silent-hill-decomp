/* str_demux.c — Silent Hill FMV stream demuxer.
 *
 * Reads consecutive Mode 2 Form 2 data sectors from the disc image at a
 * known file start, parses the per-sector 32-byte CDSECTOR header, and
 * reassembles each frame's MDEC bitstream by concatenating the 2292-byte
 * chunks from the N sectors that share the same frameCount. */

#include "str_demux.h"

#include <string.h>

static inline uint16_t rd_le16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static inline uint32_t rd_le32(const uint8_t* p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8)  |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int read_one_sector(str_stream_t* s, uint8_t* buf /*[2336]*/) {
    if (s->cur_sector >= s->total_sectors) return 0;
    long ofs = (long)(s->base_sector + s->cur_sector) * STR_RAW_SECTOR_BYTES
             + STR_HDR_SKIP_BYTES;
    if (fseek(s->bin, ofs, SEEK_SET) != 0) return 0;
    size_t n = fread(buf, 1, STR_LOGICAL_SECTOR_BYTES, s->bin);
    s->cur_sector++;
    return n == STR_LOGICAL_SECTOR_BYTES;
}

int str_open(str_stream_t* s, FILE* bin, uint32_t base_sector, uint32_t total_sectors) {
    if (!s || !bin) return 0;
    s->bin           = bin;
    s->base_sector   = base_sector;
    s->total_sectors = total_sectors;
    s->cur_sector    = 0;
    s->audio_cb      = 0;
    s->audio_user    = 0;
    return 1;
}

int str_read_frame(str_stream_t* s,
                   uint16_t* bs_out,
                   str_frame_info_t* info_out,
                   size_t* bs_halfwords_out)
{
    if (!s || !bs_out || !info_out || !bs_halfwords_out) return -1;

    uint8_t  sector[STR_LOGICAL_SECTOR_BYTES];
    int      have_info     = 0;
    int      sectors_seen  = 0;
    int      sectors_total = 0;
    uint32_t frame_lock    = 0;
    size_t   bs_byte_pos   = 0;
    size_t   bs_byte_cap   = STR_FRAME_BS_MAX_HALFWORDS * 2;
    uint8_t* bs_bytes      = (uint8_t*)bs_out;

    for (;;) {
        if (!read_one_sector(s, sector)) {
            if (have_info && sectors_seen > 0) {
                *bs_halfwords_out = bs_byte_pos / 2;
                return 1;
            }
            return 0;
        }

        /* FMV files interleave XA audio sectors among the video data
         * sectors. Forward audio sectors to the registered callback (if
         * any) so the player can decode XA-ADPCM in sync with the video,
         * then skip them in the video demux path. */
        uint8_t submode = sector[2];
        if (submode & 0x04) {
            if (s->audio_cb) s->audio_cb(sector, s->audio_user);
            continue;
        }

        const uint8_t* hdr   = sector + STR_SUBHEADER_SIZE;
        const uint8_t* chunk = hdr   + STR_CDSECTOR_HEADER_SIZE;

        /* CDSECTOR header */
        uint16_t id        = rd_le16(hdr + 0x00);
        uint16_t type      = rd_le16(hdr + 0x02);

        /* The disc interleaves null/padding sectors (submode 0x00, zeroed
         * header) roughly every 8 sectors. They carry the audio bit too
         * rarely to be caught above, so guard on the video magic: anything
         * that isn't a real STR video sector is skipped, never parsed as a
         * frame header. Without this a null sector landing mid-frame (e.g.
         * M2 frame 17, the first 9-sector frame) trips the frame-boundary
         * check on frame_no=0 and desyncs secCount → fatal -2. */
        if (id != 0x0160 || type != 0x8001) continue;

        uint16_t secCount  = rd_le16(hdr + 0x04);
        uint16_t nSectors  = rd_le16(hdr + 0x06);
        uint32_t frame_no  = rd_le32(hdr + 0x08);
        uint32_t frame_sz  = rd_le32(hdr + 0x0C);
        uint16_t width     = rd_le16(hdr + 0x10);
        uint16_t height    = rd_le16(hdr + 0x12);

        if (!have_info) {
            info_out->width       = (int)width;
            info_out->height      = (int)height;
            info_out->n_sectors   = (int)nSectors;
            info_out->frame_count = frame_no;
            info_out->frame_size  = frame_sz;
            sectors_total         = (int)nSectors;
            frame_lock            = frame_no;
            have_info             = 1;
        } else if (frame_no != frame_lock) {
            /* Sector belongs to next frame — back up so the caller sees it. */
            s->cur_sector--;
            *bs_halfwords_out = bs_byte_pos / 2;
            return 1;
        }

        /* Sectors should arrive in order; if not, bail.
         * (-2 = secCount mismatch, -3 = bitstream overflow — distinct codes
         * so the [FMVEND] diagnostic can tell which path stopped an FMV.) */
        if ((int)secCount != sectors_seen) {
            s->dbg_secCount    = (int)secCount;
            s->dbg_sectorsSeen = sectors_seen;
            s->dbg_submode     = (int)submode;
            s->dbg_frameNo     = frame_no;
            s->dbg_frameLock   = frame_lock;
            return -2;
        }

        if (bs_byte_pos + STR_VIDEO_CHUNK_BYTES > bs_byte_cap) {
            s->dbg_secCount    = (int)secCount;
            s->dbg_sectorsSeen = sectors_seen;
            s->dbg_submode     = (int)submode;
            s->dbg_frameNo     = frame_no;
            s->dbg_frameLock   = frame_lock;
            return -3;
        }
        memcpy(bs_bytes + bs_byte_pos, chunk, STR_VIDEO_CHUNK_BYTES);
        bs_byte_pos += STR_VIDEO_CHUNK_BYTES;
        sectors_seen++;

        if (sectors_seen >= sectors_total) {
            /* frame_sz is in 16-bit halfwords per per-sector header (verified
             * empirically against C1 — using bytes truncates intra frames mid-
             * macroblock; treating as halfwords feeds the full bitstream). */
            size_t valid_bytes = (size_t)frame_sz * 2u;
            if (valid_bytes > bs_byte_pos) valid_bytes = bs_byte_pos;
            *bs_halfwords_out = valid_bytes / 2;
            return 1;
        }
    }
}
