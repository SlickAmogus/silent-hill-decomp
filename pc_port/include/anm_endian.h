/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * anm_endian.h - byte-swap an ANM header in place after it is read off the disc.
 *
 * Big-endian consoles only (360, PS3). On little-endian hosts the entry point
 * still exists but is a no-op, so call sites need no #ifdef of their own.
 */
#ifndef ANM_ENDIAN_H
#define ANM_ENDIAN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Converts an ANM buffer's 20-byte header from its on-disc little-endian form
 * to host order. Idempotent, and SAFE TO CALL ON A HEADERLESS BUFFER: the
 * keyframe-only banks (HB_WEP*, HB_M*S*) begin with channel data, and this
 * refuses to touch anything that does not satisfy the header's cross-field
 * invariants. */
void Anm_SwapForBigEndian(void* addr);

#ifdef __cplusplus
}
#endif

#endif /* ANM_ENDIAN_H */
