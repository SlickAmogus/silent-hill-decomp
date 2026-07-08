#ifndef HIRES_OVERRIDE_H
#define HIRES_OVERRIDE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Hi-res TIM override system.
 *
 * When loose-file loading is enabled (g_PcConfig.allowLooseFiles), TIMs in
 * gamedata/load/ that are LARGER than the disc image's buffer slot are
 * treated as hi-res replacements. They cannot fit in PSX VRAM at native
 * resolution, so instead of overwriting the VRAM upload we:
 *   1. Parse the loose TIM (any bit depth) or PNG and decode to RGBA8
 *   2. Upload that decoded RGBA8 into a separate full-resolution GL texture
 *   3. Register the (vramX, vramY, vramW, vramH, clutX, clutY, bitDepth)
 *      tuple → GL texture mapping in a lookup table
 *   4. Let the disc image's native-res TIM upload to VRAM as normal (so
 *      VRAM still has consistent fallback data for any prim we don't catch)
 *
 * At draw time, the AddSplit hook in PsyX_GPU.cpp consults the table by the
 * incoming poly's (tpage, clut). On a match, the split's textureId is
 * redirected to the hi-res GL texture and its texFormat is forced to
 * TF_32_BIT_RGBA — which selects the direct-sample 32-bit RGBA shader that
 * already exists for DR_PSYX_TEX overrides. Sampling treats native PSX UVs
 * (0..vramSpanInPixels) as 0..1 of the hi-res texture, so any uniform
 * upscale factor (2×, 4×, 8×) Just Works.
 *
 * Limitations of v1:
 *   - One loose TIM is assumed to fully cover one tpage region. If a single
 *     tpage hosts two original disc TIMs (UV split layout), only one can be
 *     replaced at hi-res; the other prims still match the override and will
 *     sample from the wrong region.
 *   - 4bpp / 8bpp originals are matched via CLUT identity. If your loose
 *     replacement TIM uses a different CLUT layout than the original, the
 *     match fails (good — we fall back to native VRAM).
 */

/* Initialize the override system. Safe to call multiple times. */
void HiresOverride_Init(void);

/* Register a hi-res override.
 *   timPath:     loose-file path (used only for log messages)
 *   timData:     raw TIM file bytes (will be parsed; not retained after return)
 *   timSize:     size of timData in bytes
 *   targetVramX, targetVramY:   16-bit VRAM cell coords where the disc TIM was
 *                               supposed to live (i.e. the rect AddSplit will
 *                               see via active tpage). Used as match key.
 *   targetVramW, targetVramH:   VRAM cell dimensions of the original.
 *   targetClutX, targetClutY:   CLUT VRAM coords (or pass -1, -1 if 16bpp).
 *   originalBitDepth:           PSX bit depth (4, 8, or 16) of the disc original.
 * Returns 0 on success, nonzero on failure (parse error, table full, etc).
 */
int HiresOverride_RegisterFromTim(const char* timPath,
                                   const unsigned char* timData,
                                   unsigned int timSize,
                                   int targetVramX, int targetVramY,
                                   int targetVramW, int targetVramH,
                                   int targetClutX, int targetClutY,
                                   int originalBitDepth);

/* Look up by current draw tpage and clut.
 * Returns the GL texture name (>0) when there is a matching override.
 * Returns 0 when there is no match.
 * On match, *outNativePixelW / *outNativePixelH are filled with the
 * original disc TIM's pixel dimensions — used as texelSize denominator
 * by the 32-bit RGBA sample shader so that the poly's PSX UVs map to
 * the matching subregion of the hi-res GL texture — and *outOffsetX /
 * *outOffsetY with the tpage origin's position INSIDE that TIM, in native
 * texels. Prim UVs restart at each tpage, so a TIM wider than one tpage
 * (e.g. a 320px background) needs the per-chunk offset added to the UVs
 * or every chunk samples the override from x=0. */
unsigned int HiresOverride_LookupByTpageClut(int tpage, int clut,
                                              int* outNativePixelW,
                                              int* outNativePixelH,
                                              int* outOffsetX,
                                              int* outOffsetY);

void HiresOverride_LogStats(void);

/* ---- Chunk-pool virtual slots (resident_textures) ----
 * CANONICAL ENCODING (every other site refers here): a virtual pool slot's
 * imageDesc carries clutX = 0, clutY = HIRES_POOL_CLUT_ROW_BASE + slotId.
 * Material_FsImageApply packs that into the prim clut halfword as
 * (clutY << 6), i.e. bit 15 set and slotId in bits 6..14 — no valid PSX prim
 * carries bit 15, so the key cannot collide with real geometry. The lookup
 * above takes an O(1) fast path on that bit (and requires the low 6 clut
 * bits to be 0, matching clutX = 0, so garbage cluts with bit 15 set are
 * still rejected 63/64 of the time).
 *
 * PoolSlotRegister decodes `data` (disc TIM or loose PNG/TIM replacement) to
 * RGBA8 and creates/REPLACES the slot's texture in place (engine slot reuse).
 * nativePixelW/H are the DISC TIM's pixel dims — the UV denominator — so a
 * replacement of any resolution maps 0..1 over the original.
 * PoolSlotsReset frees every slot texture; called on map (re)init. */
#define HIRES_POOL_CLUT_ROW_BASE 512
#define HIRES_POOL_SLOT_MAX      256

int  HiresOverride_PoolSlotRegister(int slotId,
                                    const unsigned char* data, unsigned int size,
                                    int nativePixelW, int nativePixelH);
void HiresOverride_PoolSlotsReset(void);

/* Pre-decoded RGBA variants (DuckStation texture-pack composites). The rect
 * form replaces in place when an identical key is re-registered. */
int  HiresOverride_PoolSlotRegisterRGBA(int slotId,
                                        const unsigned char* rgba, int w, int h,
                                        int nativePixelW, int nativePixelH);
int  HiresOverride_RegisterRGBA(const char* label,
                                const unsigned char* rgba, int w, int h,
                                int targetVramX, int targetVramY,
                                int targetVramW, int targetVramH,
                                int targetClutX, int targetClutY,
                                int originalBitDepth);

#ifdef __cplusplus
}
#endif

#endif /* HIRES_OVERRIDE_H */
