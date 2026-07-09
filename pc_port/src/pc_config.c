#include "pc_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stddef.h>
#include "xa_player.h"

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
    .widescreenMode  = 1, /* 0=pillarbox, 1=Hor+ (default, no bars + correct proportions), 2=stretch */
    .menuPillarbox   = 1, /* 1=pillarbox 2D screens (black bars), 0=stretch to fill */
    .allowLooseFiles = 0, /* 0=disc image only, 1=scan gamedata/load/ first */
    .residentTextures = 1, /* 1=expanded chunk-texture pool w/ per-slot GL textures (whole map textured), 0=vanilla 8+2 VRAM pool */
    .texturePacks = 1, /* 1=scan gamedata/texturemods/ for DuckStation texture packs (loose dirs or .zip) */
    .usePgxp        = 0, /* 0=affine textures (PSX look), 1=PGXP perspective correct (WIP) */
    .msaaSamples    = 0, /* 0=off, 2/4/8 = MSAA sample count */
    .postProcess    = 0, /* 0=off, 1.. = post-process look */
    .tonemap        = 0, /* 0=off, 1=Reinhard, 2=ACES, 3=Filmic */
    .perPixelFlashlight = 0, /* 0=per-vertex (PSX), 1=per-pixel flashlight cone */
    .flashlightShadows  = 1, /* per-pixel flashlight casts real-time shadows (on by default; only visible when perPixelFlashlight is on) */
    .flashlightIntensity  = 2.10f, /* per-pixel flashlight cone brightness scale, 0..3 */
    .flashlightSize       = 2.40f, /* per-pixel flashlight cone coverage multiplier */
    .flashlightIntensityFps = 2.10f, /* FPS-mode brightness (head-mounted) */
    .flashlightSizeFps      = 1.30f, /* FPS-mode coverage (tighter than third-person) */
    .postProcessIntensity = 1.0f, /* post-process effect mix, 0..1 */
    .tonemapIntensity     = 1.0f, /* tone-map mix, 0..1 */
    .xaVolume             = 1.0f, /* XA cutscene-voice volume, 0..1; 1.0 = unchanged */
    .fmvVolume            = 1.0f, /* FMV movie (SDL PCM) volume, 0..1; 1.0 = unchanged */
    .enableDebugLog = 0, /* 0=no SilentHill.log, 1=write SilentHill.log (debug builds) */
    .allowDebugControls = 0, /* 0=off (default), 1=enable dev/cheat keys */
    .controllerMovement = 2, /* 0=analog, 1=dpad, 2=both */
    .movementOriginal = 1,   /* 1 = PSX lower-body movement machine (default); 0 = legacy PC shim */

    .controlStyle        = 0, /* 0 = Classic (default), 1 = TPS */
    .allowMouseSecondary = 1, /* deprecated: mouse + alternate binds always active */
    .invertMouseY        = 0,
    .invertControllerY   = 0,
    .tpsAimZoom          = 1, /* zoom TPS/OTS camera in while aiming */
    .crosshair           = 0, /* draw a center crosshair while aiming in TPS/OTS */
    .aimAssist           = 1, /* OTS/TPS free-aim aim assist (mouse body-coverage + controller auto-aim) */
    .altButtonSprint     = 0, /* alt cams sprint from the run control only (off = full stick push also sprints) */
    .immersiveFpsHeadTracking = 0, /* FPS view follows head-bone rotation (experiment, off by default) */
    .control2d               = 0, /* 2D screen-relative movement (experiment, off by default) */
    .adsr                = 1,    /* SPU ADSR envelopes on (BGM instrument fades) */
    .fpsFov              = 67.4f, /* first-person FOV; 67.4 = the game's native projection (H=240), identical to pre-FOV builds */
    .reverbScale         = 0.0f, /* 0 = PsyCross default depth->wet scale */
    .mouseSensitivity        = 1.0f,
    .controllerSensitivity   = 1.0f,

    /* === CLASSIC scheme: tank controls + fixed PSX camera (the default). The
     * keyboard + controller alternates are intentionally unset (== unbound). === */
    .classic = {
        .keyUp = "Up", .keyDown = "Down", .keyLeft = "Left", .keyRight = "Right",
        .keyCross = "C", .keyCircle = "V", .keyTriangle = "Z", .keySquare = "X",
        .keyL1 = "A", .keyR1 = "D", .keyL2 = "Right Shift", .keyR2 = "Left Shift",
        .keyL3 = "NONE", .keyR3 = "NONE", .keyStart = "Return", .keySelect = "Space",
        .padCross = "a", .padCircle = "b", .padTriangle = "y", .padSquare = "x",
        .padL1 = "leftshoulder", .padR1 = "rightshoulder",
        .padL2 = "lefttrigger", .padR2 = "righttrigger",
        .padL3 = "leftstick", .padR3 = "rightstick",
        .padStart = "start", .padSelect = "back",
    },
    /* === ALTCAM scheme: any alternate/modern camera (TPS/OTS). WASD move,
     * A/D strafe, mouse aim(RMB)/fire(LMB); controller LT aim / RT fire, A = use.
     * Unbound actions are "NONE" so they don't fall back to a classic default. === */
    .altcam = {
        .keyUp = "W", .keyDown = "S", .keyLeft = "Left", .keyRight = "Right",
        .keyCross = "Mouse1", .keyCircle = "F", .keyTriangle = "Tab", .keySquare = "Left Shift",
        .keyL1 = "A", .keyR1 = "D", .keyL2 = "NONE", .keyR2 = "Mouse2",
        .keyL3 = "NONE", .keyR3 = "NONE", .keyStart = "Return", .keySelect = "Space",
        .keyCross2 = "E",
        .padCross = "righttrigger", .padCircle = "b", .padTriangle = "y", .padSquare = "leftshoulder",
        .padL1 = "NONE", .padR1 = "NONE",
        .padL2 = "NONE", .padR2 = "lefttrigger",
        .padL3 = "leftstick", .padR3 = "rightstick",
        .padStart = "start", .padSelect = "back",
        .padCross2 = "a",
    },
    .keyQuickSave = "F6", .keyQuickLoad = "F8",
    .keyChangeCam = "F9", .padChangeCam = "rightstick",
    .keySwapShoulder = "Mouse3",
    .keyConsole = "`",
    .keyGfxCycle = "\\",
    .keyGfxPrev  = "[",
    .keyGfxNext  = "]",
    .keyExitGame = "Escape",

    .language       = 0, /* 0=en 1=de 2=fr 3=es 4=it — PAL-disc text language; USA discs ignore it */
    .mapName        = "map0_s00"
};

/* Blue-blood fix (#41): a per-map buffer overrun writes a stray value into
 * g_GameWork.config.extraBloodColor on some maps (e.g. 2 = green), re-paletting
 * all blood. The only legitimate writers (options menu, save load, settings
 * reset) mirror their value here; Map_EffectTexturesLoad re-applies it every map
 * load, so map corruption can't change the player's blood color. Default 0/red. */
unsigned char g_PcTrustedBloodColor = 0;

/* Per-scheme control-binding config keys -> offset within ControlScheme. The
 * same base key with an "_altcam" suffix targets the altcam scheme; without it,
 * the classic scheme (resolved in the parser below). */
static const struct { const char* key; size_t off; } s_SchemeBinds[] = {
    { "key_up",       offsetof(ControlScheme, keyUp)       },
    { "key_down",     offsetof(ControlScheme, keyDown)     },
    { "key_left",     offsetof(ControlScheme, keyLeft)     },
    { "key_right",    offsetof(ControlScheme, keyRight)    },
    { "key_cross",    offsetof(ControlScheme, keyCross)    },
    { "key_circle",   offsetof(ControlScheme, keyCircle)   },
    { "key_triangle", offsetof(ControlScheme, keyTriangle) },
    { "key_square",   offsetof(ControlScheme, keySquare)   },
    { "key_l1",       offsetof(ControlScheme, keyL1)       },
    { "key_r1",       offsetof(ControlScheme, keyR1)       },
    { "key_l2",       offsetof(ControlScheme, keyL2)       },
    { "key_r2",       offsetof(ControlScheme, keyR2)       },
    { "key_l3",       offsetof(ControlScheme, keyL3)       },
    { "key_r3",       offsetof(ControlScheme, keyR3)       },
    { "key_start",    offsetof(ControlScheme, keyStart)    },
    { "key_select",   offsetof(ControlScheme, keySelect)   },
    { "key_up_2",       offsetof(ControlScheme, keyUp2)       },
    { "key_down_2",     offsetof(ControlScheme, keyDown2)     },
    { "key_left_2",     offsetof(ControlScheme, keyLeft2)     },
    { "key_right_2",    offsetof(ControlScheme, keyRight2)    },
    { "key_cross_2",    offsetof(ControlScheme, keyCross2)    },
    { "key_circle_2",   offsetof(ControlScheme, keyCircle2)   },
    { "key_triangle_2", offsetof(ControlScheme, keyTriangle2) },
    { "key_square_2",   offsetof(ControlScheme, keySquare2)   },
    { "key_l1_2",       offsetof(ControlScheme, keyL12)       },
    { "key_r1_2",       offsetof(ControlScheme, keyR12)       },
    { "key_l2_2",       offsetof(ControlScheme, keyL22)       },
    { "key_r2_2",       offsetof(ControlScheme, keyR22)       },
    { "key_l3_2",       offsetof(ControlScheme, keyL32)       },
    { "key_r3_2",       offsetof(ControlScheme, keyR32)       },
    { "key_start_2",    offsetof(ControlScheme, keyStart2)    },
    { "key_select_2",   offsetof(ControlScheme, keySelect2)   },
    { "pad_cross",    offsetof(ControlScheme, padCross)    },
    { "pad_circle",   offsetof(ControlScheme, padCircle)   },
    { "pad_triangle", offsetof(ControlScheme, padTriangle) },
    { "pad_square",   offsetof(ControlScheme, padSquare)   },
    { "pad_l1",       offsetof(ControlScheme, padL1)       },
    { "pad_r1",       offsetof(ControlScheme, padR1)       },
    { "pad_l2",       offsetof(ControlScheme, padL2)       },
    { "pad_r2",       offsetof(ControlScheme, padR2)       },
    { "pad_l3",       offsetof(ControlScheme, padL3)       },
    { "pad_r3",       offsetof(ControlScheme, padR3)       },
    { "pad_start",    offsetof(ControlScheme, padStart)    },
    { "pad_select",   offsetof(ControlScheme, padSelect)   },
    { "pad_cross_2",    offsetof(ControlScheme, padCross2)    },
    { "pad_circle_2",   offsetof(ControlScheme, padCircle2)   },
    { "pad_triangle_2", offsetof(ControlScheme, padTriangle2) },
    { "pad_square_2",   offsetof(ControlScheme, padSquare2)   },
    { "pad_l1_2",       offsetof(ControlScheme, padL12)       },
    { "pad_r1_2",       offsetof(ControlScheme, padR12)       },
    { "pad_l2_2",       offsetof(ControlScheme, padL22)       },
    { "pad_r2_2",       offsetof(ControlScheme, padR22)       },
    { "pad_l3_2",       offsetof(ControlScheme, padL32)       },
    { "pad_r3_2",       offsetof(ControlScheme, padR32)       },
    { "pad_start_2",    offsetof(ControlScheme, padStart2)    },
    { "pad_select_2",   offsetof(ControlScheme, padSelect2)   },
};

/* Global (scheme-independent) binds -> offset within s_PcConfig. */
static const struct { const char* key; size_t off; } s_GlobalBinds[] = {
    { "key_quicksave",     offsetof(s_PcConfig, keyQuickSave)    },
    { "key_quickload",     offsetof(s_PcConfig, keyQuickLoad)    },
    { "key_change_cam",    offsetof(s_PcConfig, keyChangeCam)    },
    { "pad_change_cam",    offsetof(s_PcConfig, padChangeCam)    },
    { "key_swap_shoulder", offsetof(s_PcConfig, keySwapShoulder) },
    { "key_console",       offsetof(s_PcConfig, keyConsole)      },
    { "key_gfx_cycle",     offsetof(s_PcConfig, keyGfxCycle)     },
    { "key_gfx_prev",      offsetof(s_PcConfig, keyGfxPrev)      },
    { "key_gfx_next",      offsetof(s_PcConfig, keyGfxNext)      },
    { "key_exit_game",     offsetof(s_PcConfig, keyExitGame)     },
};

/* Remembered at load time so PcConfig_SaveMapName writes the same file. */
static char s_configPath[512] = "config.cfg";

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
    if (path) {
        strncpy(s_configPath, path, sizeof(s_configPath) - 1);
        s_configPath[sizeof(s_configPath) - 1] = '\0';
    }

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
            /* 0 = windowed, 1 = exclusive fullscreen, 2 = borderless. */
            int v = atoi(value);
            if (v < 0 || v > 2) v = 0;
            g_PcConfig.fullscreen = v;
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
            int v = atoi(value);
            if (v < 0 || v > 3) v = 0;
            g_PcConfig.showConsole = v;
        }
        else if (strcmp(key, "psx_dither") == 0)
        {
            int v = atoi(value);
            if (v < 0) v = 0;
            if (v > 2) v = 2;
            g_PcConfig.psxDither = v;
        }
        else if (strcmp(key, "widescreen_mode") == 0)
        {
            int v = atoi(value);
            if (v < 0 || v > 2) v = 0; /* invalid -> default to pillarbox */
            g_PcConfig.widescreenMode = v;
        }
        else if (strcmp(key, "menu_pillarbox") == 0)
        {
            g_PcConfig.menuPillarbox = (atoi(value) != 0);
        }
        else if (strcmp(key, "allow_loose_files") == 0)
        {
            g_PcConfig.allowLooseFiles = (atoi(value) != 0);
        }
        else if (strcmp(key, "resident_textures") == 0)
        {
            g_PcConfig.residentTextures = (atoi(value) != 0);
        }
        else if (strcmp(key, "texture_packs") == 0)
        {
            g_PcConfig.texturePacks = (atoi(value) != 0);
        }
        else if (strcmp(key, "use_pgxp") == 0)
        {
            g_PcConfig.usePgxp = (atoi(value) != 0);
        }
        else if (strcmp(key, "msaa") == 0)
        {
            /* Antialiasing sample count: 0 (off), 2, 4, 8. Anything else snaps
             * to the nearest sane value so a bad config can't wedge the driver. */
            int v = atoi(value);
            if      (v >= 8) v = 8;
            else if (v >= 4) v = 4;
            else if (v >= 2) v = 2;
            else             v = 0;
            g_PcConfig.msaaSamples = v;
        }
        else if (strcmp(key, "post_process") == 0)
        {
            int v = atoi(value);
            if (v < 0) v = 0;
            g_PcConfig.postProcess = v;
        }
        else if (strcmp(key, "tonemap") == 0)
        {
            int v = atoi(value);
            if (v < 0) v = 0;
            if (v > 3) v = 3;
            g_PcConfig.tonemap = v;
        }
        else if (strcmp(key, "per_pixel_flashlight") == 0)
        {
            g_PcConfig.perPixelFlashlight = (atoi(value) != 0);
        }
        else if (strcmp(key, "flashlight_shadows") == 0)
        {
            g_PcConfig.flashlightShadows = (atoi(value) != 0);
        }
        else if (strcmp(key, "flashlight_intensity") == 0)
        {
            float v = (float)atof(value);
            if (v < 0.0f) v = 0.0f;
            if (v > 3.0f) v = 3.0f;
            g_PcConfig.flashlightIntensity = v;
        }
        else if (strcmp(key, "flashlight_size") == 0)
        {
            float v = (float)atof(value);
            if (v < 0.0f) v = 0.0f;
            if (v > 3.0f) v = 3.0f;
            g_PcConfig.flashlightSize = v;
        }
        else if (strcmp(key, "flashlight_intensity_fps") == 0)
        {
            float v = (float)atof(value);
            if (v < 0.0f) v = 0.0f;
            if (v > 3.0f) v = 3.0f;
            g_PcConfig.flashlightIntensityFps = v;
        }
        else if (strcmp(key, "flashlight_size_fps") == 0)
        {
            float v = (float)atof(value);
            if (v < 0.0f) v = 0.0f;
            if (v > 3.0f) v = 3.0f;
            g_PcConfig.flashlightSizeFps = v;
        }
        else if (strcmp(key, "xa_volume") == 0)
        {
            float v = (float)atof(value);
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            g_PcConfig.xaVolume = v;
        }
        else if (strcmp(key, "fmv_volume") == 0)
        {
            float v = (float)atof(value);
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            g_PcConfig.fmvVolume = v;
        }
        else if (strcmp(key, "post_process_intensity") == 0)
        {
            float v = (float)atof(value);
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            g_PcConfig.postProcessIntensity = v;
        }
        else if (strcmp(key, "tonemap_intensity") == 0)
        {
            float v = (float)atof(value);
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            g_PcConfig.tonemapIntensity = v;
        }
        else if (strcmp(key, "enable_debug_log") == 0)
        {
            g_PcConfig.enableDebugLog = (atoi(value) != 0);
        }
        else if (strcmp(key, "allow_debug_controls") == 0)
        {
            g_PcConfig.allowDebugControls = (atoi(value) != 0);
        }
        else if (strcmp(key, "controller_movement") == 0)
        {
            if (strcmp(value, "analog") == 0)    g_PcConfig.controllerMovement = 0;
            else if (strcmp(value, "dpad") == 0) g_PcConfig.controllerMovement = 1;
            else                                 g_PcConfig.controllerMovement = 2; /* both */
        }
        else if (strcmp(key, "movement_original") == 0)
        {
            g_PcConfig.movementOriginal = (atoi(value) != 0);
        }
        else if (strcmp(key, "language") == 0)
        {
            /* Language id string (launcher dropdown). Index order matches the
             * PAL disc's option-menu / VIN2-5 dir order. Unknown -> English. */
            if (strcmp(value, "de") == 0)      g_PcConfig.language = 1;
            else if (strcmp(value, "fr") == 0) g_PcConfig.language = 2;
            else if (strcmp(value, "es") == 0) g_PcConfig.language = 3;
            else if (strcmp(value, "it") == 0) g_PcConfig.language = 4;
            else                               g_PcConfig.language = 0;
        }
        else if (strcmp(key, "control_style") == 0)
        {
            /* Style id string (matches the registry in control_style.c). The
             * launcher writes the id; map the known ones to the index. Unknown
             * -> Classic. control_style.c re-validates against its registry. */
            if (strcmp(value, "tps") == 0)      g_PcConfig.controlStyle = 1;
            else if (strcmp(value, "ots") == 0) g_PcConfig.controlStyle = 2;
            else if (strcmp(value, "fps") == 0) g_PcConfig.controlStyle = 3;
            else                                g_PcConfig.controlStyle = 0;
        }
        else if (strcmp(key, "allow_mouse_secondary") == 0)
        {
            g_PcConfig.allowMouseSecondary = (atoi(value) != 0);
        }
        else if (strcmp(key, "invert_mouse_y") == 0)
        {
            g_PcConfig.invertMouseY = (atoi(value) != 0);
        }
        else if (strcmp(key, "invert_controller_y") == 0)
        {
            g_PcConfig.invertControllerY = (atoi(value) != 0);
        }
        else if (strcmp(key, "tps_aim_zoom") == 0)
        {
            g_PcConfig.tpsAimZoom = (atoi(value) != 0);
        }
        else if (strcmp(key, "crosshair") == 0)
        {
            g_PcConfig.crosshair = (atoi(value) != 0);
        }
        else if (strcmp(key, "aim_assist") == 0)
        {
            g_PcConfig.aimAssist = (atoi(value) != 0);
        }
        else if (strcmp(key, "altcam_button_sprint") == 0)
        {
            g_PcConfig.altButtonSprint = (atoi(value) != 0);
        }
        else if (strcmp(key, "adsr") == 0)
        {
            g_PcConfig.adsr = (atoi(value) != 0);
        }
        else if (strcmp(key, "fps_fov") == 0)
        {
            float v = (float)atof(value);
            if (v < 55.0f)  v = 55.0f;
            if (v > 110.0f) v = 110.0f;
            g_PcConfig.fpsFov = v;
        }
        else if (strcmp(key, "reverb_scale") == 0)
        {
            g_PcConfig.reverbScale = (float)atof(value);
        }
        else if (strcmp(key, "immersive_fps_head_tracking") == 0)
        {
            g_PcConfig.immersiveFpsHeadTracking = (atoi(value) != 0);
        }
        else if (strcmp(key, "control_2d") == 0)
        {
            g_PcConfig.control2d = (atoi(value) != 0);
        }
        else if (strcmp(key, "mouse_sensitivity") == 0)
        {
            float v = (float)atof(value);
            if (v < 0.1f) v = 0.1f;
            if (v > 4.0f) v = 4.0f;
            g_PcConfig.mouseSensitivity = v;
        }
        else if (strcmp(key, "controller_sensitivity") == 0)
        {
            float v = (float)atof(value);
            if (v < 0.1f) v = 0.1f;
            if (v > 4.0f) v = 4.0f;
            g_PcConfig.controllerSensitivity = v;
        }
        else if (strcmp(key, "unlimited_enemies") == 0)
        {
            g_PcConfig.unlimitedEnemies = (atoi(value) != 0);
        }
        else if (strcmp(key, "control_styles") == 0)
        {
            /* Game-owned registry list, published for the launcher's dropdown.
             * The game writes it on boot; it reads nothing back. Ignore. */
        }
        else if (strcmp(key, "map") == 0)
        {
            if (strlen(value) > 0 && strlen(value) < sizeof(g_PcConfig.mapName))
            {
                strncpy(g_PcConfig.mapName, value, sizeof(g_PcConfig.mapName) - 1);
                g_PcConfig.mapName[sizeof(g_PcConfig.mapName) - 1] = '\0';
            }
        }
        else if (strncmp(key, "launcher_", 9) == 0)
        {
            /* Launcher-managed keys (launcher_repo_url / _branch / _build) live in
             * this same config.cfg under the "## Launcher" section. The game owns
             * none of them — ignore silently so they don't hit the unknown-key
             * warning below. */
        }
        else
        {
            /* Control bindings. Per-scheme keys (key_*, pad_*, +_2) optionally
             * carry an "_altcam" suffix selecting the alternate-camera scheme;
             * without it they target classic. Global meta-binds (quicksave /
             * change_cam / swap_shoulder) have no scheme. Table-driven copy. */
            char base[64];
            ControlScheme* scheme = &g_PcConfig.classic;
            size_t klen = strlen(key);
            size_t bi;
            int matched = 0;
            const size_t suflen = 7; /* strlen("_altcam") */

            strncpy(base, key, sizeof(base) - 1);
            base[sizeof(base) - 1] = '\0';
            if (klen > suflen && strcmp(key + klen - suflen, "_altcam") == 0)
            {
                scheme = &g_PcConfig.altcam;
                base[klen - suflen] = '\0';
            }

            for (bi = 0; bi < sizeof(s_SchemeBinds) / sizeof(s_SchemeBinds[0]); bi++)
            {
                if (strcmp(base, s_SchemeBinds[bi].key) == 0)
                {
                    char* field = (char*)scheme + s_SchemeBinds[bi].off;
                    strncpy(field, value, 23);
                    field[23] = '\0';
                    matched = 1;
                    break;
                }
            }
            for (bi = 0; !matched && bi < sizeof(s_GlobalBinds) / sizeof(s_GlobalBinds[0]); bi++)
            {
                if (strcmp(key, s_GlobalBinds[bi].key) == 0)
                {
                    char* field = (char*)&g_PcConfig + s_GlobalBinds[bi].off;
                    strncpy(field, value, 23);
                    field[23] = '\0';
                    matched = 1;
                }
            }
            if (!matched)
                fprintf(stderr, "[CONFIG] Unknown key: %s\n", key);
        }
    }

    fclose(f);

    fprintf(stderr, "[CONFIG] Resolution: %dx%d, Fullscreen: %d, DisableCulling: %d, Map: %s\n",
            g_PcConfig.windowWidth, g_PcConfig.windowHeight,
            g_PcConfig.fullscreen, g_PcConfig.disableCulling, g_PcConfig.mapName);
}

/* Rewrite (or append) a single `key = value` line in the loaded config file,
 * preserving every other line and comment. */
void PcConfig_SaveKeyValue(const char* cfgKey, const char* cfgValue)
{
    /* Big enough to hold the whole config with headroom: the file grows as new
     * settings are toggled (each unknown key appends a line), and any line past
     * this cap would be dropped on the next save — silently resetting those keys
     * to their defaults. The full keybind config is ~380 lines already. */
    static char lines[1024][256];
    int   n = 0;
    int   i;
    int   found = 0;
    FILE* f;

    if (cfgKey == NULL || cfgKey[0] == '\0' || cfgValue == NULL)
        return;

    f = fopen(s_configPath, "r");
    if (!f)
        return;
    while (n < (int)(sizeof(lines) / sizeof(lines[0])) &&
           fgets(lines[n], sizeof(lines[n]), f))
        n++;
    fclose(f);

    for (i = 0; i < n; i++)
    {
        char*  p = lines[i];
        char   key[64] = {0};
        char*  eq;
        size_t kl;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == ';') continue;
        eq = strchr(p, '=');
        if (!eq) continue;
        kl = (size_t)(eq - p);
        if (kl >= sizeof(key)) kl = sizeof(key) - 1;
        strncpy(key, p, kl);
        key[kl] = '\0';
        TrimWhitespace(key);
        if (strcmp(key, cfgKey) == 0)
        {
            snprintf(lines[i], sizeof(lines[i]), "%s = %s\n", cfgKey, cfgValue);
            found = 1;
            break;
        }
    }

    f = fopen(s_configPath, "w");
    if (!f)
        return;
    for (i = 0; i < n; i++)
        fputs(lines[i], f);
    if (!found)
        fprintf(f, "%s = %s\n", cfgKey, cfgValue);
    fclose(f);
}

void PcConfig_ApplyXaVolume(float norm)
{
    char buf[16];
    if (norm < 0.0f) norm = 0.0f;
    if (norm > 1.0f) norm = 1.0f;
    g_PcConfig.xaVolume = norm;
    XaPlayer_SetMasterVolume(norm); /* sets g_PcXaVolume + live source gain */
    snprintf(buf, sizeof(buf), "%.3f", norm);
    PcConfig_SaveKeyValue("xa_volume", buf);
}

/* Rewrite the `map = ...` line. Used by the in-game map-cycle debug keys so the
 * choice persists to the next New Game / launch. */
void PcConfig_SaveMapName(const char* mapName)
{
    if (mapName == NULL || mapName[0] == '\0')
        return;
    PcConfig_SaveKeyValue("map", mapName);
}

