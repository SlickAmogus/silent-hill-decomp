#ifndef PC_AUDIO_CONFIG_H
#define PC_AUDIO_CONFIG_H

enum
{
    PC_SPU_RENDERER_LEGACY = 0,
    PC_SPU_RENDERER_EXACT = 1,
    PC_SPU_RENDERER_IDEAL = 2,
    PC_SPU_RENDERER_REFERENCE = 3
};

typedef struct
{
    int renderer;
    int idealClip;
    int referenceClip;
    int referenceDither;
    int backend;
    int mode;
    int rate;
    int bitPerfect;
} PcAudioConfig;

extern PcAudioConfig g_PcAudioConfig;

void PcAudioConfig_Load(const char* path);
int PcAudioConfig_UsesSoftwareSpu(void);

#endif
