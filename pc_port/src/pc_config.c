#include "pc_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stddef.h>

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
    .usePgxp        = 0, /* 0=affine textures (PSX look), 1=PGXP perspective correct (WIP) */
    .enableDebugLog = 0, /* 0=no SilentHill.log, 1=write SilentHill.log (debug builds) */
    .allowDebugControls = 0, /* 0=off (default), 1=enable dev/cheat keys */
    .controllerMovement = 2, /* 0=analog, 1=dpad, 2=both */
    .movementOriginal = 1,   /* 1 = PSX lower-body movement machine (default); 0 = legacy PC shim */

    /* Keyboard defaults (SDL scancode names) */
    .keyUp = "Up", .keyDown = "Down", .keyLeft = "Left", .keyRight = "Right",
    .keyCross = "C", .keyCircle = "V", .keyTriangle = "Z", .keySquare = "X",
    .keyL1 = "A", .keyR1 = "D", .keyL2 = "Right Shift", .keyR2 = "Left Shift",
    .keyL3 = "[", .keyR3 = "]", .keyStart = "Return", .keySelect = "Space",
    .keyQuickSave = "F6", .keyQuickLoad = "F8",
    /* Controller defaults (SDL game-controller names) */
    .padCross = "a", .padCircle = "b", .padTriangle = "y", .padSquare = "x",
    .padL1 = "leftshoulder", .padR1 = "rightshoulder",
    .padL2 = "lefttrigger", .padR2 = "righttrigger",
    .padL3 = "leftstick", .padR3 = "rightstick",
    .padStart = "start", .padSelect = "back",

    .mapName        = "map0_s00"
};

/* Blue-blood fix (#41): a per-map buffer overrun writes a stray value into
 * g_GameWork.config.extraBloodColor on some maps (e.g. 2 = green), re-paletting
 * all blood. The only legitimate writers (options menu, save load, settings
 * reset) mirror their value here; Map_EffectTexturesLoad re-applies it every map
 * load, so map corruption can't change the player's blood color. Default 0/red. */
unsigned char g_PcTrustedBloodColor = 0;

/* Control-binding config keys -> string field in g_PcConfig. Table-driven so
 * the parser stays compact (28 binds). */
static const struct { const char* key; size_t off; } s_ControlBinds[] = {
    { "key_up",       offsetof(s_PcConfig, keyUp)       },
    { "key_down",     offsetof(s_PcConfig, keyDown)     },
    { "key_left",     offsetof(s_PcConfig, keyLeft)     },
    { "key_right",    offsetof(s_PcConfig, keyRight)    },
    { "key_cross",    offsetof(s_PcConfig, keyCross)    },
    { "key_circle",   offsetof(s_PcConfig, keyCircle)   },
    { "key_triangle", offsetof(s_PcConfig, keyTriangle) },
    { "key_square",   offsetof(s_PcConfig, keySquare)   },
    { "key_l1",       offsetof(s_PcConfig, keyL1)       },
    { "key_r1",       offsetof(s_PcConfig, keyR1)       },
    { "key_l2",       offsetof(s_PcConfig, keyL2)       },
    { "key_r2",       offsetof(s_PcConfig, keyR2)       },
    { "key_l3",       offsetof(s_PcConfig, keyL3)       },
    { "key_r3",       offsetof(s_PcConfig, keyR3)       },
    { "key_start",    offsetof(s_PcConfig, keyStart)    },
    { "key_select",   offsetof(s_PcConfig, keySelect)   },
    { "key_quicksave", offsetof(s_PcConfig, keyQuickSave) },
    { "key_quickload", offsetof(s_PcConfig, keyQuickLoad) },
    { "pad_cross",    offsetof(s_PcConfig, padCross)    },
    { "pad_circle",   offsetof(s_PcConfig, padCircle)   },
    { "pad_triangle", offsetof(s_PcConfig, padTriangle) },
    { "pad_square",   offsetof(s_PcConfig, padSquare)   },
    { "pad_l1",       offsetof(s_PcConfig, padL1)       },
    { "pad_r1",       offsetof(s_PcConfig, padR1)       },
    { "pad_l2",       offsetof(s_PcConfig, padL2)       },
    { "pad_r2",       offsetof(s_PcConfig, padR2)       },
    { "pad_l3",       offsetof(s_PcConfig, padL3)       },
    { "pad_r3",       offsetof(s_PcConfig, padR3)       },
    { "pad_start",    offsetof(s_PcConfig, padStart)    },
    { "pad_select",   offsetof(s_PcConfig, padSelect)   },
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
        else if (strcmp(key, "use_pgxp") == 0)
        {
            g_PcConfig.usePgxp = (atoi(value) != 0);
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
            /* Control bindings (key_ and pad_ keys): table-driven copy into
             * the matching g_PcConfig string field. */
            size_t bi;
            int matched = 0;
            for (bi = 0; bi < sizeof(s_ControlBinds) / sizeof(s_ControlBinds[0]); bi++)
            {
                if (strcmp(key, s_ControlBinds[bi].key) == 0)
                {
                    char* field = (char*)&g_PcConfig + s_ControlBinds[bi].off;
                    strncpy(field, value, 23);
                    field[23] = '\0';
                    matched = 1;
                    break;
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

/* Rewrite the `map = ...` line in the loaded config file, preserving every
 * other line and comment. Used by the in-game map-cycle debug keys so the
 * choice persists to the next New Game / launch. */
void PcConfig_SaveMapName(const char* mapName)
{
    static char lines[400][256];
    int   n = 0;
    int   i;
    int   found = 0;
    FILE* f;

    if (mapName == NULL || mapName[0] == '\0')
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
        if (strcmp(key, "map") == 0)
        {
            snprintf(lines[i], sizeof(lines[i]), "map = %s\n", mapName);
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
        fprintf(f, "map = %s\n", mapName);
    fclose(f);
}

