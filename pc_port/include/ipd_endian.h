/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * ipd_endian.h - byte-swap the IPD arrays that are NOT reformatted.
 *
 * Big-endian consoles only (360, PS3). No-ops on little-endian hosts, so call
 * sites need no #ifdef of their own.
 */
#ifndef IPD_ENDIAN_H
#define IPD_ENDIAN_H

#ifdef __cplusplus
extern "C" {
#endif

struct _IpdModelBuffer;
struct _IpdCollisionData;

/* Call once per IPD buffer, before any Ipd_SwapModelBufferPayload for it.
 * Clears the already-swapped set, which is what stops an array shared by two
 * model buffers from being converted twice. */
void Ipd_SwapPayloadBegin(void);

/* Swap one model buffer's subcellPositions / field_10 SVECTOR arrays in place.
 * Safe to call repeatedly for the same buffer within one Begin..end window. */
void Ipd_SwapModelBufferPayload(struct _IpdModelBuffer* buf);

/* Swap the collision sub-arrays (split vertices, surfaces, subcells, round
 * obstacles, subcell ranges) a parsed s_IpdCollisionData points at. `collraw`
 * is the 308-byte PSX collision struct the offsets come from; `lmHdrOff` caps
 * the last array. Call after IpdColl_RelocateClobbered, once per parse. */
void Ipd_SwapCollisionPayload(struct _IpdCollisionData* coll, const unsigned char* collraw, unsigned lmHdrOff);

#ifdef __cplusplus
}
#endif

#endif /* IPD_ENDIAN_H */
