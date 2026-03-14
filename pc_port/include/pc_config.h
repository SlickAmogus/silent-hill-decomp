#ifndef PC_CONFIG_H
#define PC_CONFIG_H

typedef struct {
    int windowWidth;
    int windowHeight;
    int fullscreen;
    int disableCulling; /* 1 = render all objects regardless of view angle */
    char mapName[64];   /* e.g. "map0_s00" */
} s_PcConfig;

extern s_PcConfig g_PcConfig;

/* Parse config.cfg from the executable's directory. Uses defaults if not found. */
void PcConfig_Load(const char* path);

#endif /* PC_CONFIG_H */
