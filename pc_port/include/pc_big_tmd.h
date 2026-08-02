#ifndef PC_BIG_TMD_H
#define PC_BIG_TMD_H

/* Oversized loose ITEM TMDs — the item-model twin of pc_big_lm.c.
 *
 * Every unique inventory item model in GameFs_UniqueItemModelLoad
 * (item_screens_3.c) reads into ONE fixed PSX-RAM slab, FS_BUFFER_5. Nearly
 * eighty UNQ*.TMD files share it, so a replacement larger than its file-table
 * slot cannot simply be byte-replaced: fsqueue_3.c's loose path refuses it
 * ("> buf ... for non-TIM read; ignoring loose file") and the disc file loads
 * instead.
 *
 * This registry, keyed by g_FileTable index, validates such a file, hands out
 * a calloc'd PC-owned buffer, and substitutes it for FS_BUFFER_5 at the single
 * Fs_QueueStartRead seam in GameFs_UniqueItemModelLoad. Fs_QueueTickRead then
 * freads into it through the same capacity-lifted byte-replace path pc_big_lm
 * uses. The consumer side (func_80054A04 / Gfx_Items_Display) resolves the
 * same substitution so GsMapModelingData parses the lifted buffer.
 *
 * STOCK GUARANTEE: Pc_BigTmd_Redirect returns its `dest` argument BY VALUE,
 * unchanged, unless ALL of these hold — loose files enabled, the loose file
 * exists, it is strictly larger than the native sector-aligned slot, and it
 * passes TMD validation. Any miss returns the identical FS_BUFFER_5 pointer,
 * so the data the native path loads and draws is exactly what it is today. */

#include <stddef.h>

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* GsMapModelingData (libgs_stub.c) bounds a primitive packet at 64 bytes when
 * it computes the block extent it memcpy's out of the load buffer. That upper
 * bound over-reads past EOF for every real TMD — 3368 bytes past the end of
 * stock UNQ21.TMD — which is harmless inside the 2 MB PSX RAM slab but would
 * walk off a tight malloc. Every buffer this module hands out is therefore
 * sized for the stub's own worst-case extent, plus this tail. */
#define PC_BIGTMD_TAIL_SLACK 64

/* The ONLY redirect entry. Keyed by fileIdx (one of the ~78 UNQ*.TMD ids), so
 * it is correct for every item that shares FS_BUFFER_5, not just one.
 *
 * Returns the buffer Fs_QueueStartRead should load into: a PC-owned buffer for
 * an accepted oversized loose file, otherwise `dest` unchanged. Also updates
 * the active-substitution record for `dest` — including CLEARING it on a miss,
 * so a stock item selected after a modded one resolves back to the slab
 * instead of drawing the previous item's stale buffer. */
void* Pc_BigTmd_Redirect(s32 fileIdx, void* dest);

/* Consumer-side resolve: the substituted buffer currently standing in for
 * `nativeDest`, or `nativeDest` itself when nothing is substituted. Identity
 * for any pointer this module never redirected. */
void* Pc_BigTmd_Resolve(void* nativeDest);

/* Capacity of a registered item-TMD destination; 0 for unregistered pointers.
 * Raises the loose byte-replace size gate in Fs_QueueTickRead. */
size_t Pc_BigTmd_DestCapacity(const void* dest);

#ifdef __cplusplus
}
#endif

#endif /* PC_BIG_TMD_H */
