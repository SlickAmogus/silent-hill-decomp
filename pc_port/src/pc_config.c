#include "pc_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

s_PcConfig g_PcConfig = {
    .windowWidth    = 640,
    .windowHeight   = 480,
    .fullscreen     = 0,
    .disableCulling = 1,
    .preloadChunks  = 1,
    .vsync          = 0,
    .refreshRate    = 0,
    .fpsCap         = 30,
    .skipIntros     = 0,
    .showConsole    = 0,
    .psxDither      = 1, /* 0=off, 1=PSX dither, 2=bilinear */
    .pixelAspectMode = 1, /* 1=CRT NTSC (1.09375), 2=square (1.0), 3=8:7 (1.143) */
    .allowLooseFiles = 0, /* 0=disc image only, 1=scan gamedata/load/ first */
    .mapName        = "map0_s00"
};

static void TrimWhitespace(char* s)
{
    /* trim trailing */
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                       s[len - 1] == '\r' || s[len - 1] == '\n'))
    {
        s[--len] = '\0';
    }
    /* trim leading */
    char* start = s;
    while (*start == ' ' || *start == '\t') start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
}

void PcConfig_Load(const char* path)
{
    FILE* f = fopen(path, "r");
    if (!f)
    {
        fprintf(stderr, "[CONFIG] %s not found, using defaults (%dx%d, fullscreen=%d, map=%s)\n",
                path, g_PcConfig.windowWidth, g_PcConfig.windowHeight,
                g_PcConfig.fullscreen, g_PcConfig.mapName);
        return;
    }

    fprintf(stderr, "[CONFIG] Loading %s\n", path);

    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        /* skip comments and empty lines */
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == ';' || *p == '\n' || *p == '\r' || *p == '\0')
            continue;

        char key[64] = {0};
        char value[128] = {0};

        char* eq = strchr(p, '=');
        if (!eq) continue;

        size_t keyLen = (size_t)(eq - p);
        if (keyLen >= sizeof(key)) keyLen = sizeof(key) - 1;
        strncpy(key, p, keyLen);
        key[keyLen] = '\0';
        TrimWhitespace(key);

        strncpy(value, eq + 1, sizeof(value) - 1);
        value[sizeof(value) - 1] = '\0';
        TrimWhitespace(value);

        if (strcmp(key, "width") == 0)
        {
            int v = atoi(value);
            if (v >= 320) g_PcConfig.windowWidth = v;
        }
        else if (strcmp(key, "height") == 0)
        {
            int v = atoi(value);
            if (v >= 240) g_PcConfig.windowHeight = v;
        }
        else if (strcmp(key, "fullscreen") == 0)
        {
            g_PcConfig.fullscreen = (atoi(value) != 0);
        }
        else if (strcmp(key, "disable_culling") == 0)
        {
            g_PcConfig.disableCulling = (atoi(value) != 0);
        }
        else if (strcmp(key, "preload_chunks") == 0)
        {
            g_PcConfig.preloadChunks = (atoi(value) != 0);
        }
        else if (strcmp(key, "vsync") == 0)
        {
            g_PcConfig.vsync = atoi(value);
        }
        else if (strcmp(key, "refresh_rate") == 0)
        {
            int v = atoi(value);
            if (v >= 0) g_PcConfig.refreshRate = v;
        }
        else if (strcmp(key, "fps_cap") == 0)
        {
            g_PcConfig.fpsCap = atoi(value);
        }
        else if (strcmp(key, "skip_intros") == 0)
        {
            g_PcConfig.skipIntros = (atoi(value) != 0);
        }
        else if (strcmp(key, "show_console") == 0)
        {
            g_PcConfig.showConsole = (atoi(value) != 0);
        }
        else if (strcmp(key, "psx_dither") == 0)
        {
            int v = atoi(value);
            if (v < 0) v = 0;
            if (v > 2) v = 2;
            g_PcConfig.psxDither = v;
        }
        else if (strcmp(key, "pixel_aspect") == 0)
        {
            int v = atoi(value);
            if (v < 1 || v > 3) v = 1; /* invalid -> default to CRT */
            g_PcConfig.pixelAspectMode = v;
        }
        else if (strcmp(key, "allow_loose_files") == 0)
        {
            g_PcConfig.allowLooseFiles = (atoi(value) != 0);
        }
        else if (strcmp(key, "map") == 0)
        {
            if (strlen(value) > 0 && strlen(value) < sizeof(g_PcConfig.mapName))
            {
                strncpy(g_PcConfig.mapName, value, sizeof(g_PcConfig.mapName) - 1);
                g_PcConfig.mapName[sizeof(g_PcConfig.mapName) - 1] = '\0';
            }
        }
        else
        {
            fprintf(stderr, "[CONFIG] Unknown key: %s\n", key);
        }
    }

    fclose(f);

    fprintf(stderr, "[CONFIG] Resolution: %dx%d, Fullscreen: %d, DisableCulling: %d, Map: %s\n",
            g_PcConfig.windowWidth, g_PcConfig.windowHeight,
            g_PcConfig.fullscreen, g_PcConfig.disableCulling, g_PcConfig.mapName);
}

