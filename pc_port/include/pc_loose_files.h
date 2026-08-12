/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_LOOSE_FILES_H
#define PC_LOOSE_FILES_H

#ifdef SH_PC_PORT

#include <stddef.h> /* size_t */
#include "decomp/types.h"

/* Loose-override lookup for PC-side code that reads a disc file OUTSIDE the FS
 * queue's TIM pipeline (pc_minimap.c reads the paper-map TIM itself). Both are
 * implemented in src/main/fsqueue_3.c, on top of the very probe chain
 * Fs_QueueTickRead / Fs_QueuePostLoadTim use, so such a reader shows the same
 * replacement the normal pipeline shows instead of a second copy that drifts.
 *
 * Probes are plain fopen: no FS-queue work, safe to call from a queue-idle
 * window. Both respect g_PcConfig.allowLooseFiles. */

/* Fills `out` with the whole-image loose override for a disc file index (.dds
 * before .png, disc-name form before stem form, a per-CLUT-row set's row 0
 * ahead of both) and returns 1; 0 when none is installed. */
int Pc_LooseImageOverridePath(s32 fileIdx, char* out, size_t outSize);

/* Read a whole loose file (64MB cap, failures logged). Caller frees. */
unsigned char* Pc_LooseSlurp(const char* path, long* outSize);

#endif /* SH_PC_PORT */

#endif /* PC_LOOSE_FILES_H */
