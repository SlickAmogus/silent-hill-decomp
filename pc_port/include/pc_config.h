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
    int showConsole;     /* EXTERNAL console window only: 1 or 3 = create it, else none.
                          * The ingame console is not config-gated anymore — `~` toggles it
                          * (needs allow_debug_controls). Legacy values 2/3 still parse. */
    int psxDither;       /* texture filtering mode: 0 = off, 1 = PSX dither, 2 = bilinear */
    int widescreenMode;  /* 0 = pillarbox (PSX-faithful, default), 1 = Hor+ (extra side content), 2 = stretch */
    int menuPillarbox;   /* 1 = pillarbox 2D screens (menus/load) with 4:3 black bars instead of stretching to fill (config key: menu_pillarbox) */
    int allowLooseFiles; /* 1 = scan gamedata/load/{folder}/{name}.{ext} before CD read (texture mod support) */
    int residentTextures; /* 1 = expanded chunk-texture pool: past the 10 vanilla VRAM pages, materials get
                           * virtual slots backed by persistent per-slot GL textures (whole map stays textured,
                           * no page stealing — the flat/rainbow class). 0 = vanilla 8+2 pool + PC keep-4/steal.
                           * (config key: resident_textures) */
    int texturePacks;     /* 1 = scan gamedata/texturemods/ for DuckStation-format texture packs
                           * (texupload-*.png, loose folders or .zip archives) and apply them by content
                           * hash at TIM upload (config key: texture_packs) */
    int texpackCacheMb;   /* RAM cap (MB) for the composed-canvas cache — chunk streaming re-uploads
                           * the same TIMs endlessly, and re-decoding pack PNGs each time stutters.
                           * 0 disables (config key: texpack_cache_mb) */
    int bulletDecals;     /* 1 = bullet-hole decals where player gunfire hits world geometry
                           * (gamedata/decal.png; up to 64 FIFO, cleared on map load)
                           * (config key: bullet_decals) */
    int globalCharaPool;  /* 1 = global chara/asset pool: every monster's model+anim+texture stays
                           * resident PC-side (virtual GL texture slots) and chara_global.dll backfills
                           * AI update funcs, so console SPAWN works for every type in any map. Native
                           * maps keep their native slots/variants; 0 = vanilla 3-types-per-map.
                           * (config key: global_chara_pool; docs/Global_Chara_Pool.md) */
    int wholeMapExteriors; /* EXPERIMENTAL, default 0: texture + draw every loaded exterior chunk (whole
                            * town resident — combine with fogstr to see it). Costs draw time and shows
                            * geometry vanilla never rendered at once. Requires preload_chunks and
                            * resident_textures. (config key: whole_map_exteriors) */
    int usePgxp;         /* 1 = enable PGXP precision/perspective-correct textures (work-in-progress) */
    int msaaSamples;     /* MSAA on the default framebuffer: 0 = off, 2/4/8 = sample count (config key: msaa) */
    int postProcess;     /* full-screen post-process look: 0 = off, 1.. = built-in filter (config key: post_process) */
    int tonemap;         /* tone-map operator: 0=off,1=Reinhard,2=ACES,3=Filmic (config key: tonemap) */
    int flashlightMode;     /* THE flashlight setting (config key: flashlight_mode):
                             * 0 = Classic (PSX per-vertex), 1 = Classic + Shadows
                             * (per-pixel, PSX-calibrated style), 2 = Modern (per-pixel
                             * stylized spotlight, no shadows), 3 = Modern + Shadows.
                             * perPixelFlashlight/flashlightShadows are DERIVED from it
                             * by Pc_FlashlightModeApply; legacy configs without the key
                             * derive it the other way (pp+shadows = Modern + Shadows). */
    int perPixelFlashlight; /* DERIVED from flashlightMode (legacy key: per_pixel_flashlight). */
    int flashlightShadows;  /* DERIVED from flashlightMode (legacy key: flashlight_shadows). */
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
    int unlimitedEnemies;   /* 1 = raise the per-room concurrent-NPC cap to NPC_COUNT_MAX (config key: unlimited_enemies) */
    int randomizer;         /* 1 = randomizer gamemode: New Game always opens in map2_s04, every door leads
                             * somewhere random, monsters/items are rerolled per area, and the run ends at the
                             * map7_s03 boss with a score-picked ending. Forces global_chara_pool on and
                             * overrides `map`. 0 = off, byte-identical vanilla.
                             * (config key: randomizer; docs/Randomizer_Mode.md) */
    int controllerMovement; /* 0 = analog stick, 1 = d-pad, 2 = both (default) */
    int movementOriginal;   /* 1 = PSX lower-body movement state
                             * machine (accel/decel, wall smack, authored sidesteps)
                             * (default). 0 = legacy PC movement shim (TPS debug cam + fallback). */

    int controlStyle;       /* active camera/control style: 0 = Classic, 1 = TPS (config key: control_style) */
    int allowMouseSecondary;/* deprecated: mouse + alternate (*_2) binds are always active now */
    int invertMouseY;       /* 1 = invert mouse Y for TPS look (config key: invert_mouse_y) */
    int invertControllerY;  /* 1 = invert right-stick Y for TPS look (config key: invert_controller_y) */
    int tpsCameraCollision; /* "Allow thirdperson camera collision": 1 (default) = the TPS/OTS eye is pulled in when level geometry would come between it and Harry; 0 = the eye keeps its ideal orbit position and may pass through walls (config key: tps_camera_collision) */
    int tpsOtsAim;          /* "OTS aiming in Thirdperson": 1 (default) = raising the gun in TPS eases the camera into the Over-the-Shoulder framing (and the shoulder-swap bind works); 0 = TPS keeps its centred camera while aiming (config key: tps_ots_aim) */
    int crosshair;          /* 1 = draw a center crosshair while aiming in TPS/OTS (config key: crosshair) */
    int aimAssist;          /* 1 = OTS/TPS free-aim aim assist (mouse body-coverage + controller auto-aim) (config key: aim_assist) */
    int mouseCursor;        /* 1 = mouse controls cursor puzzles + clickable main menu (config key: mouse_cursor) */
    int altButtonSprint;    /* "Always use button based sprinting": 1 = walk by default, sprint ONLY while the bound run control is held — applies to alt cameras (TPS/OTS/FPS) AND 2D control under any camera; 0 = a near-full stick push also sprints (config key: altcam_button_sprint) */
    int immersiveFpsHeadTracking; /* 1 = FPS view direction follows Harry's animated head-bone rotation (idle sway/lean), mouse layered on top (config key: immersive_fps_head_tracking) */
    int control2d;          /* 1 = 2D screen-relative movement (input aligns with the camera; Harry turns to face the move direction) under ALL non-FPS camera styles (config key: control_2d) — an Experiment, off by default */
    int   adsr;             /* 1 = SPU ADSR envelopes (instrument attack/release fades in sequenced BGM); default 1 (config key: adsr) */
    float fpsFov;           /* first-person horizontal FOV in degrees (4:3 basis), 55..110; default 71.1 = the game's OWN projection (H = gsScreenHeight = 224 on the 320-wide progressive frame), so the default is a no-op; applied ONLY during FPS gameplay (config key: fps_fov) */
    float tpsFov;           /* Thirdperson/OTS horizontal FOV in degrees (4:3 basis), 55..110; default 71.1 = the game's OWN projection (H = gsScreenHeight = 224 on the 320-wide progressive frame), so the default is a no-op; applied ONLY during TPS/OTS gameplay — the Classic camera always keeps the original projection (config key: tps_fov) */
    float tpsAimZoom;       /* "TPS/OTS Aim Zoom": how far the TPS/OTS camera dollies in while aiming, as a percentage of the zoom range, 0..100. 50 (default) = the original full zoom, 100 = a deeper 2x zoom, 0 = no zoom at all. Replaces the old tps_aim_zoom on/off key (config key: tps_aim_zoom_amount) */
    float reverbScale;      /* reverb depth->wet mapping scale, 0 = leave PsyCross default (2.0) (config key: reverb_scale) */
    float mouseSensitivity;      /* mouse-look sensitivity multiplier for TPS/OTS/FPS cameras, 0.1..4.0; default 1.0 (config key: mouse_sensitivity) */
    float controllerSensitivity; /* right-stick look sensitivity multiplier for TPS/OTS/FPS cameras, 0.1..4.0; default 1.0 (config key: controller_sensitivity) */

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
    /* PC-only graphics-effect tuning keys (keyboard-only). keyGfxCycle switches
     * which enabled effect (flashlight / post-process / tonemap) is being tuned;
     * keyGfxPrev/keyGfxNext lower/raise its intensity. Defaults \ / [ / ]. */
    char keyGfxCycle[24];
    char keyGfxPrev[24];
    char keyGfxNext[24];
    /* PC-only: exit-game bind (title/menu = quit, in-game = warm-reboot to
     * title). Default Escape; rebindable/unbindable like any other global bind
     * (config key: key_exit_game). */
    char keyExitGame[24];

    /* Text language for PAL/European discs: 0=en 1=de 2=fr 3=es 4=it (config
     * key: language, string ids). Order matches the PAL disc's own option-menu
     * order and its VIN/VIN2..VIN5 localized-file dirs. On USA discs it only
     * selects the port's menu translations when a fan-translated (modified)
     * disc is active — story/item text then comes from the disc itself. */
    int language;

    /* Preferred disc region when several discs are in gamedata/: 0=auto
     * (USA wins, then PAL, then NTSC-J), 1=usa, 2=pal, 3=jap (config key:
     * region; the launcher's Region dropdown writes it). Missing preferred
     * region falls back to auto. */
    int region;

    /* Exact disc image filename in gamedata/ (config key: disc_image; the
     * launcher's Disc dropdown writes it). Empty = auto-pick by region rules.
     * Lets fan-translated / modified images be selected over the vanilla
     * name-priority order. */
    char discImage[128];

    char mapName[64];    /* e.g. "map0_s00" */
} s_PcConfig;

extern s_PcConfig g_PcConfig;

/* True while the experimental whole-town render mode applies: exterior map,
 * street room, whole_map_exteriors + preload_chunks + resident_textures all
 * on. Implemented in bodyprog_80040B74.c (needs g_Map/g_SavegamePtr). */
int Pc_WholeMapDrawActive(void);

/* Apply a flashlight mode (0..3, see flashlightMode) to the config + all PsyX
 * globals. Swaps flashlight_intensity/size between the two per-pixel styles'
 * calibrated defaults when the current value IS the other style's default
 * (Modern 2.10/2.40, Classic+Shadows 1.20/3.00); customized values are kept.
 * persist=1 also writes the config keys. Used by boot, F4, options, console. */
void Pc_FlashlightModeApply(int mode, int persist);

/* "Classic" / "Classic + Shadows" / "Modern" / "Modern + Shadows" */
const char* Pc_FlashlightModeLabel(int mode);

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

