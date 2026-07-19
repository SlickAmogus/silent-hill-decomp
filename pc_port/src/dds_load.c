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
#include <string.h>

#include "dds_load.h"
#include "sh_log.h"

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
    fmt = d.srgb ? GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM : GL_COMPRESSED_RGBA_BPTC_UNORM;
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
