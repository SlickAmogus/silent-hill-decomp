/*
 * ReadAVI.h - AVI file parser
 *
 * Based on REDRIVER2 implementation by olegvedi@gmail.com (2018)
 * Original copyright: Michael Kohn <mike@mikekohn.net> (2004-2013)
 *
 * BSD 3-Clause License - see ReadAVI.cpp for full license text.
 *
 * PC port extensions: 64-bit file offsets, OpenDML (RIFF AVIX) multi-segment
 * files, index built by scanning the movi lists (idx1 is ignored — see
 * ReadAVI.cpp), WAVE_FORMAT_EXTENSIBLE resolution. Removes every 2 GB / 4 GB
 * size ceiling the original 32-bit parser had.
 */
#ifndef READAVI_H_
#define READAVI_H_

#include <fstream>
#include <vector>
#include <stdint.h>

class ReadAVI {
    enum {
        AVIF_HASINDEX = 0x10,
        AVIF_MUSTUSEINDEX = 0x20,
        AVIF_ISINTERLEAVED = 0x100,
    };

public:
    typedef struct {
        int TimeBetweenFrames;
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

    typedef struct {
        char DataType[5];
        char DataHandler[5];
        int Flags;
        int Priority;
        int InitialFrames;
        int TimeScale;
        int DataRate;
        int StartTime;
        int DataLength;
        int SuggestedBufferSize;
        int Quality;
        int SampleSize;
    } stream_header_t;

    typedef struct {
        int header_size;
        int image_width;
        int image_height;      /* negative = top-down DIB */
        int number_of_planes;
        int bits_per_pixel;
        char compression_type[5];
        int image_size_in_bytes;
        int x_pels_per_meter;
        int y_pels_per_meter;
        int colors_used;
        int colors_important;
        int* palette;
    } stream_format_t;

    typedef struct {
        int header_size;
        int format;            /* wFormatTag; WAVE_FORMAT_EXTENSIBLE resolved to its SubFormat tag */
        int channels;
        int samples_per_second;
        int bytes_per_second;
        int block_size_of_data;
        int bits_per_sample;
        int extended_size;
    } stream_format_auds_t;

#define ChunkTypesCnt 4
    typedef enum {
        ctype_none = 0,
        ctype_uncompressed_video_frame = 1,
        ctype_compressed_video_frame = 2,
        ctype_palette_change = 4,
        ctype_audio_data = 8,
        ctype_video_data = 1 + 2,
    } chunk_type_t;

    ReadAVI(const char* filename);
    virtual ~ReadAVI();

    bool IsOpen() const { return inFile.is_open() && index_entries.size() > 0; }

    avi_header_t GetAviHeader() { return avi_header; }
    stream_format_t GetVideoFormat() { return stream_format_vid; }
    stream_format_auds_t GetAudioFormat() { return stream_format_auds; }
    unsigned GetIndexCnt() { return (unsigned)index_entries.size(); }

    typedef struct {
        chunk_type_t type;
        unsigned stream_num;
        unsigned char* buf;
        unsigned pointer;
    } frame_entry_t;

    int GetFrameFromIndex(frame_entry_t* frame_entry);

private:
    typedef struct {
        chunk_type_t type;
        int stream_num;
        int64_t dwChunkOffset;   /* absolute file offset of the chunk payload */
        uint32_t dwChunkLength;
    } index_entry_t;

    typedef struct {
        char type_id[3];
        chunk_type_t type;
    } chunk_type_int_t;

    static chunk_type_int_t chunk_types[ChunkTypesCnt];

    std::vector<index_entry_t> index_entries;
    std::ifstream inFile;
    avi_header_t avi_header;
    stream_header_t stream_header_vid;
    stream_format_t stream_format_vid;
    stream_header_t stream_header_auds;
    stream_format_auds_t stream_format_auds;
    int64_t fileSize;
    unsigned char* data_buf;
    unsigned data_buf_size;
    bool headers_parsed;

    int parse_riff_segment(int64_t segment_end, bool first_segment);
    int parse_hdrl_list();
    int read_avi_header();
    int read_stream_header(stream_header_t* sheader);
    int read_stream_format();
    int read_stream_format_auds(uint32_t strf_size);
    int parse_hdrl(unsigned int size);
    int parse_movi(int64_t list_end);
    int decodeCkid(char* ckid, chunk_type_t* chunk_type);
    uint32_t read_uint();
    int read_int();
    int read_word();
    void read_chars(char* s, int count);
    void read_chars_bin(unsigned char* s, int count);
    void check_data_buf(unsigned size);
    void free();
};

#endif /* READAVI_H_ */
