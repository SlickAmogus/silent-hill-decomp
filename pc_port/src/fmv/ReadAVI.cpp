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
    movi_offset = 0;
    fileSize = 0;

    inFile.open(filename, ios_base::ate | ios_base::in | ios_base::binary);
    if (!inFile.is_open())
        return;

    fileSize = (long)inFile.tellg();
    inFile.close();
    inFile.open(filename, ios_base::in | ios_base::binary);

    try {
        parse_riff();
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
                inFile.seekg(index_entries[i].dwChunkOffset, ios_base::beg);
                read_chars_bin(data_buf, index_entries[i].dwChunkLength);
            }
            catch (...) {
                return -1;
            }
            frame_entry->buf = data_buf;
            frame_entry->pointer = i + 1;
            frame_entry->stream_num = index_entries[i].stream_num;
            frame_entry->type = index_entries[i].type;
            return index_entries[i].dwChunkLength;
        }
    }
    return -1;
}

int ReadAVI::decodeCkid(char* ckid, chunk_type_t* chunk_type)
{
    int stream = (ckid[0] - '0') * 10 + (ckid[1] - '0');
    for (int i = 0; i < ChunkTypesCnt; i++) {
        if (chunk_types[i].type_id[0] == ckid[2] && chunk_types[i].type_id[1] == ckid[3]) {
            *chunk_type = chunk_types[i].type;
            return stream;
        }
    }
    return -1;
}

int ReadAVI::parse_idx1(int chunk_len)
{
    index_entry_t index_entry;
    index_entry.type = ctype_none;

    for (int t = 0; t < chunk_len / 16; t++) {
        char buf[5];
        read_chars(buf, 4);
        index_entry.stream_num = decodeCkid(buf, &index_entry.type);
        index_entry.dwFlags = read_int();
        index_entry.dwChunkOffset = read_int() + movi_offset + 0x10;
        index_entry.dwChunkLength = read_int();
        index_entries.push_back(index_entry);
    }
    return 0;
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

    if (stream_format_vid.colors_important != 0) {
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

int ReadAVI::read_stream_format_auds()
{
    stream_format_auds.format = read_word();              /* wFormatTag */
    stream_format_auds.channels = read_word();            /* nChannels */
    stream_format_auds.samples_per_second = read_int();   /* nSamplesPerSec */
    stream_format_auds.bytes_per_second = read_int();     /* nAvgBytesPerSec */
    stream_format_auds.block_size_of_data = read_word();  /* nBlockAlign */
    stream_format_auds.bits_per_sample = read_word();     /* wBitsPerSample */
    return 0;
}

int ReadAVI::parse_hdrl_list()
{
    char chunk_id[5];
    int chunk_size;
    char chunk_type[5];
    int end_of_chunk;
    int next_chunk;
    int stream_type = 0;

    read_chars(chunk_id, 4);
    chunk_size = read_int();
    read_chars(chunk_type, 4);

    end_of_chunk = chunk_size - 4 + (int)inFile.tellg();

    if (strcmp(chunk_id, "JUNK") == 0) {
        inFile.seekg(end_of_chunk, ios_base::beg);
        return 0;
    }

    while (inFile.tellg() < end_of_chunk) {
        read_chars(chunk_type, 4);
        chunk_size = read_int();
        next_chunk = chunk_size + (int)inFile.tellg();

        if (strcmp("strh", chunk_type) == 0) {
            long marker = (long)inFile.tellg();
            char buffer[5];
            read_chars(buffer, 4);
            inFile.seekg(marker, ios_base::beg);

            if (strcmp(buffer, "vids") == 0) {
                stream_type = 0;
                read_stream_header(&stream_header_vid);
            } else if (strcmp(buffer, "auds") == 0) {
                stream_type = 1;
                read_stream_header(&stream_header_auds);
            } else {
                return -1;
            }
        } else if (strcmp("strf", chunk_type) == 0) {
            if (stream_type == 0)
                read_stream_format();
            else
                read_stream_format_auds();
        }

        inFile.seekg(next_chunk, ios_base::beg);
    }

    inFile.seekg(end_of_chunk, ios_base::beg);
    return 0;
}

int ReadAVI::parse_movi(int size)
{
    char chunk_id[5];
    index_entry_t index_entry;

    do {
        long offset = (long)inFile.tellg();
        read_chars(chunk_id, 4);
        index_entry.stream_num = decodeCkid(chunk_id, &index_entry.type);

        if (index_entry.stream_num < 0) {
            inFile.seekg(offset - 4, ios_base::beg);
            break;
        }

        index_entry.dwChunkLength = read_int();

        if (!(avi_header.Flags & AVIF_HASINDEX)) {
            index_entry.dwChunkOffset = (int)offset + 8;
            index_entries.push_back(index_entry);
        }

        long end_of_chunk = index_entry.dwChunkLength + inFile.tellg();
        end_of_chunk = (end_of_chunk + 1) & ~1;
        inFile.seekg(end_of_chunk, ios_base::beg);

        int blk_size = (int)(end_of_chunk - offset);
        size -= blk_size;
    } while (size > 7);

    return 0;
}

int ReadAVI::parse_hdrl(unsigned int size)
{
    char chunk_id[5];
    int chunk_size;
    int end_of_chunk;
    long offset = (long)inFile.tellg();

    read_chars(chunk_id, 4);
    chunk_size = read_int();

    end_of_chunk = chunk_size + (int)inFile.tellg();
    if ((end_of_chunk % 4) != 0)
        end_of_chunk = end_of_chunk + (4 - (end_of_chunk % 4));

    read_avi_header();

    while (inFile.tellg() < offset + (long)size - 4)
        parse_hdrl_list();

    return 0;
}

int ReadAVI::parse_riff()
{
    char chunk_id[5];
    int chunk_size;
    char chunk_type[5];
    int end_of_chunk, end_of_subchunk;

    ZEROIZE(avi_header);
    ZEROIZE(stream_header_vid);
    ZEROIZE(stream_format_vid);
    ZEROIZE(stream_header_auds);
    ZEROIZE(stream_format_auds);

    read_chars(chunk_id, 4);
    chunk_size = read_int();
    read_chars(chunk_type, 4);

    if (strcmp("RIFF", chunk_id) != 0 || strcmp("AVI ", chunk_type) != 0)
        return 1;

    end_of_chunk = chunk_size - 4 + (int)inFile.tellg();

    while (inFile.tellg() < end_of_chunk) {
        long offset = (long)inFile.tellg();
        read_chars(chunk_id, 4);
        chunk_size = read_int();
        end_of_subchunk = chunk_size + (int)inFile.tellg();

        if (strcmp("JUNK", chunk_id) == 0 || strcmp("PAD ", chunk_id) == 0) {
            chunk_type[0] = 0;
        } else {
            read_chars(chunk_type, 4);
        }

        if (strcmp("JUNK", chunk_id) == 0 || strcmp("PAD ", chunk_id) == 0) {
            /* skip */
        } else if (strcmp("INFO", chunk_type) == 0) {
            /* skip */
        } else if (strcmp("hdrl", chunk_type) == 0) {
            parse_hdrl(chunk_size);
        } else if (strcmp("movi", chunk_type) == 0) {
            movi_offset = (long)offset;
            parse_movi(chunk_size);
        } else if (strcmp("idx1", chunk_id) == 0) {
            inFile.seekg(inFile.tellg() - std::streamoff(4), ios_base::beg);
            parse_idx1(chunk_size);
        } else {
            if (chunk_size == 0) break;
        }

        inFile.seekg(end_of_subchunk, ios_base::beg);
    }

    if (stream_format_vid.palette) {
        delete[] stream_format_vid.palette;
        stream_format_vid.palette = NULL;
    }
    return 0;
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
