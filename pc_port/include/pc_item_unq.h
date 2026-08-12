/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_ITEM_UNQ_H
#define PC_ITEM_UNQ_H

#include "common.h"

#ifdef SH_PC_PORT

/* itemId -> UNQ*.TMD file index.
 *
 * The retail loader GameFs_UniqueItemModelLoad (0x80053B08) resolves this
 * mapping inline and immediately fires the read, so the file index is never
 * available as a value. The modern glTF item path needs that value at three
 * points where no read is happening -- carousel link, world-pickup link, and
 * the availability probe -- so the mapping is mirrored here.
 *
 * This file is PC-only. The retail switch is left byte-for-byte untouched so
 * the PSX build is unaffected. Keep this table synchronized with that switch;
 * the mapping is intentionally straightforward to review mechanically.
 */
s32 Pc_ItemUnq_FromItemId(u8 itemId);

/* One-shot latch for the requested item id.
 *
 * Gfx_Items_Display (0x80054720) is a matching retail function, so it cannot
 * gain a parameter. In every retail call path the id it needs is already
 * derivable -- callers only invoke it once the savegame item matches
 * g_MapOverlayHdr.loadableItems[loadableItemIdx]. The one exception is the
 * PC-only modern fallback, which passes loadableItemIdx = 0 for an item that is
 * deliberately NOT in the map pack; that path latches the real id here first.
 *
 * Set is one-shot: Take clears it, so a latch can never leak into a later call.
 */
void Pc_ItemUnq_SetRequestedItemId(u8 itemId);
u8   Pc_ItemUnq_TakeRequestedItemId(s32 loadableItemIdx);

#endif /* SH_PC_PORT */
#endif /* PC_ITEM_UNQ_H */
