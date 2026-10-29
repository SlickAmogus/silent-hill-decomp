/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * lm_endian.h - byte-swap an LM's MESH PAYLOAD after it is read off the disc.
 *
 * Big-endian consoles only (360, PS3). No-ops on little-endian hosts, so call
 * sites need no #ifdef of their own.
 */
#ifndef LM_ENDIAN_H
#define LM_ENDIAN_H

#ifdef __cplusplus
extern "C" {
#endif

struct _MeshHeader;

/* Call once per LM buffer, before any Lm_SwapMeshPayload for it. Clears the
 * already-swapped set, which is what stops a mesh header shared by two models
 * from being swapped twice (which would silently restore disc order). */
void Lm_SwapPayloadBegin(void);

/* Swap one mesh's vertex/primitive arrays in place. Safe to call repeatedly for
 * the same mesh within one Begin..end window; only the first call converts. */
void Lm_SwapMeshPayload(struct _MeshHeader* mesh);

#ifdef __cplusplus
}
#endif

#endif /* LM_ENDIAN_H */
