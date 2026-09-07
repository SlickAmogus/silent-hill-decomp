/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_PORT_XA_WAV_H
#define PC_PORT_XA_WAV_H

#include <stdint.h>

/* Loose PCM WAV files that stand in for XA voice audio, shared by both XA
 * players (OpenAL and software SPU). 8/16-bit PCM, mono or stereo; the PCM
 * comes back as interleaved int16 frames in a malloc'd buffer the caller frees.
 * Returns 0 (nothing allocated) when the file is missing or unsupported;
 * unsupported files are logged, missing ones are not. */
int XaWav_Load(const char* path, unsigned char** outPcm, uint32_t* outBytes, int* outRate, int* outStereo);

/* The override path for a disc line: <gamedata>/load/XA/xa_NNNN.wav. */
void XaWav_OverridePath(uint16_t xaIdx, char* out, size_t outSize);

#endif
