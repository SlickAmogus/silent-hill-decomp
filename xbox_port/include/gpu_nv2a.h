/*
 * gpu_nv2a.h - low-level NV2A surface used by the PSX libgpu (gpu_xbox.c).
 */
#ifndef SH_GPU_NV2A_H
#define SH_GPU_NV2A_H

/* Screen-space vertex fed to the NV2A: position in pixels, diffuse colour
 * (0..1), texcoord in TEXELS (matches vs.vs.cg attribs 0/3/9). */
#pragma pack(1)
typedef struct {
    float pos[3];
    float col[4];
    float tex[2];
} ShVertex;
#pragma pack()

void GpuNv2a_Init(void);
void GpuNv2a_FrameBegin(void);
void GpuNv2a_FrameEnd(void);
void GpuNv2a_WaitVbl(void);
void GpuNv2a_EmitTris(const ShVertex* verts, int count);

/* Texture binding for the PSX VRAM path (psx_vram.c). BindTexture binds an
 * ARGB8888 buffer of w*h texels; BindWhite restores the 1x1-white default for
 * untextured prims. AllocTexMem returns GPU-DMA-able contiguous memory. */
void  GpuNv2a_BindTexture(const void* addr, int w, int h);
void  GpuNv2a_BindWhite(void);
void* GpuNv2a_AllocTexMem(int bytes);

#endif /* SH_GPU_NV2A_H */
