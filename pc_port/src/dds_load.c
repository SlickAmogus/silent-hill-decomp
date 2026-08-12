/* SPDX-License-Identifier: GPL-3.0-or-later */
/* BC7 .dds upload for texture mods.
 *
 * A pack texture costs 4 bytes/texel as RGBA8, plus a third again for the mip
 * chain (hires_override.c pack_bytes_for). BC7 is 16 bytes per 4x4 block = 1
 * byte/texel, so a BC7 mod is exactly 4x cheaper in VRAM for the same art, with
 * a real 8-bit alpha channel — which matters because the 32-bit override shader
 * cuts out on `alpha < 0.5`. (BC1/DXT1 would wreck that cutout; only BC7 is
 * accepted here.)
 *
 * Only BC7 with the DX10 header is read. BC7 has no legacy FourCC, so anything
 * else is rejected rather than guessed at.
 *
 * BPTC is core only in GL 4.2 and PsyX walks the context version down as far as
 * 3.0, so support is an ARB extension query at runtime with a clean fall back to
 * the existing .png path. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dds_load.h"
#include "sh_log.h"

#define BCDEC_IMPLEMENTATION
#include "bcdec.h"

#include <PsyX/common/glad.h>

#ifndef GL_COMPRESSED_RGBA_BPTC_UNORM
#define GL_COMPRESSED_RGBA_BPTC_UNORM 0x8E8C
#endif
#ifndef GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM
#define GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM 0x8E8D
#endif
#ifndef GL_NUM_EXTENSIONS
#define GL_NUM_EXTENSIONS 0x821D
#endif
#ifndef GL_TEXTURE_MAX_LEVEL
#define GL_TEXTURE_MAX_LEVEL 0x813D
#endif

#define DDS_MAGIC       0x20534444u /* "DDS " */
#define DDS_HDR_SIZE    124
#define DDS_DX10_OFF    (4 + DDS_HDR_SIZE)
#define DDS_DATA_OFF    (DDS_DX10_OFF + 20)
#define DDPF_FOURCC     0x4
#define FOURCC_DX10     0x30315844u /* "DX10" */
#define DXGI_BC7_UNORM  98
#define DXGI_BC7_SRGB   99

static int s_bptcChecked = 0;
static int s_bptcOk = 0;

/* glGetString(GL_EXTENSIONS) returns NULL in a core profile, so the indexed
 * query is the only reliable form here. Checked once, logged once. */
int Dds_BptcSupported(void)
{
    GLint n = 0, i;

    if (s_bptcChecked) return s_bptcOk;
    s_bptcChecked = 1;

    if (glGetStringi == NULL)
    {
        SH_DBG("[DDS] glGetStringi unavailable — BC7 .dds disabled");
        return 0;
    }
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (i = 0; i < n; i++)
    {
        const char* e = (const char*)glGetStringi(GL_EXTENSIONS, (GLuint)i);
        if (e != NULL && strcmp(e, "GL_ARB_texture_compression_bptc") == 0)
        {
            s_bptcOk = 1;
            break;
        }
    }
    SH_DBG("[DDS] BC7 (GL_ARB_texture_compression_bptc): %s",
           s_bptcOk ? "supported" : "NOT supported — .dds mods will fall back to .png");
    return s_bptcOk;
}

static unsigned rd32(const unsigned char* p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

int Dds_ParseBptc(const unsigned char* data, int size, s_DdsBptc* out)
{
    unsigned flags, fourcc, dxgi;

    if (data == NULL || size < (int)DDS_DATA_OFF) return 0;
    if (rd32(data) != DDS_MAGIC) return 0;
    if (rd32(data + 4) != DDS_HDR_SIZE) return 0;

    flags  = rd32(data + 4 + 76);   /* DDS_PIXELFORMAT.dwFlags  */
    fourcc = rd32(data + 4 + 80);   /* DDS_PIXELFORMAT.dwFourCC */
    if (!(flags & DDPF_FOURCC) || fourcc != FOURCC_DX10) return 0;

    dxgi = rd32(data + DDS_DX10_OFF);
    if (dxgi != DXGI_BC7_UNORM && dxgi != DXGI_BC7_SRGB) return 0;

    out->height   = (int)rd32(data + 4 + 8);
    out->width    = (int)rd32(data + 4 + 12);
    out->mipCount = (int)rd32(data + 4 + 24);
    out->srgb     = (dxgi == DXGI_BC7_SRGB);
    out->data     = data + DDS_DATA_OFF;
    out->size     = size - (int)DDS_DATA_OFF;
    if (out->width <= 0 || out->height <= 0 || out->size <= 0) return 0;
    if (out->mipCount < 1) out->mipCount = 1;
    return 1;
}

/* Human-readable name for a .dds the CPU decoder refuses, so an unreadable pack
 * entry is never silently dropped. Only the block formats a texture mod would
 * plausibly ship are named; anything else prints its raw code. */
static const char* Dds_FormatName(unsigned fourcc, unsigned dxgi, char* buf, size_t bufSize)
{
    if (fourcc != FOURCC_DX10)
    {
        snprintf(buf, bufSize, "FourCC '%c%c%c%c'",
                 (char)(fourcc & 0xFF), (char)((fourcc >> 8) & 0xFF),
                 (char)((fourcc >> 16) & 0xFF), (char)((fourcc >> 24) & 0xFF));
        return buf;
    }
    switch (dxgi)
    {
        case 28: case 29: return "DXGI 28/29 R8G8B8A8 (uncompressed)";
        case 87: case 88: return "DXGI 87/88 B8G8R8A8 (uncompressed)";
        case 70: case 71: case 72: return "DXGI 70-72 BC1/DXT1";
        case 73: case 74: case 75: return "DXGI 73-75 BC2/DXT3";
        case 76: case 77: case 78: return "DXGI 76-78 BC3/DXT5";
        case 79: case 80: case 81: return "DXGI 79-81 BC4";
        case 82: case 83: case 84: return "DXGI 82-84 BC5";
        case 94: case 95: case 96: return "DXGI 94-96 BC6H (HDR)";
        default: break;
    }
    snprintf(buf, bufSize, "DXGI format %u", dxgi);
    return buf;
}

static void Dds_LogUnsupported(const unsigned char* data, int size)
{
    static int s_logged = 0;
    char       buf[64];
    unsigned   fourcc = 0, dxgi = 0;

    if (s_logged >= 4) return;
    s_logged++;

    if (data == NULL || size < (int)(4 + DDS_HDR_SIZE) || rd32(data) != DDS_MAGIC)
    {
        SH_DBG("[DDS] not a .dds file — entry skipped");
        return;
    }
    fourcc = rd32(data + 4 + 80);
    if (fourcc == FOURCC_DX10 && size >= (int)DDS_DATA_OFF) dxgi = rd32(data + DDS_DX10_OFF);
    SH_DBG("[DDS] %s is not decodable — only BC7 (DXGI 98/99) is supported; entry skipped",
           Dds_FormatName(fourcc, dxgi, buf, sizeof(buf)));
}

/* CPU decode of mip 0 to RGBA8. The sub-rect compositor (tex_pack.c) blits
 * 32-bit pixels and cannot composite BC7 blocks, so an entry that cannot take
 * the whole-upload compressed path is decoded here instead. Caller frees. */
unsigned char* Dds_DecodeRgba(const unsigned char* data, int size, int* outW, int* outH)
{
    s_DdsBptc      d;
    unsigned char* rgba;
    int            bx, by, bw, bh, pitch;

    if (!Dds_ParseBptc(data, size, &d))
    {
        Dds_LogUnsupported(data, size);
        return NULL;
    }

    bw    = (d.width + 3) / 4;
    bh    = (d.height + 3) / 4;
    pitch = d.width * 4;
    if ((long long)bw * (long long)bh * 16 > (long long)d.size)
    {
        static int s_truncLog = 0;
        if (s_truncLog < 4)
        {
            s_truncLog++;
            SH_DBG("[DDS] truncated %dx%d BC7 — %d bytes of blocks, %lld needed; entry skipped",
                   d.width, d.height, d.size, (long long)bw * (long long)bh * 16);
        }
        return NULL;
    }

    rgba = (unsigned char*)malloc((size_t)d.width * (size_t)d.height * 4);
    if (rgba == NULL) return NULL;

    for (by = 0; by < bh; by++)
    {
        for (bx = 0; bx < bw; bx++)
        {
            const unsigned char* src = d.data + ((size_t)by * (size_t)bw + (size_t)bx) * 16;
            unsigned char*       dst = rgba + (size_t)(by * 4) * (size_t)pitch + (size_t)(bx * 4) * 4;

            /* bcdec always writes a full 4x4; a texture whose size is not a
             * multiple of 4 has padded edge blocks that would run past the
             * buffer, so those decode via a scratch block and are clipped. */
            if ((bx + 1) * 4 <= d.width && (by + 1) * 4 <= d.height)
            {
                bcdec_bc7(src, dst, pitch);
            }
            else
            {
                unsigned char tmp[4 * 4 * 4];
                int           cw = d.width - bx * 4;
                int           ch = d.height - by * 4;
                int           r;

                if (cw > 4) cw = 4;
                if (ch > 4) ch = 4;
                bcdec_bc7(src, tmp, 4 * 4);
                for (r = 0; r < ch; r++)
                    memcpy(dst + (size_t)r * (size_t)pitch, tmp + (size_t)r * 16, (size_t)cw * 4);
            }
        }
    }

    *outW = d.width;
    *outH = d.height;
    return rgba;
}

/* glGenerateMipmap is INVALID on a compressed texture and fails silently black,
 * so the file's own mip chain is uploaded level by level; a single-level file
 * pins MAX_LEVEL to 0 instead. */
int Dds_UploadBptc(unsigned int* tex, const unsigned char* fileData, int fileSize, int nearest)
{
    s_DdsBptc d;
    GLenum fmt;
    int level, w, h, off;

    if (!Dds_ParseBptc(fileData, fileSize, &d)) return -1;
    if (!Dds_BptcSupported()) return -1;

    if (*tex == 0)
    {
        glGenTextures(1, tex);
        if (*tex == 0) return -1;
    }
    /* Always the non-sRGB format, even for a BC7_UNORM_SRGB (dxgi 99) file.
     * The renderer is gamma-space throughout (PNG overrides upload as plain
     * GL_RGBA and nothing enables GL_FRAMEBUFFER_SRGB), and Dds_DecodeRgba —
     * the CPU path the same file takes when it has to be composited — returns
     * the stored bytes untouched. Honouring the sRGB tag here would make the
     * whole-cover and sub-rect paths of ONE file render at different
     * brightness, and would re-apply the very darkening the launcher's
     * --ignore-srgb fix exists to prevent. */
    fmt = GL_COMPRESSED_RGBA_BPTC_UNORM;
    glBindTexture(GL_TEXTURE_2D, *tex);
    while (glGetError() != GL_NO_ERROR) { } /* drain stale errors */

    w = d.width; h = d.height; off = 0;
    for (level = 0; level < d.mipCount; level++)
    {
        int bw = (w + 3) / 4, bh = (h + 3) / 4;
        int bytes = bw * bh * 16; /* BC7: 16 bytes per 4x4 block */
        if (off + bytes > d.size) break;
        glCompressedTexImage2D(GL_TEXTURE_2D, level, fmt, w, h, 0, bytes, d.data + off);
        off += bytes;
        if (w == 1 && h == 1) { level++; break; }
        if (w > 1) w >>= 1;
        if (h > 1) h >>= 1;
    }

    /* Same contract as upload_rgba: any error degrades to a clean miss rather
     * than a live texture over undefined storage. A compressed upload has more
     * ways to fail, not fewer. */
    {
        GLenum err = glGetError();
        if (err != GL_NO_ERROR || level == 0)
        {
            static int s_errLog = 0;
            if (s_errLog < 8)
            {
                SH_DBG("[DDS] GL error 0x%X uploading %dx%d BC7 — keeping native art",
                       (unsigned)err, d.width, d.height);
                s_errLog++;
            }
            glBindTexture(GL_TEXTURE_2D, 0);
            glDeleteTextures(1, tex);
            *tex = 0;
            return -1;
        }
    }

    if (level > 1)
    {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, level - 1);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        nearest ? GL_NEAREST : GL_LINEAR_MIPMAP_LINEAR);
    }
    else
    {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return 0;
}
