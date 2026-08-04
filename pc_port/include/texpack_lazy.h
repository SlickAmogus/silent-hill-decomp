#ifndef TEXPACK_LAZY_H
#define TEXPACK_LAZY_H

#ifdef __cplusplus
extern "C" {
#endif

/* Demand-driven ("lazy") DuckStation texture-pack compose for the chunk/chara
 * POOL-SLOT path.
 *
 * WHY: Fs_QueuePostLoadTim used to compose a pack canvas for EVERY CLUT row of
 * every pool slot as the TIM landed. Two things made that unpayable:
 *
 *   1. The per-tick compose budget is bypassed. Fs_QueueUpdatePostLoad refills
 *      s_texpackComposeBudget at the TOP of every tick, and Gfx_InGameDraw runs
 *      2 passes x up to 500 Fs_QueueUpdate() calls synchronously inside ONE
 *      frame (world_draw.c), so a cell crossing ran hundreds of ~7.6 ms composes
 *      back to back in a single frame (worst measured ~1.36 s).
 *   2. Most of that work is never looked at: a measured mean 9.23 CLUT rows
 *      composed per slot when only 2-3 are ever drawn.
 *
 * So the loader now only RETAINS the compose inputs, and a (slot, row) pair
 * composes the moment a prim actually samples it - HiresOverride_LookupByTpageClut
 * calls TexPackLazy_NoteWanted - paid out of a wall-clock budget by
 * TexPackLazy_Pump once per presented frame.
 *
 * LANDING SAFETY: the retain call sits inside the same `if (!registered)` block
 * as HiresOverride_PoolSlotRegister(disc TIM), which uploads every shipped CLUT
 * row at native resolution. A row that has not composed yet therefore draws the
 * correct native art with the correct palette - never a hole, never a wrong
 * palette - PROVIDED that base registration succeeded. Its return value is
 * ignored at the call site; when it fails glTexture[row] stays 0, the lookup
 * returns 0 and the prim samples raw VRAM, exactly as before packs existed.
 * The only new visible cost is HD pop-in.
 *
 * SHADER SAFETY: u_hiresHalf is derived from each row's own GL texture size
 * (rowW/rowH per row), so a native-res row and a composed row both sample
 * correctly and may coexist inside one slot; no uniform is shared between them.
 *
 * NOT USED for the VRAM-rect path (fonts, HUD, items, 2D backgrounds). That path
 * composes under HiresOverride_SetForceNearestUpload, which is set and cleared
 * around the synchronous compose in Fs_QueuePostLoadTim; deferring it past that
 * window would re-open the gutterless-glyph ghost text on FONT16/FONT24. Pool
 * slots only. */

/* Retain one pool slot's TIM source so its rows can compose on demand.
 * `pixels` is the raw PSX pixel block (w16*h halfwords) and `clut` the palette
 * block (clutW*clutRows halfwords). Both are COPIED, because they point INTO
 * the queue entry's load buffer, which the queue hands to the next file it
 * reads. Everything else TexPack_Compose needs is a scalar.
 *
 * Replaces (and exactly credits back) whatever this slot held before. No-op
 * when the TIM is not 4/8bpp, when there is no palette block, or when the
 * retain cap is spent; the slot then simply keeps its native disc art forever,
 * which is the same safe fallback as a row that has not composed yet.
 * `timName` is used only to name the slot in log lines. */
void TexPackLazy_RegisterSlotSource(int slotId, const char* timName,
                                    const unsigned char* pixels, int w16, int h,
                                    const unsigned short* clut, int clutW, int clutRows,
                                    int bpp, int nativeW, int nativeH);

/* Release one slot's retained source and its exact retain-cap charge. A source
 * outliving its slot would compose the PREVIOUS occupant's sheet into the new
 * one, so every path that rewrites a slot wholesale must call this. */
void TexPackLazy_DropSlot(int slotId);

/* Map (re)init: release every chunk-range slot (0 .. HIRES_POOL_CHARA_SLOT_BASE-1)
 * and arm the post-load burst budget. Mirrors HiresOverride_PoolSlotsReset,
 * which frees exactly that range and is the only caller.
 *
 * This is the EXPLICIT post-load signal, deliberately not "Fs_QueueGetLength() > 0":
 * the queue is routinely non-empty on ordinary gameplay frames, so deriving
 * "we are loading" from it would hand the fat budget to exactly the frames this
 * change exists to keep smooth. */
void TexPackLazy_MapReset(void);

/* "A prim is sampling this (slot, row) right now." Called per textured prim
 * from HiresOverride_LookupByTpageClut, so it must stay O(1) and allocation
 * free. Main thread only: the only paths in are ApplyHiresOverride and the
 * ShouldDropForClut discriminator, both under DrawOTag. */
void TexPackLazy_NoteWanted(int slotId, int row);

/* Service the wanted set. MUST run after the frame's OT submit: it creates and
 * REPLACES GL texture objects, and ApplyHiresOverride has already baked GL
 * names into this frame's prims - the same work inside the mid-frame FS-queue
 * drain in Gfx_InGameDraw would delete a name a submitted prim still points at.
 * Called right after PsyX_EndScene() returns in MainLoop, i.e. after the swap.
 *
 * NO PRE-WARM IS POSSIBLE during a blocking map load: Fs_QueueWaitForEmpty
 * spins VSync + Fs_QueueUpdate and never reaches PsyX_EndScene, and the wanted
 * set is populated by draw-time lookups, so nothing is wanted until a frame has
 * drawn. The first frame after any map load is 100% native art, by construction.
 *
 * Bounded by texpack_lazy_ms of wall clock, minimum one row per frame. */
void TexPackLazy_Pump(void);

#ifdef __cplusplus
}
#endif

#endif /* TEXPACK_LAZY_H */
