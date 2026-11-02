/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "xa_wav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sh_log.h"

extern const char* PcPort_GetGameDataPath(void);

static uint32_t Rd32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint32_t Rd16(const unsigned char* p) { return p[0] | (p[1] << 8); }

void XaWav_OverridePath(uint16_t xaIdx, char* out, size_t outSize)
{
    snprintf(out, outSize, "%s/load/XA/xa_%04u.wav", PcPort_GetGameDataPath(), (unsigned)xaIdx);
}

int XaWav_Load(const char* path, unsigned char** outPcm, uint32_t* outBytes, int* outRate, int* outStereo)
{
    FILE*          f;
    long           size;
    unsigned char* d;
    uint32_t       off, fmtTag = 0, channels = 0, rate = 0, bits = 0;
    const unsigned char* data = NULL;
    uint32_t       dataLen = 0;

    f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 44 || size > 64L * 1024L * 1024L) { fclose(f); return 0; }
    d = (unsigned char*)malloc((size_t)size);
    if (!d || fread(d, 1, (size_t)size, f) != (size_t)size) { free(d); fclose(f); return 0; }
    fclose(f);

    if (memcmp(d, "RIFF", 4) != 0 || memcmp(d + 8, "WAVE", 4) != 0)
    {
        SH_DBG("[XA] %s is not a RIFF WAVE file, ignored", path);
        free(d);
        return 0;
    }
    for (off = 12; off + 8 <= (uint32_t)size; )
    {
        uint32_t len = Rd32(d + off + 4);
        const unsigned char* body = d + off + 8;
        if (off + 8 + len > (uint32_t)size) len = (uint32_t)size - off - 8;
        if (memcmp(d + off, "fmt ", 4) == 0 && len >= 16)
        {
            fmtTag   = Rd16(body);
            channels = Rd16(body + 2);
            rate     = Rd32(body + 4);
            bits     = Rd16(body + 14);
            /* WAVE_FORMAT_EXTENSIBLE: the real tag is the sub-format's first word. */
            if (fmtTag == 0xFFFE && len >= 26) fmtTag = Rd16(body + 24);
        }
        else if (memcmp(d + off, "data", 4) == 0)
        {
            data    = body;
            dataLen = len;
        }
        off += 8 + len + (len & 1);
    }
    if (!data || fmtTag != 1 || (channels != 1 && channels != 2) || rate < 4000 || rate > 96000 ||
        (bits != 16 && bits != 8))
    {
        SH_DBG("[XA] %s unsupported (tag=%u ch=%u rate=%u bits=%u); needs 8/16-bit PCM mono or stereo",
               path, fmtTag, channels, rate, bits);
        free(d);
        return 0;
    }

    {
        uint32_t frameIn  = channels * (bits / 8);
        uint32_t frames   = dataLen / frameIn;
        uint32_t bytes    = frames * channels * 2;
        unsigned char* pcm = (unsigned char*)malloc(bytes ? bytes : 2);
        if (!pcm) { free(d); return 0; }
        if (bits == 16)
        {
            memcpy(pcm, data, bytes);
        }
        else
        {
            uint32_t i;
            int16_t* o = (int16_t*)pcm;
            for (i = 0; i < frames * channels; i++) o[i] = (int16_t)(((int)data[i] - 128) << 8);
        }
        /* The software SPU's XA input takes only the disc rates and drops
         * anything else without a word, so everything becomes 37800 Hz here. */
        if (rate != 37800 && rate != 18900)
        {
            uint64_t outFrames = ((uint64_t)frames * 37800u) / rate;
            int16_t* in  = (int16_t*)pcm;
            int16_t* out = (int16_t*)malloc((size_t)(outFrames * channels * 2 + 2));
            uint64_t i;
            uint32_t c;
            if (!out) { free(pcm); free(d); return 0; }
            for (i = 0; i < outFrames; i++)
            {
                uint64_t srcPos = i * rate;
                uint32_t idx    = (uint32_t)(srcPos / 37800u);
                uint32_t frac   = (uint32_t)(srcPos % 37800u);
                for (c = 0; c < channels; c++)
                {
                    int a = in[idx * channels + c];
                    int b = (idx + 1 < frames) ? in[(idx + 1) * channels + c] : a;
                    out[i * channels + c] = (int16_t)(a + (int)(((int64_t)(b - a) * frac) / 37800));
                }
            }
            free(pcm);
            pcm   = (unsigned char*)out;
            bytes = (uint32_t)(outFrames * channels * 2);
            rate  = 37800;
        }
        *outPcm   = pcm;
        *outBytes = bytes;
    }
    free(d);
    *outRate   = (int)rate;
    *outStereo = (channels == 2);
    return 1;
}

/* A text-box voice file mixed into a disc line that is still producing audio
 * (map1_s02: Harry's thoughts appear over the tail of Cheryl's stream). Held
 * in the stream's own rate and channel count so it adds sample for sample. */
static int16_t* s_sidePcm    = NULL;
static uint32_t s_sideFrames = 0;
static uint32_t s_sidePos    = 0;
static int      s_sideRate   = 0;
static int      s_sideStereo = 0;

void XaSide_Stop(void)
{
    free(s_sidePcm);
    s_sidePcm    = NULL;
    s_sideFrames = 0;
    s_sidePos    = 0;
}

int XaSide_Start(const char* path, int rate, int stereo)
{
    unsigned char* pcm;
    uint32_t       bytes, inFrames, outFrames, i;
    int            inRate = 0, inStereo = 0;
    int            inCh, outCh = stereo ? 2 : 1;
    int16_t*       in;
    int16_t*       out;

    if (rate <= 0 || !XaWav_Load(path, &pcm, &bytes, &inRate, &inStereo)) return 0;
    inCh      = inStereo ? 2 : 1;
    in        = (int16_t*)pcm;
    inFrames  = bytes / (uint32_t)(inCh * 2);
    outFrames = (uint32_t)(((uint64_t)inFrames * (uint32_t)rate) / (uint32_t)inRate);
    out       = (int16_t*)malloc((size_t)outFrames * (size_t)outCh * 2 + 2);
    if (!out || inFrames == 0) { free(out); free(pcm); return 0; }
    for (i = 0; i < outFrames; i++)
    {
        uint64_t srcPos = (uint64_t)i * (uint32_t)inRate;
        uint32_t idx    = (uint32_t)(srcPos / (uint32_t)rate);
        int64_t  frac   = (int64_t)(srcPos % (uint32_t)rate);
        uint32_t nxt    = (idx + 1 < inFrames) ? idx + 1 : idx;
        int      c;
        for (c = 0; c < outCh; c++)
        {
            int a, b;
            if (inCh == outCh)
            {
                a = in[idx * inCh + c];
                b = in[nxt * inCh + c];
            }
            else if (inCh == 1)
            {
                a = in[idx];
                b = in[nxt];
            }
            else
            {
                a = (in[idx * 2] + in[idx * 2 + 1]) / 2;
                b = (in[nxt * 2] + in[nxt * 2 + 1]) / 2;
            }
            out[i * outCh + c] = (int16_t)(a + (int)(((int64_t)(b - a) * frac) / rate));
        }
    }
    free(pcm);
    XaSide_Stop();
    s_sidePcm    = out;
    s_sideFrames = outFrames;
    s_sideRate   = rate;
    s_sideStereo = stereo;
    SH_DBG("[XA] file %s mixed over the playing line (%ums)", path,
           (unsigned)(((uint64_t)outFrames * 1000u) / (uint32_t)rate));
    return 1;
}

int XaSide_Active(void)
{
    return s_sidePcm != NULL && s_sidePos < s_sideFrames;
}

uint32_t XaSide_Mix(const int16_t* src, int16_t* dst, uint32_t frames, int channels)
{
    uint32_t n, i;
    int      c;

    if (!XaSide_Active() || channels != (s_sideStereo ? 2 : 1)) return 0;
    n = s_sideFrames - s_sidePos;
    if (n > frames) n = frames;
    memcpy(dst, src, (size_t)frames * (size_t)channels * 2);
    for (i = 0; i < n; i++)
    {
        for (c = 0; c < channels; c++)
        {
            int v = dst[i * channels + c] + s_sidePcm[(s_sidePos + i) * channels + c];
            dst[i * channels + c] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
        }
    }
    return n;
}

void XaSide_Advance(uint32_t frames)
{
    s_sidePos += frames;
    if (s_sidePos > s_sideFrames) s_sidePos = s_sideFrames;
}

int XaSide_TakeRest(unsigned char** outPcm, uint32_t* outBytes, int* outRate, int* outStereo)
{
    int      ch = s_sideStereo ? 2 : 1;
    uint32_t bytes;

    if (!XaSide_Active()) return 0;
    bytes   = (s_sideFrames - s_sidePos) * (uint32_t)ch * 2;
    *outPcm = (unsigned char*)malloc(bytes);
    if (!*outPcm) { XaSide_Stop(); return 0; }
    memcpy(*outPcm, s_sidePcm + (size_t)s_sidePos * (size_t)ch, bytes);
    *outBytes  = bytes;
    *outRate   = s_sideRate;
    *outStereo = s_sideStereo;
    XaSide_Stop();
    return 1;
}
