#include "pc_audio_config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

PcAudioConfig g_PcAudioConfig = {
    PC_SPU_RENDERER_LEGACY, 0, 0, 0, 0, 1, 0, 0
};

static char* Trim(char* text)
{
    char* end;

    while (isspace((unsigned char)*text))
        text++;
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1]))
        end--;
    *end = '\0';
    return text;
}

static int ParseClip(const char* value)
{
    if (strcmp(value, "psx") == 0)
        return 1;
    if (strcmp(value, "soft") == 0)
        return 2;
    return 0;
}

void PcAudioConfig_Load(const char* path)
{
    FILE* file = fopen(path, "r");
    char line[512];

    if (!file)
        return;

    while (fgets(line, sizeof(line), file))
    {
        char* equals;
        char* key = Trim(line);
        char* value;
        char* comment;

        if (*key == '\0' || *key == '#' || *key == ';')
            continue;
        equals = strchr(key, '=');
        if (!equals)
            continue;
        *equals = '\0';
        value = Trim(equals + 1);
        key = Trim(key);
        comment = strpbrk(value, "#;");
        if (comment)
            *comment = '\0';
        value = Trim(value);

        if (strcmp(key, "spu_renderer") == 0)
        {
            if (strcmp(value, "exact") == 0)
                g_PcAudioConfig.renderer = PC_SPU_RENDERER_EXACT;
            else if (strcmp(value, "ideal") == 0)
                g_PcAudioConfig.renderer = PC_SPU_RENDERER_IDEAL;
            else if (strcmp(value, "reference") == 0)
                g_PcAudioConfig.renderer = PC_SPU_RENDERER_REFERENCE;
            else
                g_PcAudioConfig.renderer = PC_SPU_RENDERER_LEGACY;
        }
        else if (strcmp(key, "ideal_clip") == 0)
            g_PcAudioConfig.idealClip = ParseClip(value);
        else if (strcmp(key, "reference_clip") == 0)
            g_PcAudioConfig.referenceClip = ParseClip(value);
        else if (strcmp(key, "reference_dither") == 0)
            g_PcAudioConfig.referenceDither = strcmp(value, "tpdf") == 0;
        else if (strcmp(key, "audio_backend") == 0)
            g_PcAudioConfig.backend = strcmp(value, "wasapi") == 0 ? 1 :
                                      strcmp(value, "sdl") == 0 ? 2 : 0;
        else if (strcmp(key, "audio_mode") == 0)
            g_PcAudioConfig.mode = strcmp(value, "exclusive") == 0 ? 2 :
                                   strcmp(value, "shared") == 0 ? 1 : 0;
        else if (strcmp(key, "audio_rate") == 0)
        {
            int rate = strcmp(value, "auto") == 0 ? 0 : atoi(value);
            g_PcAudioConfig.rate =
                rate == 44100 || rate == 88200 || rate == 176400 ||
                rate == 352800 ? rate : 0;
        }
        else if (strcmp(key, "audio_bit_perfect") == 0)
            g_PcAudioConfig.bitPerfect = atoi(value) != 0;
    }
    fclose(file);

    if (PcAudioConfig_UsesSoftwareSpu() && g_PcAudioConfig.rate == 0)
        g_PcAudioConfig.rate =
            g_PcAudioConfig.renderer == PC_SPU_RENDERER_EXACT ? 44100 : 176400;
}

int PcAudioConfig_UsesSoftwareSpu(void)
{
    return g_PcAudioConfig.renderer != PC_SPU_RENDERER_LEGACY;
}
