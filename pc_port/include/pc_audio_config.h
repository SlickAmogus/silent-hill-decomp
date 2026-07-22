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
} PcAudioConfig;

extern PcAudioConfig g_PcAudioConfig;

void PcAudioConfig_Load(const char* path);
int PcAudioConfig_UsesSoftwareSpu(void);

#endif
