/*
 * avi_read.c - RIFF/AVI parser: C stdio port of pc_port/src/fmv/ReadAVI.cpp
 * (see avi_read.h for why the C++ original isn't compiled in place).
 *
 * The parse walk is a mechanical translation — same chunk dispatch, same idx1
 * offset math (stored offset + movi_offset + 0x10 = absolute chunk DATA
 * position), same no-index movi-scan fallback — so any AVI the PC port plays
 * parses identically here. Deviations, all safety-only:
 *   - a sticky r->err flag replaces ifstream exceptions (truncated files
 *     terminate the walk instead of looping on a failed stream),
 *   - idx1 entries with unknown ckids are skipped instead of inheriting the
 *     previous entry's type (ReadAVI reused the stale local),
 *   - the vids palette is not stored (ReadAVI read it, then deleted it at the
 *     end of parse_riff; MJPG never has one),
 *   - the chunk read buffer is sized ONCE per movie from the index's largest
 *     entry instead of growing per chunk.
 *
 * ReadAVI lineage: Michael Kohn (2004-2013), olegvedi (2018), BSD 3-Clause —
 * full license text in pc_port/src/fmv/ReadAVI.cpp.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "avi_read.h"

/* --- little-endian primitive reads (ReadAVI::read_int/read_word/read_chars) */

static int avi_read_int(avi_reader_t* r)
{
    unsigned char b[4];
    if (fread(b, 1, 4, r->f) != 4) {
        r->err = 1;
        return 0;
    }
    return (int)((unsigned)b[0] | ((unsigned)b[1] << 8) |
                 ((unsigned)b[2] << 16) | ((unsigned)b[3] << 24));
}

static int avi_read_word(avi_reader_t* r)
{
    unsigned char b[2];
    if (fread(b, 1, 2, r->f) != 2) {
        r->err = 1;
        return 0;
    }
    return (int)((unsigned)b[0] | ((unsigned)b[1] << 8));
}

static void avi_read_chars(avi_reader_t* r, char* s, int count)
{
    if (fread(s, 1, (size_t)count, r->f) != (size_t)count)
        r->err = 1;
    s[count] = 0;
}

static long avi_tell(avi_reader_t* r)
{
    return ftell(r->f);
}

static void avi_seek(avi_reader_t* r, long off)
{
    if (off < 0 || fseek(r->f, off, SEEK_SET) != 0)
        r->err = 1;
}

/* --- index ------------------------------------------------------------------*/

static void avi_push_index(avi_reader_t* r, unsigned char type, int stream_num,
                           unsigned offset, unsigned length)
{
    avi_index_entry_t* e;

    if (r->index_count == r->index_cap) {
        unsigned ncap = r->index_cap ? r->index_cap * 2u : 1024u;
        void*    p    = realloc(r->index, ncap * sizeof(avi_index_entry_t));
        if (!p) {
            r->err = 1;
            return;
        }
        r->index     = (avi_index_entry_t*)p;
        r->index_cap = ncap;
    }
    e             = &r->index[r->index_count++];
    e->type       = type;
    e->stream_num = (unsigned char)stream_num;
    e->offset     = offset;
    e->length     = length;
    if (length > r->max_chunk)
        r->max_chunk = length;
}

/* "NNtt" ckid -> stream number + chunk type (ReadAVI::decodeCkid). */
static int avi_decode_ckid(const char* ckid, unsigned char* out_type)
{
    int stream = (ckid[0] - '0') * 10 + (ckid[1] - '0');

    if (ckid[2] == 'd' && ckid[3] == 'b') { *out_type = AVI_CTYPE_UNCOMPRESSED; return stream; }
    if (ckid[2] == 'd' && ckid[3] == 'c') { *out_type = AVI_CTYPE_COMPRESSED;   return stream; }
    if (ckid[2] == 'p' && ckid[3] == 'c') { *out_type = AVI_CTYPE_PALETTE;      return stream; }
    if (ckid[2] == 'w' && ckid[3] == 'b') { *out_type = AVI_CTYPE_AUDIO;        return stream; }
    return -1;
}

/* --- chunk parsers (1:1 with the ReadAVI methods of the same names) ---------*/

static void avi_parse_idx1(avi_reader_t* r, int chunk_len)
{
    int t, n = chunk_len / 16;

    for (t = 0; t < n && !r->err; t++) {
        char          buf[5];
        unsigned char type = AVI_CTYPE_NONE;
        int           stream_num, offset, length;

        avi_read_chars(r, buf, 4);
        stream_num = avi_decode_ckid(buf, &type);
        (void)avi_read_int(r);                                     /* dwFlags */
        offset = avi_read_int(r) + (int)r->movi_offset + 0x10;
        length = avi_read_int(r);
        if (r->err)
            break;
        if (stream_num >= 0 && type != AVI_CTYPE_NONE)
            avi_push_index(r, type, stream_num, (unsigned)offset, (unsigned)length);
    }
}

static void avi_read_avi_header(avi_reader_t* r)
{
    r->hdr.TimeBetweenFrames     = avi_read_int(r);
    r->hdr.MaximumDataRate       = avi_read_int(r);
    r->hdr.PaddingGranularity    = avi_read_int(r);
    r->hdr.Flags                 = avi_read_int(r);
    r->hdr.TotalNumberOfFrames   = avi_read_int(r);
    r->hdr.NumberOfInitialFrames = avi_read_int(r);
    r->hdr.NumberOfStreams       = avi_read_int(r);
    r->hdr.SuggestedBufferSize   = avi_read_int(r);
    r->hdr.Width                 = avi_read_int(r);
    r->hdr.Height                = avi_read_int(r);
    r->hdr.TimeScale             = avi_read_int(r);
    r->hdr.DataRate              = avi_read_int(r);
    r->hdr.StartTime             = avi_read_int(r);
    r->hdr.DataLength            = avi_read_int(r);
}

static void avi_read_stream_header(avi_reader_t* r, avi_stream_header_t* sh)
{
    avi_read_chars(r, sh->DataType, 4);
    avi_read_chars(r, sh->DataHandler, 4);
    sh->Flags               = avi_read_int(r);
    sh->Priority            = avi_read_int(r);
    sh->InitialFrames       = avi_read_int(r);
    sh->TimeScale           = avi_read_int(r);
    sh->DataRate            = avi_read_int(r);
    sh->StartTime           = avi_read_int(r);
    sh->DataLength          = avi_read_int(r);
    sh->SuggestedBufferSize = avi_read_int(r);
    sh->Quality             = avi_read_int(r);
    sh->SampleSize          = avi_read_int(r);
}

static void avi_read_stream_format(avi_reader_t* r)
{
    r->vid.header_size         = avi_read_int(r);
    r->vid.image_width         = avi_read_int(r);
    r->vid.image_height        = avi_read_int(r);
    r->vid.number_of_planes    = avi_read_word(r);
    r->vid.bits_per_pixel      = avi_read_word(r);
    avi_read_chars(r, r->vid.compression_type, 4);
    r->vid.image_size_in_bytes = avi_read_int(r);
    r->vid.x_pels_per_meter    = avi_read_int(r);
    r->vid.y_pels_per_meter    = avi_read_int(r);
    r->vid.colors_used         = avi_read_int(r);
    r->vid.colors_important    = avi_read_int(r);
    /* Palette (colors_important RGB triples) deliberately not read — the
     * caller seeks to the next chunk regardless, and MJPG has no palette. */
}

static void avi_read_stream_format_auds(avi_reader_t* r)
{
    r->auds.format             = avi_read_word(r);   /* wFormatTag */
    r->auds.channels           = avi_read_word(r);   /* nChannels */
    r->auds.samples_per_second = avi_read_int(r);    /* nSamplesPerSec */
    r->auds.bytes_per_second   = avi_read_int(r);    /* nAvgBytesPerSec */
    r->auds.block_size_of_data = avi_read_word(r);   /* nBlockAlign */
    r->auds.bits_per_sample    = avi_read_word(r);   /* wBitsPerSample */
}

static void avi_parse_hdrl_list(avi_reader_t* r)
{
    char chunk_id[5];
    int  chunk_size;
    char chunk_type[5];
    long end_of_chunk, next_chunk;
    int  stream_type = 0;

    avi_read_chars(r, chunk_id, 4);
    chunk_size = avi_read_int(r);
    avi_read_chars(r, chunk_type, 4);
    if (r->err)
        return;

    end_of_chunk = chunk_size - 4 + avi_tell(r);

    if (strcmp(chunk_id, "JUNK") == 0) {
        avi_seek(r, end_of_chunk);
        return;
    }

    while (!r->err && avi_tell(r) < end_of_chunk) {
        avi_read_chars(r, chunk_type, 4);
        chunk_size = avi_read_int(r);
        next_chunk = chunk_size + avi_tell(r);

        if (strcmp("strh", chunk_type) == 0) {
            long marker = avi_tell(r);
            char buffer[5];
            avi_read_chars(r, buffer, 4);
            avi_seek(r, marker);

            if (strcmp(buffer, "vids") == 0) {
                stream_type = 0;
                avi_read_stream_header(r, &r->strh_vid);
            } else if (strcmp(buffer, "auds") == 0) {
                stream_type = 1;
                avi_read_stream_header(r, &r->strh_auds);
            } else {
                return;   /* unknown stream kind — bail like ReadAVI's -1 */
            }
        } else if (strcmp("strf", chunk_type) == 0) {
            if (stream_type == 0)
                avi_read_stream_format(r);
            else
                avi_read_stream_format_auds(r);
        }

        avi_seek(r, next_chunk);
    }

    avi_seek(r, end_of_chunk);
}

static void avi_parse_hdrl(avi_reader_t* r, unsigned size)
{
    char chunk_id[5];
    long offset = avi_tell(r);

    avi_read_chars(r, chunk_id, 4);   /* "avih" */
    (void)avi_read_int(r);            /* avih size (56) — header read is fixed-size */
    avi_read_avi_header(r);

    while (!r->err && avi_tell(r) < offset + (long)size - 4)
        avi_parse_hdrl_list(r);
}

static void avi_parse_movi(avi_reader_t* r, int size)
{
    char chunk_id[5];

    do {
        long          offset = avi_tell(r);
        unsigned char type   = AVI_CTYPE_NONE;
        int           stream_num, chunk_len;
        long          end_of_chunk;

        avi_read_chars(r, chunk_id, 4);
        stream_num = avi_decode_ckid(chunk_id, &type);
        if (r->err)
            break;

        if (stream_num < 0) {
            avi_seek(r, offset - 4);
            break;
        }

        chunk_len = avi_read_int(r);

        if (!(r->hdr.Flags & AVI_FLAG_HASINDEX))
            avi_push_index(r, type, stream_num, (unsigned)(offset + 8), (unsigned)chunk_len);

        end_of_chunk = chunk_len + avi_tell(r);
        end_of_chunk = (end_of_chunk + 1) & ~1L;   /* chunks are word-aligned */
        avi_seek(r, end_of_chunk);

        size -= (int)(end_of_chunk - offset);
    } while (size > 7 && !r->err);
}

static int avi_parse_riff(avi_reader_t* r)
{
    char chunk_id[5];
    int  chunk_size;
    char chunk_type[5];
    long end_of_chunk, end_of_subchunk;

    avi_read_chars(r, chunk_id, 4);
    chunk_size = avi_read_int(r);
    avi_read_chars(r, chunk_type, 4);

    if (r->err || strcmp("RIFF", chunk_id) != 0 || strcmp("AVI ", chunk_type) != 0)
        return 1;

    end_of_chunk = chunk_size - 4 + avi_tell(r);

    while (!r->err && avi_tell(r) < end_of_chunk) {
        long offset = avi_tell(r);

        avi_read_chars(r, chunk_id, 4);
        chunk_size      = avi_read_int(r);
        end_of_subchunk = chunk_size + avi_tell(r);

        if (strcmp("JUNK", chunk_id) == 0 || strcmp("PAD ", chunk_id) == 0) {
            chunk_type[0] = 0;
        } else {
            avi_read_chars(r, chunk_type, 4);
        }
        if (r->err)
            break;

        if (strcmp("JUNK", chunk_id) == 0 || strcmp("PAD ", chunk_id) == 0) {
            /* skip */
        } else if (strcmp("INFO", chunk_type) == 0) {
            /* skip */
        } else if (strcmp("hdrl", chunk_type) == 0) {
            avi_parse_hdrl(r, (unsigned)chunk_size);
        } else if (strcmp("movi", chunk_type) == 0) {
            r->movi_offset = offset;
            avi_parse_movi(r, chunk_size);
        } else if (strcmp("idx1", chunk_id) == 0) {
            /* The 4 bytes just consumed as chunk_type are idx1 payload. */
            avi_seek(r, avi_tell(r) - 4);
            avi_parse_idx1(r, chunk_size);
        } else {
            if (chunk_size == 0)
                break;
        }

        avi_seek(r, end_of_subchunk);
    }

    return 0;
}

/* --- public API --------------------------------------------------------------*/

int Avi_Open(avi_reader_t* r, const char* path)
{
    memset(r, 0, sizeof(*r));

    r->f = fopen(path, "rb");
    if (!r->f)
        return 0;

    if (fseek(r->f, 0, SEEK_END) != 0) {
        Avi_Close(r);
        return 0;
    }
    r->file_size = ftell(r->f);
    if (r->file_size <= 0 || fseek(r->f, 0, SEEK_SET) != 0) {
        Avi_Close(r);
        return 0;
    }

    avi_parse_riff(r);

    /* A truncated tail leaves the entries parsed so far usable (matches the
     * C++ version's catch(...) keeping a non-empty index) — only a fully
     * unusable file (no index at all) is rejected. */
    if (r->index_count == 0) {
        Avi_Close(r);
        return 0;
    }

    r->chunk_cap = r->max_chunk + 1;
    r->chunk_buf = (unsigned char*)malloc(r->chunk_cap);
    if (!r->chunk_buf) {
        Avi_Close(r);
        return 0;
    }

    r->err = 0;
    return 1;
}

void Avi_Close(avi_reader_t* r)
{
    if (r->f)
        fclose(r->f);
    free(r->index);
    free(r->chunk_buf);
    memset(r, 0, sizeof(*r));
}

int Avi_NextChunk(avi_reader_t* r, unsigned type_mask, unsigned* cursor,
                  int* out_type, unsigned char** out_buf)
{
    unsigned i;

    if (!r->f || !r->chunk_buf)
        return -1;

    for (i = *cursor; i < r->index_count; i++) {
        const avi_index_entry_t* e = &r->index[i];

        if ((e->type & type_mask) == 0)
            continue;
        if (e->length > r->chunk_cap)
            return -1;   /* unreachable: chunk_cap covers the index maximum */
        if (fseek(r->f, (long)e->offset, SEEK_SET) != 0)
            return -1;
        if (fread(r->chunk_buf, 1, e->length, r->f) != e->length)
            return -1;

        *out_type = e->type;
        *out_buf  = r->chunk_buf;
        *cursor   = i + 1;
        return (int)e->length;
    }
    return -1;
}
