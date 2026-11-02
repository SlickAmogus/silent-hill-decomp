/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_LIPSYNC_H
#define PC_LIPSYNC_H

#include <stddef.h>
#include <stdint.h>
#include <stddef.h> /* NULL in the generated speaker table. */

#ifdef __cplusplus
extern "C" {
#endif

/* Lip sync from the voice stream itself. The XA players feed each line's PCM
 * as they decode it, ahead of playback, and the loudness envelope drives a
 * mouth. The game never records who speaks a line, so the speaker comes from
 * a table by line (pc_lipsync_speakers.c, overridden per line by
 * gamedata/load/XA/speakers.txt), and for a line the table lacks, from the
 * line's pitch: male or female. */
void Pc_Lip_Feed(uint16_t xaIdx, const int16_t* pcm, int frames, int rate, int channels);
void Pc_Lip_Stop(void);
void Pc_Lip_Hold(int hold);

/* Mouth opening 0..1 at the current play position, or -1 when no voice is
 * playing. *speaker is the speaking character's model name (HERO, SIBYL,
 * DARIA, ...) when the line is tabled, else NULL; then *female is 1 or 0 once
 * the line's pitch is known, -1 before. */
float Pc_Lip_Level(const char** speaker, int* female);

/* g_PcLipSpeaker[xaIdx]: model name, or NULL when unknown. */
#define PC_LIP_LINES 727
extern const char* const g_PcLipSpeaker[PC_LIP_LINES];

#ifdef __cplusplus
}
#endif

#endif
