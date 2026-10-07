/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_AUDIO_CONFIG_H
#define PC_AUDIO_CONFIG_H

enum
{
    PC_SPU_RENDERER_LEGACY = 0,
    PC_SPU_RENDERER_AUTHENTIC = 1,
    PC_SPU_RENDERER_HIGH_PRECISION = 2,
    PC_SPU_RENDERER_MODERN = 3
};

typedef struct
{
    int renderer;
    int highPrecisionClip;
    int modernClip;
    int modernDither;
    int backend;
    int mode;
    int rate;
    int bitPerfect;
    /* audio_spatial: run the software SPU through OpenAL placement so its
     * accurate reverb reaches surround layouts. Software renderers only --
     * the legacy backend already has its own speaker handling.
     *
     * Left unset it follows audio_output: asking for quad/5.1/7.1 turns it on,
     * because a surround layout on the software SPU does nothing without it and
     * there was no way to discover that. spatialUserSet records an explicit
     * setting so audio_spatial = 0 can still force the plain stereo sink. */
    int spatial;
    int spatialUserSet;
} PcAudioConfig;

extern PcAudioConfig g_PcAudioConfig;

void PcAudioConfig_Load(const char* path);
int PcAudioConfig_UsesSoftwareSpu(void);

#endif
