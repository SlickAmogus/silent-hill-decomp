#ifndef TEX_PACK_H
#define TEX_PACK_H

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
 * each matching PNG over it, mirroring DuckStation's compositor. */

/* True when at least one pack entry is indexed (first call scans the
 * texturemods folder; respects g_PcConfig.texturePacks). */
int TexPack_HasEntries(void);

/* Compose a replacement image for one TIM upload.
 *   pixels     raw PSX pixel block (w16*h halfwords, row-major)
 *   w16, h     upload size in VRAM halfwords
 *   clut       palette halfwords (NULL for 16bpp)
 *   clutCount  entries available at `clut`
 *   bpp        4, 8 or 16
 * Returns a malloc'd RGBA8 canvas (*outW x *outH — the upload at pack scale)
 * when at least one pack entry matches, else NULL. */
unsigned char* TexPack_Compose(const unsigned char* pixels, int w16, int h,
                               const unsigned short* clut, int clutCount,
                               int bpp, int* outW, int* outH);

#ifdef __cplusplus
}
#endif

#endif /* TEX_PACK_H */
