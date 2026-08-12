/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TEX_PACK_H
#define TEX_PACK_H

#include <stddef.h> /* size_t */

#ifdef __cplusplus
extern "C" {
#endif

/* DuckStation-format texture packs.
 *
 * Scans gamedata/texturemods/ (recursively; loose folders and .zip archives)
 * for DuckStation texture-cache dumps/replacements:
 *
 *   texupload-{P4|P8|STP4|STP8}-{srcHash}-{palHash}-{WxH}-{ox}-{oy}-{wxh}-P{palMin}-{palMax}.png
 *   texupload-{C16|STC16}-{srcHash}-{WxH}-{ox}-{oy}-{wxh}.png
 *
 * where srcHash = XXH3-64 of the VRAM write payload (for us: the TIM pixel
 * block, row-major halfwords), palHash = XXH3-64 of the CLUT halfwords
 * (all 16 for 4bpp / 256 for 8bpp when the P range is full; DuckStation's
 * partial-range quirk hashes the FIRST palMax-palMin+1 entries), WxH = the
 * write size in VRAM halfwords, and ox/oy/wxh = the replaced sub-rectangle
 * in native texels. One upload can have many sub-rectangle replacements;
 * TexPack_Compose rebuilds the whole upload at the pack's scale and blits
 * each matching image over it, mirroring DuckStation's compositor.
 *
 * Entries may also be .dds (BC7). A .dds covering the whole upload is handed
 * back compressed for a 4x cheaper VRAM upload (see TexPack_LastComposeIsDds);
 * one that only covers a sub-rectangle is BC7-decoded on the CPU and blitted
 * like a PNG, since the compositor works in 32-bit pixels. */

/* True when at least one pack entry is indexed (first call scans the
 * texturemods folder; respects g_PcConfig.texturePacks). */
int TexPack_HasEntries(void);

/* Compose a replacement image for one TIM upload.
 *   pixels     raw PSX pixel block (w16*h halfwords, row-major)
 *   w16, h     upload size in VRAM halfwords
 *   clut       palette halfwords (NULL for 16bpp)
 *   clutCount  entries available at `clut`
 *   bpp        4, 8 or 16
 * Returns an RGBA8 canvas (*outW x *outH — the upload at pack scale) when at
 * least one pack entry matches, else NULL. The canvas is OWNED by the
 * content-keyed compose cache (`texpack_cache_mb`) — do NOT free it; it stays
 * valid until the next TexPack_Compose call at minimum. */
const unsigned char* TexPack_Compose(const unsigned char* pixels, int w16, int h,
                                     const unsigned short* clut, int clutCount,
                                     int bpp, int* outW, int* outH);

/* Content key (pixels + palette + bpp) of the MOST RECENT TexPack_Compose call.
 * The same value the compose cache keys on, folded to one u64. Non-zero after a
 * call that hashed its inputs; read it immediately after TexPack_Compose and
 * feed it to the *Keyed pool registrars so a re-upload of byte-identical content
 * skips the glTexImage2D churn. 0 means "no stable key" (always re-upload). */
unsigned long long TexPack_LastComposeHash(void);

/* BC7 .dds whole-upload result of the most recent TexPack_Compose. When the
 * single matching entry is a .dds covering the entire upload, Compose returns
 * NULL, TexPack_LastComposeIsDds() is 1, and TexPack_LastComposeDds() yields
 * the raw .dds file bytes to hand to Dds_UploadBptc (keyed by
 * TexPack_LastComposeHash so a byte-identical re-upload is skipped). The bytes
 * stay valid until the next TexPack_Compose call. */
int                  TexPack_LastComposeIsDds(void);
const unsigned char* TexPack_LastComposeDds(size_t* outSize);

/* 1 when the most recent TexPack_Compose paid the expensive path (pack files
 * read + decoded + upscaled, or a .dds read), 0 when it hit the compose cache
 * or matched no entry. The upload sites budget composes per frame; a cache hit
 * costs almost nothing and must not spend that budget. */
int TexPack_LastComposeWasBuilt(void);

/* ---- Texture dumping (dump_textures) ----
 * Writes gamedata/dump/ as a ready-made texture pack: every entry is named with
 * the grammar above, so dropping the folder into gamedata/texturemods/ matches
 * the very uploads it came from with no conversion step.
 *
 * Like DuckStation, an entry is the piece of the sheet ONE OBJECT draws, seen
 * through the palette that object selects — not the whole sheet. The piece is
 * the UV bounding box of one model over the sheet, read out of the .ILM/.PLM/
 * .IPD by TexPack_DumpScanLm; the sheet a box belongs to is the material name,
 * which is also the TIM name. A sheet no model ever references (fonts, HUD, 2D
 * screens) still falls back to one whole-upload entry per CLUT row.
 *
 * Both calls are no-ops unless g_PcConfig.dumpTextures: nothing is hashed,
 * scanned, allocated or written when the option is off. Entries already dumped
 * (this session or a previous one) are skipped, so a long play session writes
 * each file exactly once. 24bpp uploads are skipped — the pack name grammar
 * cannot express them. */

/* Register the geometry of one just-loaded LM (PSX-layout bytes, header at
 * `lm`) — .ILM, .PLM or an .IPD's embedded LM, all identical here. Call BEFORE
 * the header is reformatted and before Material_FsImageApply rebases anything:
 * the material/primitive CLUT words must still be the values from the file.
 * Also crops any upload already held for the materials named by this LM. */
void TexPack_DumpScanLm(const unsigned char* lm);

/* Dump one TIM upload. `timName` is the file name (extension optional) — it is
 * what ties the upload to the geometry registered above. pixels/w16/h/clut/bpp
 * are exactly TexPack_Compose's arguments; clutRows is the CLUT block height,
 * so all palettes are covered in one call. */
void TexPack_DumpUpload(const char* timName, const unsigned char* pixels, int w16, int h,
                        const unsigned short* clut, int clutW, int clutRows, int bpp);

/* Settle every upload still held for late geometry: the ones no model ever
 * claimed are written whole-cover. Runs at exit on its own; call it to force
 * that point earlier. */
void TexPack_DumpFlush(void);

#ifdef __cplusplus
}
#endif

#endif /* TEX_PACK_H */
