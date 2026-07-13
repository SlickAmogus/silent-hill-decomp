/*
 * ReadAVI.cpp - AVI file parser
 *
 * Copyright (c) 2004-2013, Michael Kohn <mike@mikekohn.net> http://www.mikekohn.net/
 * Copyright (c) 2018, olegvedi@gmail.com (C++ implementation and adds)
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of the author nor the names of its contributors may be
 *   used to endorse or promote products derived from this software without
 *   specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * PC port rework (fan FMV-replacement mods hit every one of these):
 *  - All file offsets are 64-bit; nothing truncates past 2 GB / 4 GB.
 *  - OpenDML (AVI 2.0): after the first `RIFF AVI ` segment, any number of
 *    `RIFF AVIX` extension segments are walked for their movi data — this is
 *    how encoders (ffmpeg included) lay out files bigger than ~1-2 GB.
 *  - The frame index is built by SCANNING the movi lists, never from `idx1`:
 *    idx1 offsets are movi-relative from some writers and absolute from
 *    others (a classic incompatibility), it does not cover AVIX segments,
 *    and it does not exist at all on some files. A linear scan has none of
 *    those problems and yields file-order playback, which is all the player
 *    needs. Unknown chunks inside movi (OpenDML `ix##` sub-indexes, JUNK,
 *    `rec ` grouping LISTs) are skipped/descended instead of aborting the
 *    scan — aborting was why big ffmpeg files played a few seconds and cut.
 *  - WAVE_FORMAT_EXTENSIBLE audio (ffmpeg emits it for >2ch or float PCM) is
 *    resolved to its real SubFormat tag.
 */

#include "ReadAVI.h"
#include <string.h>

#define ZEROIZE(x) {memset(&x, 0, sizeof(x));}

using namespace std;

ReadAVI::chunk_type_int_t ReadAVI::chunk_types[ChunkTypesCnt] = {
    {"db", ctype_uncompressed_video_frame},
    {"dc", ctype_compressed_video_frame},
    {"pc", ctype_palette_change},
    {"wb", ctype_audio_data}
};

ReadAVI::ReadAVI(const char* filename)
{
    data_buf = NULL;
    stream_format_vid.palette = NULL;
    data_buf_size = 0;
    fileSize = 0;
    headers_parsed = false;

    inFile.open(filename, ios_base::in | ios_base::binary);
    if (!inFile.is_open())
        return;

    inFile.seekg(0, ios_base::end);
    fileSize = (int64_t)inFile.tellg();
    inFile.seekg(0, ios_base::beg);

    try {
        /* Walk every top-level RIFF segment: `AVI ` first, then any number
         * of OpenDML `AVIX` extensions. Segment sizes are 32-bit, but their
         * POSITIONS are not — always resume from the real 64-bit offset. */
        int64_t pos = 0;
        bool first = true;

        while (pos + 12 <= fileSize) {
            char id[5], type[5];

            inFile.clear();
            inFile.seekg((streamoff)pos, ios_base::beg);
            read_chars(id, 4);
            uint32_t size = read_uint();
            read_chars(type, 4);

            if (strcmp(id, "RIFF") != 0)
                break;
            if (first && strcmp(type, "AVI ") != 0)
                break;
            if (!first && strcmp(type, "AVIX") != 0)
                break;

            int64_t seg_end = pos + 8 + (int64_t)size;
            if (seg_end > fileSize)
                seg_end = fileSize; /* tolerate truncated writers */

            parse_riff_segment(seg_end, first);

            first = false;
            pos = seg_end + (seg_end & 1); /* RIFF segments are word-aligned */
        }
    }
    catch (...) {
        if (index_entries.size() == 0) {
            free();
        }
    }
}

ReadAVI::~ReadAVI()
{
    free();
}

void ReadAVI::free()
{
    if (inFile.is_open())
        inFile.close();
    delete[] stream_format_vid.palette;
    stream_format_vid.palette = NULL;
    delete[] data_buf;
    data_buf = NULL;
}

void ReadAVI::check_data_buf(unsigned size)
{
    if (data_buf_size < size) {
        delete[] data_buf;
        data_buf = new unsigned char[size + 1];
        data_buf_size = size;
    }
}

int ReadAVI::GetFrameFromIndex(frame_entry_t* frame_entry)
{
    if (frame_entry->pointer >= index_entries.size())
        return -5;

    for (unsigned i = frame_entry->pointer; i < index_entries.size(); i++) {
        if (index_entries[i].type & frame_entry->type) {
            check_data_buf(index_entries[i].dwChunkLength);
            try {
                inFile.clear();
                inFile.seekg((streamoff)index_entries[i].dwChunkOffset, ios_base::beg);
                read_chars_bin(data_buf, index_entries[i].dwChunkLength);
            }
            catch (...) {
                return -1;
            }
            frame_entry->buf = data_buf;
            frame_entry->pointer = i + 1;
            frame_entry->stream_num = index_entries[i].stream_num;
            frame_entry->type = index_entries[i].type;
            return (int)index_entries[i].dwChunkLength;
        }
    }
    return -1;
}

int ReadAVI::decodeCkid(char* ckid, chunk_type_t* chunk_type)
{
    if (ckid[0] < '0' || ckid[0] > '9' || ckid[1] < '0' || ckid[1] > '9')
        return -1;

    int stream = (ckid[0] - '0') * 10 + (ckid[1] - '0');
    for (int i = 0; i < ChunkTypesCnt; i++) {
        if (chunk_types[i].type_id[0] == ckid[2] && chunk_types[i].type_id[1] == ckid[3]) {
            *chunk_type = chunk_types[i].type;
            return stream;
        }
    }
    return -1;
}

int ReadAVI::read_avi_header()
{
    avi_header.TimeBetweenFrames = read_int();
    avi_header.MaximumDataRate = read_int();
    avi_header.PaddingGranularity = read_int();
    avi_header.Flags = read_int();
    avi_header.TotalNumberOfFrames = read_int();
    avi_header.NumberOfInitialFrames = read_int();
    avi_header.NumberOfStreams = read_int();
    avi_header.SuggestedBufferSize = read_int();
    avi_header.Width = read_int();
    avi_header.Height = read_int();
    avi_header.TimeScale = read_int();
    avi_header.DataRate = read_int();
    avi_header.StartTime = read_int();
    avi_header.DataLength = read_int();
    return 0;
}

int ReadAVI::read_stream_header(stream_header_t* sheader)
{
    read_chars(sheader->DataType, 4);
    read_chars(sheader->DataHandler, 4);
    sheader->Flags = read_int();
    sheader->Priority = read_int();
    sheader->InitialFrames = read_int();
    sheader->TimeScale = read_int();
    sheader->DataRate = read_int();
    sheader->StartTime = read_int();
    sheader->DataLength = read_int();
    sheader->SuggestedBufferSize = read_int();
    sheader->Quality = read_int();
    sheader->SampleSize = read_int();
    return 0;
}

int ReadAVI::read_stream_format()
{
    stream_format_vid.header_size = read_int();
    stream_format_vid.image_width = read_int();
    stream_format_vid.image_height = read_int();
    stream_format_vid.number_of_planes = read_word();
    stream_format_vid.bits_per_pixel = read_word();
    read_chars(stream_format_vid.compression_type, 4);
    stream_format_vid.image_size_in_bytes = read_int();
    stream_format_vid.x_pels_per_meter = read_int();
    stream_format_vid.y_pels_per_meter = read_int();
    stream_format_vid.colors_used = read_int();
    stream_format_vid.colors_important = read_int();
    stream_format_vid.palette = 0;

    if (stream_format_vid.colors_important != 0 &&
        stream_format_vid.colors_important <= 256) {
        stream_format_vid.palette = new int[stream_format_vid.colors_important];
        for (int t = 0; t < stream_format_vid.colors_important; t++) {
            unsigned char buf[3];
            read_chars_bin(buf, 3);
            int b = buf[0], g = buf[1], r = buf[2];
            stream_format_vid.palette[t] = (r << 16) + (g << 8) + b;
        }
    }
    return 0;
}

int ReadAVI::read_stream_format_auds(uint32_t strf_size)
{
    stream_format_auds.format = read_word();              /* wFormatTag */
    stream_format_auds.channels = read_word();            /* nChannels */
    stream_format_auds.samples_per_second = read_int();   /* nSamplesPerSec */
    stream_format_auds.bytes_per_second = read_int();     /* nAvgBytesPerSec */
    stream_format_auds.block_size_of_data = read_word();  /* nBlockAlign */
    stream_format_auds.bits_per_sample = read_word();     /* wBitsPerSample */

    /* WAVE_FORMAT_EXTENSIBLE: the real format tag is the first word of the
     * SubFormat GUID (offset 24 in the strf payload; layout is cbSize(2),
     * wValidBitsPerSample(2), dwChannelMask(4), SubFormat GUID(16)). */
    if (stream_format_auds.format == 0xFFFE && strf_size >= 26) {
        stream_format_auds.extended_size = read_word(); /* cbSize */
        read_word();                                    /* wValidBitsPerSample */
        read_int();                                     /* dwChannelMask */
        stream_format_auds.format = read_word();        /* SubFormat tag */
    }
    return 0;
}

int ReadAVI::parse_hdrl_list()
{
    char chunk_id[5];
    uint32_t chunk_size;
    char chunk_type[5];
    int64_t end_of_chunk;
    int64_t next_chunk;
    int stream_type = 0;

    read_chars(chunk_id, 4);
    chunk_size = read_uint();
    read_chars(chunk_type, 4);

    end_of_chunk = (int64_t)inFile.tellg() + (int64_t)chunk_size - 4;

    if (strcmp(chunk_id, "JUNK") == 0) {
        inFile.seekg((streamoff)end_of_chunk, ios_base::beg);
        return 0;
    }

    while ((int64_t)inFile.tellg() < end_of_chunk) {
        read_chars(chunk_type, 4);
        chunk_size = read_uint();
        next_chunk = (int64_t)inFile.tellg() + (int64_t)chunk_size;

        if (strcmp("strh", chunk_type) == 0) {
            int64_t marker = (int64_t)inFile.tellg();
            char buffer[5];
            read_chars(buffer, 4);
            inFile.seekg((streamoff)marker, ios_base::beg);

            if (strcmp(buffer, "vids") == 0) {
                stream_type = 0;
                read_stream_header(&stream_header_vid);
            } else if (strcmp(buffer, "auds") == 0) {
                stream_type = 1;
                read_stream_header(&stream_header_auds);
            } else {
                /* txts/mids/other stream: leave headers alone, skip chunks */
                stream_type = -1;
            }
        } else if (strcmp("strf", chunk_type) == 0) {
            if (stream_type == 0)
                read_stream_format();
            else if (stream_type == 1)
                read_stream_format_auds(chunk_size);
        }

        inFile.seekg((streamoff)(next_chunk + (next_chunk & 1)), ios_base::beg);
    }

    inFile.seekg((streamoff)end_of_chunk, ios_base::beg);
    return 0;
}

/* Scan one `LIST movi` payload, recording every ##db/##dc/##pc/##wb chunk.
 * `rec ` grouping LISTs are descended into; anything unrecognized (OpenDML
 * ix## sub-indexes, JUNK padding) is skipped by its declared size. */
int ReadAVI::parse_movi(int64_t list_end)
{
    while ((int64_t)inFile.tellg() + 8 <= list_end) {
        char chunk_id[5];
        index_entry_t index_entry;

        int64_t offset = (int64_t)inFile.tellg();
        read_chars(chunk_id, 4);
        uint32_t chunk_size = read_uint();

        if (!inFile.good())
            break;

        if (strcmp(chunk_id, "LIST") == 0) {
            /* Descend: skip the 4-byte list type (`rec `), scan its chunks. */
            read_chars(chunk_id, 4);
            continue;
        }

        index_entry.stream_num = decodeCkid(chunk_id, &index_entry.type);
        if (index_entry.stream_num >= 0 && chunk_size > 0) {
            index_entry.dwChunkOffset = offset + 8;
            index_entry.dwChunkLength = chunk_size;
            index_entries.push_back(index_entry);
        }

        int64_t next = offset + 8 + (int64_t)chunk_size;
        next += (next & 1); /* chunks are word-aligned */
        if (next <= offset) /* corrupt size — bail rather than loop forever */
            break;
        inFile.seekg((streamoff)next, ios_base::beg);
    }
    return 0;
}

int ReadAVI::parse_hdrl(unsigned int size)
{
    char chunk_id[5];
    uint32_t chunk_size;
    int64_t offset = (int64_t)inFile.tellg();

    read_chars(chunk_id, 4);
    chunk_size = read_uint();
    (void)chunk_size;

    read_avi_header();

    while ((int64_t)inFile.tellg() < offset + (int64_t)size - 4)
        parse_hdrl_list();

    return 0;
}

/* Walk one top-level RIFF segment (`AVI ` or `AVIX`). The stream is
 * positioned just past the segment's 12-byte RIFF header. Headers (hdrl)
 * only exist in the first segment; movi lists exist in all of them. */
int ReadAVI::parse_riff_segment(int64_t segment_end, bool first_segment)
{
    char chunk_id[5];
    uint32_t chunk_size;
    char chunk_type[5];
    int64_t end_of_subchunk;

    if (first_segment) {
        ZEROIZE(avi_header);
        ZEROIZE(stream_header_vid);
        ZEROIZE(stream_format_vid);
        ZEROIZE(stream_header_auds);
        ZEROIZE(stream_format_auds);
    }

    while ((int64_t)inFile.tellg() + 8 <= segment_end) {
        read_chars(chunk_id, 4);
        chunk_size = read_uint();

        if (!inFile.good())
            break;

        end_of_subchunk = (int64_t)inFile.tellg() + (int64_t)chunk_size;

        if (strcmp("LIST", chunk_id) == 0) {
            read_chars(chunk_type, 4);

            if (strcmp("hdrl", chunk_type) == 0 && first_segment) {
                parse_hdrl(chunk_size);
                headers_parsed = true;
            } else if (strcmp("movi", chunk_type) == 0) {
                parse_movi(end_of_subchunk);
            }
            /* INFO / odml / unknown lists: skip */
        }
        /* idx1 deliberately ignored — the movi scan above is the index. */

        if (chunk_size == 0 && strcmp("LIST", chunk_id) != 0)
            break; /* corrupt chunk — avoid an infinite loop */

        end_of_subchunk += (end_of_subchunk & 1);
        if (end_of_subchunk > segment_end)
            break;
        inFile.clear();
        inFile.seekg((streamoff)end_of_subchunk, ios_base::beg);
    }

    if (first_segment && stream_format_vid.palette) {
        delete[] stream_format_vid.palette;
        stream_format_vid.palette = NULL;
    }
    return 0;
}

uint32_t ReadAVI::read_uint()
{
    unsigned char buf[4];
    inFile.read((char*)buf, 4);
    return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

int ReadAVI::read_int()
{
    unsigned char buf[4];
    inFile.read((char*)buf, 4);
    return buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
}

int ReadAVI::read_word()
{
    unsigned char buf[2];
    inFile.read((char*)buf, 2);
    return buf[0] | (buf[1] << 8);
}

void ReadAVI::read_chars(char* s, int count)
{
    inFile.read(s, count);
    s[count] = 0;
}

void ReadAVI::read_chars_bin(unsigned char* s, int count)
{
    inFile.read((char*)s, count);
}
