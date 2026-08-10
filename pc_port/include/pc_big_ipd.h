#ifndef PC_BIG_IPD_H
#define PC_BIG_IPD_H

#include <stddef.h>

#include "game.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Readable slack past the file so a validator peeking one byte past a
 * truncated section cannot run off the allocation. */
#define PC_BIGIPD_TAIL_SLACK 16

/* Record the destination buffer a chunk slot was assigned, and how many bytes
 * it can hold. Called for every slot by Ipd_ActiveChunksClear. */
void Pc_BigIpd_RegisterSlot(const void* slot, size_t cap);

/* Before a chunk read is enqueued: if fileIdx has a loose replacement larger
 * than *pDest can hold, swap in a PC-owned buffer big enough and update
 * *pDest. No-op when there is no loose file, it fits, or allocation fails. */
void Pc_BigIpd_EnsureCapacity(void** pDest, s32 fileIdx);

/* Bytes `dest` can hold, or 0 if it is not a known chunk destination. Used by
 * the FS queue to lift its loose-file size gate and by the IPD reformatter to
 * bound-check header offsets against the real buffer. */
size_t Pc_BigIpd_DestCapacity(const void* dest);

#ifdef __cplusplus
}
#endif

#endif /* PC_BIG_IPD_H */
