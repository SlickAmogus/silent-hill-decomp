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
 * or every chunk samples the override from x=0.
 * *outHiresW / *outHiresH are the matched GL texture's actual pixel dims —
 * the shader clamps its LINEAR footprint inside each native texel with the
 * implied upscale factor, so atlas neighbors (font glyphs) never bleed. */
unsigned int HiresOverride_LookupByTpageClut(int tpage, int clut,
                                              int* outNativePixelW,
                                              int* outNativePixelH,
                                              int* outOffsetX,
                                              int* outOffsetY,
                                              int* outHiresW,
                                              int* outHiresH);

void HiresOverride_LogStats(void);

/* ---- Chunk-pool virtual slots (resident_textures) ----
 * CANONICAL ENCODING (every other site refers here). Chunk TIMs carry
 * MULTI-ROW CLUTs (palette variants; school TIMs ship 6-12 rows) and each
 * prim selects its row with a baked clut delta of +64*row that
 * Model_MaterialFlagsApply preserves across rebasing. The virtual key must
 * therefore survive "+64*row" without colliding with another slot:
 *
 *   imageDesc.clutX = (slotId % 64) * 16          -> clut word bits 0..5
 *   imageDesc.clutY = 512 + (slotId / 64) * 16    -> clut word bits 6..15
 *
 * Material_FsImageApply packs (clutY << 6) | (clutX >> 4): bit 15 set (no
 * valid PSX prim carries it — clutY 512+ doesn't exist), slot id split
 * across the X bits (row deltas never touch them) and 16-row-spaced Y
 * groups. Decode from a prim clut word:
 *
 *   q      = ((clut >> 6) & 0x3FF) - 512;         q < 0 -> not virtual
 *   slotId = (q / 16) * 64 + (clut & 0x3F);
 *   row    = q % 16;                              (HIRES_POOL_MAX_ROWS)
 *
 * PoolSlotRegister decodes `data` (disc TIM or loose PNG/TIM replacement)
 * to RGBA8 — one texture PER CLUT ROW for TIMs (PNG has one palette: row 0)
 * — replacing in place on engine slot reuse. A row without a texture falls
 * back to row 0. nativePixelW/H are the DISC TIM's pixel dims — the UV
 * denominator — so a replacement of any resolution maps 0..1 over the
 * original. PoolSlotsReset frees everything below the chara range; called on
 * map (re)init.
 *
 * Slot id ranges:
 *   0..254   map chunk pool (192 full + 64 half virtual slots)
 *   255      bullet-decal texture (pc_decals.c)
 *   256+id   global chara pool (pc_chara_pool.c), id = e_CharaId. These
 *            PERSIST across map loads (PoolSlotsReset skips them). A chara
 *            TIM with more than 16 CLUT rows (PRS ships 48) spills rows
 *            16..31 into slot+64 and 32..47 into slot+128 — that aliasing is
 *            inherent to the clut-word encoding (row deltas of +64*row walk
 *            the Y groups), so 320..447 are reserved as spill space and must
 *            never be allocated directly. */
#define HIRES_POOL_CLUT_ROW_BASE  512
#define HIRES_POOL_SLOT_MAX       512
#define HIRES_POOL_MAX_ROWS       16
#define HIRES_POOL_CHARA_SLOT_BASE 256
/* Bullet-decal texture (pc_decals.c) — last chunk-range id, excluded from the
 * Texture_Get claim list. Named so the chunk-pool bound can't drift when
 * HIRES_POOL_SLOT_MAX grows (it did: 256->512 for the chara range). */
#define HIRES_POOL_DECAL_SLOT     (HIRES_POOL_CHARA_SLOT_BASE - 1)

int  HiresOverride_PoolSlotRegister(int slotId,
                                    const unsigned char* data, unsigned int size,
                                    int nativePixelW, int nativePixelH);
void HiresOverride_PoolSlotsReset(void);
void HiresOverride_CharaPoolSlotReset(int slotId);

/* Pre-decoded RGBA variants (DuckStation texture-pack composites). The rect
 * form replaces in place when an identical key is re-registered. */
int  HiresOverride_PoolSlotRegisterRGBA(int slotId, int row,
                                        const unsigned char* rgba, int w, int h,
                                        int nativePixelW, int nativePixelH);

/* Per-CLUT-row loose replacement: decode a PNG (or TIM) and install it as a
 * single palette ROW on top of the disc-decoded base, so a modder can replace
 * one body region of a multi-CLUT chara/BG TIM (rows are named "{file}.pNN.png"
 * — see fsqueue_3.c). Unlike a whole-image replacement these overlay ONLY the
 * given row; untouched rows keep the native art. Not pack-budgeted (user file).
 * Returns 0 on success. */
int  HiresOverride_PoolSlotLoosePngRow(int slotId, int row,
                                       const unsigned char* data, unsigned int size,
                                       int nativePixelW, int nativePixelH);
int  HiresOverride_RegisterLoosePngRow(const char* label,
                                       const unsigned char* data, unsigned int size,
                                       int targetVramX, int targetVramY,
                                       int targetVramW, int targetVramH,
                                       int targetClutX, int targetClutY,
                                       int originalBitDepth);

/* Whole-image loose replacement applied to ALL of a texture's CLUT rows: decode
 * once and register the same image at clut cells targetClutY .. targetClutY+rows-1,
 * so one PNG replaces every palette of a multi-CLUT VRAM texture (a monster/boss
 * you don't want to recolour per-region). rows<=1 or no CLUT registers a single
 * entry. Returns 0 if any row registered. (The chara/resident POOL path gets this
 * for free via the lookup's row-0 fallback; this is the VRAM-resident path.) */
int  HiresOverride_RegisterLoosePngAllRows(const char* label,
                                           const unsigned char* data, unsigned int size,
                                           int targetVramX, int targetVramY,
                                           int targetVramW, int targetVramH,
                                           int targetClutX, int targetClutY,
                                           int rows, int originalBitDepth);
int  HiresOverride_RegisterRGBA(const char* label,
                                const unsigned char* rgba, int w, int h,
                                int targetVramX, int targetVramY,
                                int targetVramW, int targetVramH,
                                int targetClutX, int targetClutY,
                                int originalBitDepth);

/* Drop every rect-keyed entry whose pixel rect or CLUT cells intersect a
 * VRAM region that was just rewritten — pool slots and map atlas VRAM
 * recycle the same rect+clut for different TIMs, and a surviving entry
 * would bind the OLD image to the new material's prims. Call for every
 * VRAM upload (pixels and CLUT rects). */
void HiresOverride_InvalidateVramRect(int x, int y, int w, int h);

/* True once live pack-composed GL textures exceed the byte budget. Pack
 * compose loops (fsqueue_3.c) stop making MORE pack textures then — rows
 * past the cap keep native disc art. Frees credit the budget back. Guards
 * the whole-town texture-all path against multi-GB pack blowups. */
int HiresOverride_PackBudgetExceeded(void);

#ifdef __cplusplus
}
#endif

#endif /* HIRES_OVERRIDE_H */
