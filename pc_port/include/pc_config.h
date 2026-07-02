#ifndef PC_CONFIG_H
#define PC_CONFIG_H

/* One full control scheme: keyboard primary + alternate, controller primary +
 * alternate. Keyboard values are SDL scancode names ("C","Up","Left Shift","[")
 * or "MouseN"; controller values are SDL game-controller names ("a",
 * "leftshoulder","righttrigger"). "NONE" (or empty) = unbound. */
typedef struct {
    char keyUp[24], keyDown[24], keyLeft[24], keyRight[24];
    char keyCross[24], keyCircle[24], keyTriangle[24], keySquare[24];
    char keyL1[24], keyR1[24], keyL2[24], keyR2[24], keyL3[24], keyR3[24];
    char keyStart[24], keySelect[24];
    char keyUp2[24], keyDown2[24], keyLeft2[24], keyRight2[24];
    char keyCross2[24], keyCircle2[24], keyTriangle2[24], keySquare2[24];
    char keyL12[24], keyR12[24], keyL22[24], keyR22[24], keyL32[24], keyR32[24];
    char keyStart2[24], keySelect2[24];
    char padCross[24], padCircle[24], padTriangle[24], padSquare[24];
    char padL1[24], padR1[24], padL2[24], padR2[24], padL3[24], padR3[24];
    char padStart[24], padSelect[24];
    char padCross2[24], padCircle2[24], padTriangle2[24], padSquare2[24];
    char padL12[24], padR12[24], padL22[24], padR22[24], padL32[24], padR32[24];
    char padStart2[24], padSelect2[24];
} ControlScheme;

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
    int widescreenMode;  /* 0 = pillarbox (PSX-faithful, default), 1 = Hor+ (extra side content), 2 = stretch */
    int menuPillarbox;   /* 1 = pillarbox 2D screens (menus/load) with 4:3 black bars instead of stretching to fill (config key: menu_pillarbox) */
    int allowLooseFiles; /* 1 = scan gamedata/load/{folder}/{name}.{ext} before CD read (texture mod support) */
    int usePgxp;         /* 1 = enable PGXP precision/perspective-correct textures (work-in-progress) */
    int msaaSamples;     /* MSAA on the default framebuffer: 0 = off, 2/4/8 = sample count (config key: msaa) */
    int postProcess;     /* full-screen post-process look: 0 = off, 1.. = built-in filter (config key: post_process) */
    int tonemap;         /* tone-map operator: 0=off,1=Reinhard,2=ACES,3=Filmic (config key: tonemap) */
    int perPixelFlashlight; /* 1 = per-pixel (fragment-shader) flashlight cone (config key: per_pixel_flashlight) */
    float flashlightIntensity;  /* per-pixel flashlight cone brightness scale, 0..3 (config key: flashlight_intensity) */
    float flashlightSize;       /* per-pixel flashlight cone coverage multiplier, 0..3 (config key: flashlight_size) */
    float flashlightIntensityFps; /* per-pixel flashlight brightness in FPS mode, 0..3 (config key: flashlight_intensity_fps) */
    float flashlightSizeFps;      /* per-pixel flashlight coverage in FPS mode, 0..3 (config key: flashlight_size_fps) */
    float postProcessIntensity; /* post-process effect mix, 0..1 (config key: post_process_intensity) */
    float tonemapIntensity;     /* tone-map mix, 0..1 (config key: tonemap_intensity) */
    float xaVolume;             /* XA cutscene-voice stream volume, 0..1; default 1.0 (config key: xa_volume) */
    float fmvVolume;            /* FMV movie (SDL PCM) volume, 0..1; default 1.0 (config key: fmv_volume) */
    int enableDebugLog;  /* 1 = create + write SilentHill.log; 0 = no log file, SH_DBG no-op (config key: enable_debug_log) */
    int allowDebugControls; /* 1 = enable dev/cheat keys (numpad, top-row digits, ~, kill-Harry, etc.); 0 = off (default) */
    int controllerMovement; /* 0 = analog stick, 1 = d-pad, 2 = both (default) */
    int movementOriginal;   /* 1 = PSX lower-body movement state
                             * machine (accel/decel, wall smack, authored sidesteps)
                             * (default). 0 = legacy PC movement shim (TPS debug cam + fallback). */

    int controlStyle;       /* active camera/control style: 0 = Classic, 1 = TPS (config key: control_style) */
    int allowMouseSecondary;/* deprecated: mouse + alternate (*_2) binds are always active now */
    int invertMouseY;       /* 1 = invert mouse Y for TPS look (config key: invert_mouse_y) */
    int invertControllerY;  /* 1 = invert right-stick Y for TPS look (config key: invert_controller_y) */
    int tpsAimZoom;         /* 1 = zoom the TPS/OTS camera in while aiming/attacking (config key: tps_aim_zoom) */
    int crosshair;          /* 1 = draw a center crosshair while aiming in TPS/OTS (config key: crosshair) */
    int aimAssist;          /* 1 = OTS/TPS free-aim aim assist (mouse body-coverage + controller auto-aim) (config key: aim_assist) */

    /* Two control schemes, selected per-frame at runtime by camera mode and
     * applied via Pc_ApplyActiveControlScheme (main_pc.c). classic = default
     * camera (tank/fixed angles); altcam = ANY alternate/modern camera (TPS/OTS).
     * Config keys: classic uses the key_ and pad_ names (with _2 alternates);
     * altcam uses the same names plus an "_altcam" suffix. */
    ControlScheme classic;
    ControlScheme altcam;

    /* Global (scheme-independent) binds. */
    char keyQuickSave[24], keyQuickLoad[24]; /* PC-only: quick save/load screen hotkeys */
    char keyChangeCam[24], padChangeCam[24]; /* PC-only: Change Camera (cycle control style) */
    char keySwapShoulder[24]; /* PC-only: swap OTS shoulder side (default Mouse3) */
    char keyConsole[24]; /* PC-only: dev console toggle key (default tilde "`"); keyboard-only */

    char mapName[64];    /* e.g. "map0_s00" */
} s_PcConfig;

extern s_PcConfig g_PcConfig;

/* Parse config.cfg from the executable's directory. Uses defaults if not found. */
void PcConfig_Load(const char* path);

/* Rewrite only the `map = ...` line in the loaded config file (preserves the
 * rest). Persists a runtime map change so the next New Game loads it. */
void PcConfig_SaveMapName(const char* mapName);

/* Rewrite (or append) a single `key = value` line in the loaded config file,
 * preserving every other line + comment. Used to persist runtime changes
 * (control_style) and to publish game-owned lists (control_styles) so the
 * launcher reflects whatever the installed build supports. */
void PcConfig_SaveKeyValue(const char* key, const char* value);

/* Set the FMV/voice (XA) master volume [0,1]: clamps, updates g_PcConfig +
 * the live XA source gain, and persists `xa_volume`. Used by the options menu
 * and the `xavolume` console command. */
void PcConfig_ApplyXaVolume(float norm);

#endif /* PC_CONFIG_H */

