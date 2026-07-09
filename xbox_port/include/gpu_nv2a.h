/*
 * gpu_nv2a.h - low-level NV2A surface used by the PSX libgpu (gpu_xbox.c).
 */
#ifndef SH_GPU_NV2A_H
#define SH_GPU_NV2A_H

/* Screen-space vertex fed to the NV2A: position in pixels, diffuse colour
 * (0..1), texcoord in TEXELS, specular = per-vertex fog contribution
 * (fogColor * fogFactor; the final combiner ADDS it: out = tex*col + spec).
 * Matches vs.vs.cg attribs 0/3/9/4 — spec appended LAST so pos/col/tex byte
 * offsets are unchanged. */
#pragma pack(1)
typedef struct {
    float pos[3];
    float col[4];
    float tex[2];
    float spec[4];
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

/* Blend state, encoded: 0 = blend off (opaque); 1..4 = ABE on with PSX ABR
 * mode 0..3 (0.5B+0.5F / B+F additive / B-F subtractive / B+0.25F). */
void  GpuNv2a_SetBlendMode(int mode);

/* Draw-clip scissor in FRAMEBUFFER pixels (NV2A window clip). w<=0 or h<=0
 * resets to the full 640x480 surface. */
void  GpuNv2a_SetScissor(int x, int y, int w, int h);

/* Frame clear colour (0xAARRGGBB) — gpu_xbox.c supplies the PSX draw-env
 * isbg background (fog colour in-game) via GpuXbox_GetClearColor(). */
unsigned int GpuXbox_GetClearColor(void);

/* Framebuffer -> PSX-VRAM readback (screen-grab effects: pause/save backgrounds,
 * air-screamer window-crash distortion, StoreImage grabs of the display pages).
 * ReadbackSurface returns a CPU-visible A8R8G8B8 surface (drains the GPU first):
 * fromLastQueued=0 -> the current back buffer (fully composed, pre-present),
 * fromLastQueued=1 -> the last completed/presented frame (safe mid-OT-walk).
 * The GpuXbox_* entry points live in gpu_xbox.c (they own the draw/display envs
 * and the screen transform, i.e. the inverse vertex mapping). */
const void* GpuNv2a_ReadbackSurface(int fromLastQueued, int* w, int* h, int* pitchBytes);
int   GpuNv2a_Ms(void);
int   GpuXbox_FbRegionOverlap(int x0, int y0, int x1, int y1);
void  GpuXbox_FbReadbackForTexture(void); /* psx_vram.c: 16-bit page decode over the fb */
void  GpuXbox_FbReadbackForStore(void);   /* psx_libgpu_xbox.c StoreImage: fb-rect grab */
void  GpuXbox_FbStoreFrameTick(void);     /* psx_libgpu_xbox.c VSync: per-frame gate reset */

#endif /* SH_GPU_NV2A_H */
