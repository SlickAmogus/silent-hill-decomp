#ifndef PC_CONFIG_H
#define PC_CONFIG_H

typedef struct {
    int windowWidth;
    int windowHeight;
    int fullscreen;
    int disableCulling;  /* 1 = render all objects regardless of view angle */
    int preloadChunks;   /* 1 = load all IPD chunks at map init instead of streaming */
    int vsync;           /* 0 = off (uncapped), 1 = on, -1 = adaptive */
    int refreshRate;     /* target refresh rate in hz (0 = display default); fullscreen only */
    int fpsCap;          /* gameplay fps cap: 0 = uncapped, 30 = PSX-accurate, 60 = smooth */
    int skipIntros;      /* 1 = skip Konami/KCET logos and opening movie, go straight to main menu */
    int showConsole;     /* 1 = keep the secondary console window visible + echo SH_DBG to it */
    char mapName[64];    /* e.g. "map0_s00" */
} s_PcConfig;

extern s_PcConfig g_PcConfig;

/* Parse config.cfg from the executable's directory. Uses defaults if not found. */
void PcConfig_Load(const char* path);

#endif /* PC_CONFIG_H */

