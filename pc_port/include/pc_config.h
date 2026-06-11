#ifndef PC_CONFIG_H
#define PC_CONFIG_H

typedef struct {
    int windowWidth;
    int windowHeight;
    int fullscreen;      /* 0 = windowed, 1 = exclusive fullscreen, 2 = borderless (desktop) */
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
    int menuPillarbox;   /* 1 = pillarbox 2D screens (menus/load) with 4:3 black bars instead of stretching to fill (config key: menu_pillarbox) */
    int allowLooseFiles; /* 1 = scan gamedata/load/{folder}/{name}.{ext} before CD read (texture mod support) */
    int usePgxp;         /* 1 = enable PGXP precision/perspective-correct textures (work-in-progress) */
    int enableDebugLog;  /* 1 = create + write SilentHill.log; 0 = no log file, SH_DBG no-op (config key: enable_debug_log) */
    int allowDebugControls; /* 1 = enable dev/cheat keys (numpad, top-row digits, ~, kill-Harry, etc.); 0 = off (default) */
    int controllerMovement; /* 0 = analog stick, 1 = d-pad, 2 = both (default) */
    int movementOriginal;   /* 1 = PSX lower-body movement state
                             * machine (accel/decel, wall smack, authored sidesteps)
                             * (default). 0 = legacy PC movement shim (TPS debug cam + fallback). */

    /* Control bindings. Keyboard = SDL scancode names ("C", "Z", "Return",
     * "Space", "Up", "Left Shift", "["). Controller = SDL game-controller
     * names ("a","b","x","y","leftshoulder","righttrigger","leftstick",
     * "start","back"). "NONE" = unbound. D-pad + sticks are movement and not
     * controller-rebindable here. Read by Pc_ApplyControlConfig in main_pc.c. */
    char keyUp[24], keyDown[24], keyLeft[24], keyRight[24];
    char keyCross[24], keyCircle[24], keyTriangle[24], keySquare[24];
    char keyL1[24], keyR1[24], keyL2[24], keyR2[24], keyL3[24], keyR3[24];
    char keyStart[24], keySelect[24];
    char keyQuickSave[24], keyQuickLoad[24]; /* PC-only: quick save/load screen hotkeys */
    char padCross[24], padCircle[24], padTriangle[24], padSquare[24];
    char padL1[24], padR1[24], padL2[24], padR2[24], padL3[24], padR3[24];
    char padStart[24], padSelect[24];

    char mapName[64];    /* e.g. "map0_s00" */
} s_PcConfig;

extern s_PcConfig g_PcConfig;

/* Parse config.cfg from the executable's directory. Uses defaults if not found. */
void PcConfig_Load(const char* path);

/* Rewrite only the `map = ...` line in the loaded config file (preserves the
 * rest). Persists a runtime map change so the next New Game loads it. */
void PcConfig_SaveMapName(const char* mapName);

#endif /* PC_CONFIG_H */

