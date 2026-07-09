/*
 * avi_read.h - RIFF/AVI parser (C port of pc_port/src/fmv/ReadAVI.{h,cpp})
 *
 * Same structs/semantics as the PC ReadAVI class so the SAME modding-community
 * MJPG+PCM AVI files behave identically, but plain C + stdio: nxdk's libcxx
 * iostreams/exceptions are unverified on this target (std::filesystem is
 * known-broken) and this build never links libcxx (NXDK_CXX unset — the one
 * C++ TU, PsyX_GTE.cpp, is freestanding). ReadAVI.cpp needs <fstream>,
 * <vector> AND try/catch, so the C port is the safe, self-contained choice.
 *
 * ReadAVI lineage: Michael Kohn (2004-2013), olegvedi (2018), BSD 3-Clause —
 * see pc_port/src/fmv/ReadAVI.cpp for the full license text.
 */
#ifndef AVI_READ_H
#define AVI_READ_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Chunk type bits (mirror of ReadAVI::chunk_type_t). */
#define AVI_CTYPE_NONE          0
#define AVI_CTYPE_UNCOMPRESSED  1   /* ##db */
#define AVI_CTYPE_COMPRESSED    2   /* ##dc */
#define AVI_CTYPE_PALETTE       4   /* ##pc */
#define AVI_CTYPE_AUDIO         8   /* ##wb */
#define AVI_CTYPE_VIDEO         (AVI_CTYPE_UNCOMPRESSED | AVI_CTYPE_COMPRESSED)

#define AVI_FLAG_HASINDEX       0x10

typedef struct {                    /* 'avih' — ReadAVI::avi_header_t */
    int TimeBetweenFrames;          /* microseconds per video frame */
    int MaximumDataRate;
    int PaddingGranularity;
    int Flags;
    int TotalNumberOfFrames;
    int NumberOfInitialFrames;
    int NumberOfStreams;
    int SuggestedBufferSize;
    int Width;
    int Height;
    int TimeScale;
    int DataRate;
    int StartTime;
    int DataLength;
} avi_header_t;

typedef struct {                    /* 'strh' — ReadAVI::stream_header_t */
    char DataType[5];
    char DataHandler[5];
    int  Flags;
    int  Priority;
    int  InitialFrames;
    int  TimeScale;
    int  DataRate;
    int  StartTime;
    int  DataLength;
    int  SuggestedBufferSize;
    int  Quality;
    int  SampleSize;
} avi_stream_header_t;

typedef struct {                    /* 'strf' vids — ReadAVI::stream_format_t */
    int  header_size;
    int  image_width;
    int  image_height;
    int  number_of_planes;
    int  bits_per_pixel;
    char compression_type[5];       /* e.g. "MJPG" */
    int  image_size_in_bytes;
    int  x_pels_per_meter;
    int  y_pels_per_meter;
    int  colors_used;
    int  colors_important;
} avi_stream_format_t;

typedef struct {                    /* 'strf' auds — WAVEFORMATEX prefix */
    int format;                     /* wFormatTag: 1 = PCM */
    int channels;
    int samples_per_second;
    int bytes_per_second;
    int block_size_of_data;         /* nBlockAlign */
    int bits_per_sample;
} avi_stream_format_auds_t;

typedef struct {
    unsigned char type;             /* AVI_CTYPE_* */
    unsigned char stream_num;
    unsigned int  offset;           /* absolute file offset of the chunk DATA */
    unsigned int  length;           /* chunk data bytes */
} avi_index_entry_t;

typedef struct {
    FILE* f;
    long  file_size;
    long  movi_offset;              /* offset of the movi LIST fourcc */
    int   err;                      /* sticky read/seek/alloc failure flag */

    avi_header_t             hdr;
    avi_stream_header_t      strh_vid;
    avi_stream_header_t      strh_auds;
    avi_stream_format_t      vid;
    avi_stream_format_auds_t auds;

    avi_index_entry_t* index;       /* idx1 (or movi-scan fallback) entries */
    unsigned index_count;
    unsigned index_cap;
    unsigned max_chunk;             /* largest chunk length seen in the index */

    unsigned char* chunk_buf;       /* single read buffer, max_chunk bytes,   */
    unsigned       chunk_cap;       /* allocated ONCE per movie at Avi_Open   */
} avi_reader_t;

/* Open + parse. Returns 1 when usable (headers parsed, index non-empty,
 * chunk buffer allocated), 0 otherwise (reader fully cleaned up). */
int  Avi_Open(avi_reader_t* r, const char* path);
void Avi_Close(avi_reader_t* r);

/* Read the next chunk at/after *cursor whose type is in type_mask, in file
 * (index) order — the mirror of ReadAVI::GetFrameFromIndex. Returns the data
 * length (>= 0), fills *out_type / *out_buf (points into r->chunk_buf, valid
 * until the next call), and advances *cursor past the entry. Returns -1 when
 * no further matching chunk exists or on read failure. */
int  Avi_NextChunk(avi_reader_t* r, unsigned type_mask, unsigned* cursor,
                   int* out_type, unsigned char** out_buf);

#ifdef __cplusplus
}
#endif

#endif /* AVI_READ_H */
