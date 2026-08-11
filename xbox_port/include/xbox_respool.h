/* Resident chunk textures on Xbox — see xbox_respool.c for the whole rationale.
 *
 * A virtual chunk-pool slot's pixels live here as the TIM's PSX word block, and
 * the paletted cache in psx_vram.c decodes out of it exactly as it decodes out
 * of s_vram for a real page. The point is that these pixels are PRIVATE: nothing
 * shares them, so the pool can never steal them out from under a prim. */
#ifndef XBOX_RESPOOL_H
#define XBOX_RESPOOL_H

typedef struct {
    int pitchWords;                  /* 16-bit words per row (the TIM's prect.w) */
    int w, h;                        /* pixel dimensions */
    int bpp;                         /* 4 or 8 */
    int rows;                        /* CLUT rows present */
    const unsigned short* clut;      /* rows * 256 words, row-major */
} s_XbResidentDesc;

/* Virtual slots the chunk pool may offer. Derived from the slabs actually
 * reserved, so an offered slot is always backable (see xbox_respool.c). */
int Xbox_ResidentFullSlots(void);
int Xbox_ResidentHalfSlots(void);

void Xbox_ResidentPoolReset(void);
int  Xbox_ResidentPoolRegisterTim(int slot, const unsigned char* tim, unsigned size,
                                  int nativeW, int nativeH);
const unsigned short* Xbox_ResidentPoolGet(int slot, s_XbResidentDesc* out);

unsigned Xbox_MemFreeKB(void);

#endif /* XBOX_RESPOOL_H */
