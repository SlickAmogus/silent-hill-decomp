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
    int showConsole;     /* 0=off, 1=external window, 2=ingame overlay, 3=ingame+external */
    int psxDither;       /* texture filtering mode: 0 = off, 1 = PSX dither, 2 = bilinear */
    int pixelAspectMode; /* 1 = CRT NTSC (1.09375), 2 = square (1.0), 3 = 8:7 (1.143) */
    int widescreenMode;  /* 0 = pillarbox (PSX-faithful, default), 1 = Hor+ (extra side content), 2 = stretch */
    int allowLooseFiles; /* 1 = scan gamedata/load/{folder}/{name}.{ext} before CD read (texture mod support) */
    int usePgxp;         /* 1 = enable PGXP precision/perspective-correct textures (work-in-progress) */
    int enableDebugLog;  /* 1 = create + write SilentHill.log; 0 = no log file, SH_DBG no-op (config key: enable_debug_log) */
    char mapName[64];    /* e.g. "map0_s00" */
} s_PcConfig;

extern s_PcConfig g_PcConfig;

/* Parse config.cfg from the executable's directory. Uses defaults if not found. */
void PcConfig_Load(const char* path);

#endif /* PC_CONFIG_H */

