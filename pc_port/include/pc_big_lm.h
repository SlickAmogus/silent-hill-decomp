#ifndef PC_BIG_LM_H
#define PC_BIG_LM_H

/* Oversized loose character models (Stage 1, docs/Oversized_Models_Plan.md).
 * A loose CHARA ILM larger than its file-table slot is validated, given a
 * PC-owned buffer, and substituted for the fixed PSX-RAM slab at the three
 * Fs_QueueStartRead hooks in world_draw.c. Native model is the fallback on
 * every failure path. */

#include <stddef.h>
#include <stdio.h>

#include "game.h"
#include "bodyprog/formats/lm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* F1: the 3-at-a-time GTE vertex/normal strides read up to 8 bytes past the
 * file arrays and the shade copy up to 3 — a last array ending at EOF must
 * not read past the malloc. */
#define PC_BIGLM_TAIL_SLACK 16

/* The ONLY redirect entry. Keyed by modelFileIdx (never charaId — F3: the
 * CHARA_FILE_INFOS row is retargeted at runtime). Substitutes regardless of
 * the incoming pointer in both directions: a previously redirected buffer
 * maps back to the slab it displaced when no replacement exists. */
s_LmHeader* Pc_BigLm_Redirect(s32 modelFileIdx, s_LmHeader* lmHdr);

/* Capacity of a registered big-model destination (owned redirect buffer or
 * pool-registered external buffer); 0 for unregistered pointers. Raises the
 * loose byte-replace size gate in Fs_QueueTickRead. */
size_t Pc_BigLm_DestCapacity(const void* dest);

/* Pointer-identity test against the redirect buffers (F4: the slot-reuse and
 * bump-cursor paths must recognize a redirected lmHdr). */
int Pc_BigLm_IsOwned(const void* p);

/* The slab pointer a redirect displaced; NULL if p is not owned. */
s_LmHeader* Pc_BigLm_NativeOf(const void* ownedPtr);

/* The fileIdx that owns buf, or -1 if buf is not a live redirect buffer. Used
 * to key v7 wide-geometry registration off the same index the redirect uses. */
s32 Pc_BigLm_FileIdxOf(const void* buf);

/* Validated actual size of an oversized loose file for fileIdx, or -1 when
 * absent / not oversized / invalid. Pool buffer sizing only. */
s32 Pc_BigLm_LooseSize(s32 fileIdx);

/* Register a pool-owned destination buffer so Pc_BigLm_DestCapacity lifts the
 * gate for it. Keyed by charaId so a pool realloc simply overwrites the slot. */
void Pc_BigLm_RegisterExternal(s32 key, const void* buf, size_t cap);

/* fsqueue_3.c's loose fopen (Linux case-insensitive fallback), exported so
 * the big-model probe sees exactly the same on-disk namespace. */
FILE* Pc_LooseFOpen(const char* path, const char* mode);

#ifdef __cplusplus
}
#endif

#endif /* PC_BIG_LM_H */
