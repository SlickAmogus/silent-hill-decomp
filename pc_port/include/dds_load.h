#pragma once
#ifdef SH_PC_PORT

/* BC7 .dds textures for mods — 4x cheaper in VRAM than the RGBA8 path, with a
 * real 8-bit alpha so the override shader's `discard alpha<0.5` cutout survives.
 * See pc_port/src/dds_load.c. */

typedef struct
{
    int                  width, height;
    int                  mipCount;
    int                  srgb;
    const unsigned char* data; /* first mip level, largest first */
    int                  size; /* bytes available from `data` */
} s_DdsBptc;

/* Cached, logged-once query of GL_ARB_texture_compression_bptc. BPTC is core
 * only in GL 4.2 and the context floor here is 3.0, so this must be checked
 * before any BC7 upload. */
int Dds_BptcSupported(void);

/* Returns 1 and fills *out when the buffer is a BC7 DDS (DX10 header, dxgiFormat
 * 98/99). Anything else returns 0 — BC7 has no legacy FourCC, so other formats
 * are rejected rather than guessed at. */
int Dds_ParseBptc(const unsigned char* data, int size, s_DdsBptc* out);

/* Uploads a BC7 DDS into *tex (generated if 0). Uploads the file's OWN mip chain
 * level by level: glGenerateMipmap is invalid on compressed textures and fails
 * silently black. Returns 0 on success; on any failure the texture is deleted
 * and zeroed so the caller misses cleanly and keeps the native art. */
int Dds_UploadBptc(unsigned int* tex, const unsigned char* fileData, int fileSize, int nearest);

#endif /* SH_PC_PORT */
