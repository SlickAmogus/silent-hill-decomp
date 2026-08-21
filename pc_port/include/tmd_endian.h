/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * tmd_endian.h - byte-swap a TMD model file in place for big-endian hosts.
 *
 * Takes the header form GsMapModelingData receives: p[0]=flags, p[1]=nobj
 * (i.e. &fileBuffer[1], the TMD id word already skipped). No-op on
 * little-endian. Idempotent by validation on nobj, so repeated registration
 * of the same buffer is safe.
 */
#ifndef TMD_ENDIAN_H
#define TMD_ENDIAN_H

void Tmd_SwapForBigEndian(void* tmdNoId);

#endif
