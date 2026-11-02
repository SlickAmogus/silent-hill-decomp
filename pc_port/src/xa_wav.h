/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_PORT_XA_WAV_H
#define PC_PORT_XA_WAV_H

#include <stddef.h>
#include <stdint.h>

/* Loose PCM WAV files that stand in for XA voice audio, shared by both XA
 * players (OpenAL and software SPU). 8/16-bit PCM, mono or stereo; the PCM
 * comes back as interleaved int16 frames in a malloc'd buffer the caller frees.
 * Returns 0 (nothing allocated) when the file is missing or unsupported;
 * unsupported files are logged, missing ones are not. */
int XaWav_Load(const char* path, unsigned char** outPcm, uint32_t* outBytes, int* outRate, int* outStereo);

/* The override path for a disc line: <gamedata>/load/XA/xa_NNNN.wav. */
void XaWav_OverridePath(uint16_t xaIdx, char* out, size_t outSize);

/* A text-box file mixed into the disc line that is playing, instead of cutting
 * it. Start converts to the stream's rate/channels; Mix copies src to dst with
 * the file added and returns the frames it covered (0 = dst untouched, push
 * src); Advance commits them once pushed. TakeRest hands back what was never
 * mixed so it can play on its own when the disc line ends first. */
int      XaSide_Start(const char* path, int rate, int stereo);
void     XaSide_Stop(void);
int      XaSide_Active(void);
uint32_t XaSide_Mix(const int16_t* src, int16_t* dst, uint32_t frames, int channels);
void     XaSide_Advance(uint32_t frames);
int      XaSide_TakeRest(unsigned char** outPcm, uint32_t* outBytes, int* outRate, int* outStereo);

#endif
