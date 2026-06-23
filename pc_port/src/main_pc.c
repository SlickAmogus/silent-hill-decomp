/*
 * main_pc.c - Silent Hill PC Port entry point
 *
 * This replaces the PSX main() with a PC-compatible version that:
 * 1. Initializes SDL2 + OpenGL via PsyCross
 * 2. Sets up the file system to read game data from disk
 * 3. Calls into the original game code
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <time.h>

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "common.h"
#include "game.h"
#include "gpu.h"
#include "sh_log.h"
#include "psx_memory.h"
#include "pc_config.h"
#include "map_registry.h"
#include "main/fsqueue.h"
#include "main/fileinfo.h"
#include "bodyprog/bodyprog.h"
#include "maps/shared/SysWork_StateStepIncrementAfterTime.h"

#include <libgpu.h>
#include <libgte.h>
#include <libetc.h>
#include <libspu.h>
#include <libcd.h>

/* PsyCross public API */
#include <PsyX/PsyX_public.h>
#include <PsyX/common/glad.h>

/* Forward declarations from game code */
extern void MainLoop(void);
extern void Fs_QueueInitialize(void);
extern void PcPort_InitCharaAnimInfo(void);

/* Overlay pointers from main.c - need runtime init on PC */
extern void* g_OvlDynamic;
extern void* g_OvlBodyprog;

/* PC port: master gate for dev/cheat keys (config: allow_debug_controls).
 * Read by DebugCamera_Update + the few stragglers. Off by default. */
int g_PcAllowDebugControls = 0;

/* "Mouse1".."Mouse5" -> SDL mouse button number (Mouse1=left, Mouse2=right,
 * Mouse3=middle, Mouse4=X1, Mouse5=X2). Returns 0 if not a mouse name. */
static int Pc_ParseMouseName(const char* v)
{
    if (!v) return 0;
    if ((v[0] == 'M' || v[0] == 'm') && (v[1] == 'o' || v[1] == 'O') &&
        (v[2] == 'u' || v[2] == 'U') && (v[3] == 's' || v[3] == 'S') &&
        (v[4] == 'e' || v[4] == 'E'))
    {
        switch (atoi(v + 5))
        {
            case 1: return SDL_BUTTON_LEFT;
            case 2: return SDL_BUTTON_RIGHT;
            case 3: return SDL_BUTTON_MIDDLE;
            case 4: return SDL_BUTTON_X1;
            case 5: return SDL_BUTTON_X2;
        }
    }
    return 0;
}

/* Build the secondary keyboard mapping + the mouse-button -> PSX-bit table from
 * the key_*_2 config binds. A secondary value is "MouseN", a key name, or
 * "NONE". Re-applied whenever the active control style changes the gate. */
static void Pc_ApplySecondaryBinds(void)
{
    struct { const char* val; unsigned short bit; int* kc; } sec[16] = {
        { g_PcConfig.keyUp2,       0x10,   &g_cfg_keyboardMapping2.kc_dpad_up    },
        { g_PcConfig.keyDown2,     0x40,   &g_cfg_keyboardMapping2.kc_dpad_down  },
        { g_PcConfig.keyLeft2,     0x80,   &g_cfg_keyboardMapping2.kc_dpad_left  },
        { g_PcConfig.keyRight2,    0x20,   &g_cfg_keyboardMapping2.kc_dpad_right },
        { g_PcConfig.keyCross2,    0x4000, &g_cfg_keyboardMapping2.kc_cross      },
        { g_PcConfig.keyCircle2,   0x2000, &g_cfg_keyboardMapping2.kc_circle     },
        { g_PcConfig.keyTriangle2, 0x1000, &g_cfg_keyboardMapping2.kc_triangle   },
        { g_PcConfig.keySquare2,   0x8000, &g_cfg_keyboardMapping2.kc_square     },
        { g_PcConfig.keyL12,       0x400,  &g_cfg_keyboardMapping2.kc_l1         },
        { g_PcConfig.keyR12,       0x800,  &g_cfg_keyboardMapping2.kc_r1         },
        { g_PcConfig.keyL22,       0x100,  &g_cfg_keyboardMapping2.kc_l2         },
        { g_PcConfig.keyR22,       0x200,  &g_cfg_keyboardMapping2.kc_r2         },
        { g_PcConfig.keyL32,       0x2,    &g_cfg_keyboardMapping2.kc_l3         },
        { g_PcConfig.keyR32,       0x4,    &g_cfg_keyboardMapping2.kc_r3         },
        { g_PcConfig.keyStart2,    0x8,    &g_cfg_keyboardMapping2.kc_start      },
        { g_PcConfig.keySelect2,   0x1,    &g_cfg_keyboardMapping2.kc_select     },
    };
    int i;

    memset(&g_cfg_keyboardMapping2, 0, sizeof(g_cfg_keyboardMapping2));
    for (i = 0; i < 8; i++) g_cfg_mouseButtonMask[i] = 0;

    for (i = 0; i < 16; i++)
    {
        const char* v = sec[i].val;
        int         mb;
        if (!v || !v[0] || strcmp(v, "NONE") == 0) { *sec[i].kc = SDL_SCANCODE_UNKNOWN; continue; }
        mb = Pc_ParseMouseName(v);
        if (mb > 0) { g_cfg_mouseButtonMask[mb] |= sec[i].bit; *sec[i].kc = SDL_SCANCODE_UNKNOWN; }
        else        { *sec[i].kc = PsyX_LookupKeyboardMapping(v, SDL_SCANCODE_UNKNOWN); }
    }

    /* Mouse + alternate binds are always active now. */
    g_cfg_allowMouseSecondary = 1;
}

/* Apply control bindings + movement/debug options from g_PcConfig onto the
 * PsyCross input mapping. Call AFTER PsyX_Initialise (which sets the built-in
 * defaults) so config overrides them; the lookups fall back to the current
 * default when a name is empty/invalid. */
static void Pc_ApplyControlConfig(void)
{
    extern int g_cfg_controllerMovement;

    g_cfg_keyboardMapping.kc_dpad_up    = PsyX_LookupKeyboardMapping(g_PcConfig.keyUp,       g_cfg_keyboardMapping.kc_dpad_up);
    g_cfg_keyboardMapping.kc_dpad_down  = PsyX_LookupKeyboardMapping(g_PcConfig.keyDown,     g_cfg_keyboardMapping.kc_dpad_down);
    g_cfg_keyboardMapping.kc_dpad_left  = PsyX_LookupKeyboardMapping(g_PcConfig.keyLeft,     g_cfg_keyboardMapping.kc_dpad_left);
    g_cfg_keyboardMapping.kc_dpad_right = PsyX_LookupKeyboardMapping(g_PcConfig.keyRight,    g_cfg_keyboardMapping.kc_dpad_right);
    g_cfg_keyboardMapping.kc_cross      = PsyX_LookupKeyboardMapping(g_PcConfig.keyCross,    g_cfg_keyboardMapping.kc_cross);
    g_cfg_keyboardMapping.kc_circle     = PsyX_LookupKeyboardMapping(g_PcConfig.keyCircle,   g_cfg_keyboardMapping.kc_circle);
    g_cfg_keyboardMapping.kc_triangle   = PsyX_LookupKeyboardMapping(g_PcConfig.keyTriangle, g_cfg_keyboardMapping.kc_triangle);
    g_cfg_keyboardMapping.kc_square     = PsyX_LookupKeyboardMapping(g_PcConfig.keySquare,   g_cfg_keyboardMapping.kc_square);
    g_cfg_keyboardMapping.kc_l1         = PsyX_LookupKeyboardMapping(g_PcConfig.keyL1,       g_cfg_keyboardMapping.kc_l1);
    g_cfg_keyboardMapping.kc_r1         = PsyX_LookupKeyboardMapping(g_PcConfig.keyR1,       g_cfg_keyboardMapping.kc_r1);
    g_cfg_keyboardMapping.kc_l2         = PsyX_LookupKeyboardMapping(g_PcConfig.keyL2,       g_cfg_keyboardMapping.kc_l2);
    g_cfg_keyboardMapping.kc_r2         = PsyX_LookupKeyboardMapping(g_PcConfig.keyR2,       g_cfg_keyboardMapping.kc_r2);
    g_cfg_keyboardMapping.kc_l3         = PsyX_LookupKeyboardMapping(g_PcConfig.keyL3,       g_cfg_keyboardMapping.kc_l3);
    g_cfg_keyboardMapping.kc_r3         = PsyX_LookupKeyboardMapping(g_PcConfig.keyR3,       g_cfg_keyboardMapping.kc_r3);
    g_cfg_keyboardMapping.kc_start      = PsyX_LookupKeyboardMapping(g_PcConfig.keyStart,    g_cfg_keyboardMapping.kc_start);
    g_cfg_keyboardMapping.kc_select     = PsyX_LookupKeyboardMapping(g_PcConfig.keySelect,   g_cfg_keyboardMapping.kc_select);

    g_cfg_controllerMapping.gc_cross    = PsyX_LookupGameControllerMapping(g_PcConfig.padCross,    g_cfg_controllerMapping.gc_cross);
    g_cfg_controllerMapping.gc_circle   = PsyX_LookupGameControllerMapping(g_PcConfig.padCircle,   g_cfg_controllerMapping.gc_circle);
    g_cfg_controllerMapping.gc_triangle = PsyX_LookupGameControllerMapping(g_PcConfig.padTriangle, g_cfg_controllerMapping.gc_triangle);
    g_cfg_controllerMapping.gc_square   = PsyX_LookupGameControllerMapping(g_PcConfig.padSquare,   g_cfg_controllerMapping.gc_square);
    g_cfg_controllerMapping.gc_l1       = PsyX_LookupGameControllerMapping(g_PcConfig.padL1,       g_cfg_controllerMapping.gc_l1);
    g_cfg_controllerMapping.gc_r1       = PsyX_LookupGameControllerMapping(g_PcConfig.padR1,       g_cfg_controllerMapping.gc_r1);
    g_cfg_controllerMapping.gc_l2       = PsyX_LookupGameControllerMapping(g_PcConfig.padL2,       g_cfg_controllerMapping.gc_l2);
    g_cfg_controllerMapping.gc_r2       = PsyX_LookupGameControllerMapping(g_PcConfig.padR2,       g_cfg_controllerMapping.gc_r2);
    g_cfg_controllerMapping.gc_l3       = PsyX_LookupGameControllerMapping(g_PcConfig.padL3,       g_cfg_controllerMapping.gc_l3);
    g_cfg_controllerMapping.gc_r3       = PsyX_LookupGameControllerMapping(g_PcConfig.padR3,       g_cfg_controllerMapping.gc_r3);
    g_cfg_controllerMapping.gc_start    = PsyX_LookupGameControllerMapping(g_PcConfig.padStart,    g_cfg_controllerMapping.gc_start);
    g_cfg_controllerMapping.gc_select   = PsyX_LookupGameControllerMapping(g_PcConfig.padSelect,   g_cfg_controllerMapping.gc_select);

    g_PcAllowDebugControls  = g_PcConfig.allowDebugControls;
    g_cfg_controllerMovement = g_PcConfig.controllerMovement;

    Pc_ApplySecondaryBinds();
}

/* Demo play file buffer pointer - default PSX address needs runtime init */
typedef struct s_DemoFrameData s_DemoFrameData;
extern s_DemoFrameData* g_Demo_PlayFileBufferPtr;

/* Unified debug log — writes to a fopen'd SilentHill.log handle that
 * doesn't depend on stdout being redirected.  Lets us keep SH_DBG going
 * to the file even when show_console=1 leaves stdout pointed at the
 * visible console window.  Set g_ShDebugEchoStdout from main() after
 * config is parsed. */
FILE* g_ShDebugLog = NULL;
int   g_ShDebugEchoStdout = 0;
void (*g_ShOverlayPushLine)(const char* line) = NULL;
/* Per-run timestamped log path so a new run never overwrites the previous log.
 * Computed once on the first call and cached, so the main log handle and the
 * stdout/stderr freopen all target the same file for this run. */
const char* SH_LogPath(void)
{
    static char s_logPath[64] = {0};
    if (!s_logPath[0]) {
        time_t now = time(NULL);
        struct tm* lt = localtime(&now);
        if (!lt || strftime(s_logPath, sizeof(s_logPath), "SilentHill_%Y%m%d_%H%M%S.log", lt) == 0) {
            snprintf(s_logPath, sizeof(s_logPath), "SilentHill.log");
        }
    }
    return s_logPath;
}

void SH_DebugLogInit(void)
{
    if (!g_ShDebugLog) {
        g_ShDebugLog = fopen(SH_LogPath(), "w");
        if (!g_ShDebugLog) {
            /* Last resort — fall back to stdout so we don't crash on the
             * first SH_DBG. Caller (main) normally pre-opens this. */
            g_ShDebugLog = stdout;
        } else {
            /* Full buffering (64KB). MSVCRT ignores _IOLBF (treats it as
             * _IOFBF), so request _IOFBF explicitly — flushes on buffer-full
             * and on clean exit. Unbuffered (_IONBF) was used for crash
             * diagnosis but it flushes every SH_DBG to disk, which halves
             * framerate under heavy per-frame logging (combat + map churn). */
            static char s_logBuf[64 * 1024];
            setvbuf(g_ShDebugLog, s_logBuf, _IOFBF, sizeof(s_logBuf));
        }
    }
}

/* Final-flush hook: ensure the log is flushed before the process exits
 * (normal or crash). Stdio also runs this via atexit but registering
 * explicitly makes the intent obvious. */
static void Sh_LogAtExitFlush(void) {
    if (g_ShDebugLog && g_ShDebugLog != stdout) fflush(g_ShDebugLog);
}

/* Crash telemetry lives in pc_crash.c (windows.h conflicts with the decomp
 * `byte` typedef, so it can't be included here). */
extern void Sh_InstallCrashFilter(void);


/* Game data path - where the extracted game files are located */
static char g_GameDataPath[512] = "./gamedata";

/* Public accessor — used by xa_player.c (and anything else that needs to
 * locate the disc image at runtime) so we don't sprinkle search-path arrays
 * across the codebase. Always returns a NUL-terminated path. */
const char* PcPort_GetGameDataPath(void)
{
    return g_GameDataPath;
}

/* Resolved disc image path (cached) and its region. The PC port is a single
 * executable that supports multiple disc regions; the active file table / XA
 * offsets are chosen here from whichever disc is present. */
static char g_GameDiscPath[1024] = { 0 };
static int  g_DiscResolved       = 0;

/* Read the boot executable name (SLUS_* / SLES_*) from a BIN's ISO9660 root
 * directory to identify the region. Returns Region_USA / Region_EUR, or -1. */
static int Pc_DetectRegionFromBin(const char* path)
{
    FILE*         f = fopen(path, "rb");
    unsigned char sec[2048];
    unsigned int  rlba;
    unsigned      o;
    int           region = -1;

    if (!f)
        return -1;

    /* PVD at LBA 16 (raw 2352-byte sectors; 2048 data bytes at offset 24). */
    fseek(f, 16 * 2352 + 24, SEEK_SET);
    if (fread(sec, 1, 2048, f) != 2048) { fclose(f); return -1; }
    rlba = sec[156 + 2] | (sec[156 + 3] << 8) | (sec[156 + 4] << 16) | ((unsigned)sec[156 + 5] << 24);

    fseek(f, (long)rlba * 2352 + 24, SEEK_SET);
    if (fread(sec, 1, 2048, f) != 2048) { fclose(f); return -1; }
    fclose(f);

    for (o = 0; o + 33 < 2048; )
    {
        unsigned L  = sec[o];
        unsigned nl = sec[o + 32];
        if (L == 0)
            break;
        if (o + 33 + nl <= 2048 && nl >= 4)
        {
            if (memcmp(&sec[o + 33], "SLUS", 4) == 0) region = Region_USA;
            else if (memcmp(&sec[o + 33], "SLES", 4) == 0) region = Region_EUR;
        }
        o += L;
    }
    return region;
}

/* Locate the disc image and select the matching region tables. Priority:
 * USA, then PAL, then the long European name (US wins if several exist). If
 * none of those names match but some .bin is present, autodetect by region. */
const char* PcPort_GetGameDiscPath(void)
{
    static const struct { const char* name; int region; } s_known[] = {
        { "Silent Hill (USA).bin",                        Region_USA },
        { "Silent Hill (PAL).bin",                        Region_EUR },
        { "Silent Hill (Europe) (En,Fr,De,Es,It).bin",    Region_EUR },
    };
    char path[1024];
    int  i;
    DIR* dir;

    if (g_DiscResolved)
        return g_GameDiscPath;
    g_DiscResolved = 1;

    for (i = 0; i < (int)(sizeof(s_known) / sizeof(s_known[0])); i++)
    {
        FILE* f;
        snprintf(path, sizeof(path), "%s/%s", g_GameDataPath, s_known[i].name);
        f = fopen(path, "rb");
        if (f)
        {
            fclose(f);
            snprintf(g_GameDiscPath, sizeof(g_GameDiscPath), "%s", path);
            Fs_InitFileTableForRegion((e_GameRegion)s_known[i].region);
            SH_LOG("Disc: %s (region %s)", s_known[i].name,
                   s_known[i].region == Region_EUR ? "EUR/PAL" : "USA");
            return g_GameDiscPath;
        }
    }

    /* Last resort: autodetect any other .bin by its ISO boot serial. Scan them
     * all and still prefer a US disc over a PAL one, so named files and US
     * always take precedence. */
    dir = opendir(g_GameDataPath);
    if (dir)
    {
        struct dirent* ent;
        char usPath[1024] = { 0 };
        char euPath[1024] = { 0 };

        while ((ent = readdir(dir)) != NULL)
        {
            const char* nm = ent->d_name;
            size_t      l  = strlen(nm);
            if (l > 4 && (strcmp(nm + l - 4, ".bin") == 0 || strcmp(nm + l - 4, ".BIN") == 0))
            {
                int r;
                snprintf(path, sizeof(path), "%s/%s", g_GameDataPath, nm);
                r = Pc_DetectRegionFromBin(path);
                if (r == Region_USA && !usPath[0])
                    snprintf(usPath, sizeof(usPath), "%s", path);
                else if (r == Region_EUR && !euPath[0])
                    snprintf(euPath, sizeof(euPath), "%s", path);
            }
        }
        closedir(dir);

        if (usPath[0] || euPath[0])
        {
            int useEur = (!usPath[0] && euPath[0]);
            snprintf(g_GameDiscPath, sizeof(g_GameDiscPath), "%s", useEur ? euPath : usPath);
            Fs_InitFileTableForRegion(useEur ? Region_EUR : Region_USA);
            SH_LOG("Disc autodetected: %s (region %s)", g_GameDiscPath,
                   useEur ? "EUR/PAL" : "USA");
            return g_GameDiscPath;
        }
    }

    Fs_InitFileTableForRegion(Region_USA); /* keep g_FileTable populated even with no disc */
    g_GameDiscPath[0] = '\0';
    SH_WARN("No Silent Hill disc image (.bin) found in %s", g_GameDataPath);
    return g_GameDiscPath;
}

static void PrintBanner(void)
{
    printf("==============================================\n");
    printf("  Silent Hill - PC Port\n");
    printf("  Based on the Silent Hill Decompilation\n");
    printf("  https://github.com/Vatuu/silent-hill-decomp\n");
    printf("==============================================\n");
    printf("\n");
}

static void ParseArgs(int argc, char* argv[])
{
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-data") == 0 && i + 1 < argc)
        {
            strncpy(g_GameDataPath, argv[i + 1], sizeof(g_GameDataPath) - 1);
            g_GameDataPath[sizeof(g_GameDataPath) - 1] = '\0';
            i++;
        }
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
            printf("Usage: SilentHillPC [options]\n");
            printf("Options:\n");
            printf("  -data <path>    Path to game data directory or CD image\n");
            printf("  -h, --help      Show this help\n");
            exit(0);
        }
    }
}

int main(int argc, char* argv[])
{
    /* Log file is NOT opened until after config load. SH_DBG calls before
     * that point are silently no-ops (the macro short-circuits on a NULL
     * handle). Avoids creating SilentHill.log when enable_debug_log=0. */
    atexit(Sh_LogAtExitFlush);
    Sh_InstallCrashFilter();

    PrintBanner();
    ParseArgs(argc, argv);

    /* Load config file */
    PcConfig_Load("config.cfg");

    /* Now that we know whether logging is enabled, open the log file (or
     * leave g_ShDebugLog NULL so SH_DBG stays a no-op). */
    if (g_PcConfig.enableDebugLog) {
        SH_DebugLogInit();
        SH_DBG("[SH] main() entered (log opened post-config)");
        {
            /* Build identification — first thing to check in user logs.
             * Generated fresh each build by cmake/gen_build_info.cmake. */
            #include "sh_build_info.h"
            SH_DBG("[SH] build " SH_BUILD_GIT_HASH " (" SH_BUILD_STAMP ")");
        }
    }

    /* PsyCross horizontal pixel-aspect compensation. Silent Hill renders a 320x224
     * framebuffer the PSX displays as a 4:3 picture, so its pixels are NOT square
     * (PAR = (4/3)/(320/224) = 14/15). PsyCross's Hor+ ortho and the matching game-side
     * cull bounds scale the framebuffer-aspect horizontal extent by g_PsxPixelAspect;
     * the value that restores the 4:3 picture is (320/224)*(3/4) = 15/14 ~= 1.0714.
     * Baked in — it was a config knob, but no other value is correct for this game. */
    {
        extern float g_PsxPixelAspect;
        g_PsxPixelAspect = (320.0f / 224.0f) * (3.0f / 4.0f);
    }

    /* Apply widescreen mode to PsyCross. */
    {
        extern int g_PcWidescreenMode;
        extern int g_PcMenuPillarbox;
        g_PcWidescreenMode = g_PcConfig.widescreenMode;
        g_PcMenuPillarbox  = g_PcConfig.menuPillarbox;
    }

    /* Console modes:
     *   0 = off       — hide window, no echo
     *   1 = external  — show console window, SH_DBG_ECHO/SH_LOG to stdout
     *   2 = ingame    — overlay only: SH_DBG_ECHO/SH_LOG + [ ] markers, no window
     *   3 = both      — overlay + console window (same overlay content as 2) */
    {
        int show = g_PcConfig.showConsole;
        if (show == 1 || show == 3) {
            /* GUI-subsystem app: no console exists at launch, so create one for
             * external mode and point stdout/stderr at it. */
            extern __declspec(dllimport) int __stdcall AllocConsole(void);
            AllocConsole();
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);
            g_ShDebugEchoStdout = 1;
            setvbuf(stdout, NULL, _IONBF, 0);
            setvbuf(stderr, NULL, _IONBF, 0);
        } else {
            /* No console in a GUI-subsystem app — just route stdout/stderr to
             * the log file (or NUL) so stray printf doesn't hit an invalid handle. */
            if (g_PcConfig.enableDebugLog) {
                freopen(SH_LogPath(), "a", stdout);
                freopen(SH_LogPath(), "a", stderr);
                setvbuf(stdout, NULL, _IONBF, 0);
                setvbuf(stderr, NULL, _IONBF, 0);
            } else {
                freopen("NUL", "w", stdout);
                freopen("NUL", "w", stderr);
            }
        }
        /* Always capture log lines into the overlay ring buffer so the in-game
         * console can be toggled on at runtime (`~`) and immediately show recent
         * output, even when it booted disabled. Visibility is gated in
         * DbgOverlay_Render by (showConsole & 2). */
        {
            extern void DbgOverlay_PushLine(const char* line);
            g_ShOverlayPushLine = DbgOverlay_PushLine;
        }
    }
    int windowWidth = g_PcConfig.windowWidth;
    int windowHeight = g_PcConfig.windowHeight;

    SH_LOG("Game data path: %s", g_GameDataPath);

    /* Initialize PSX memory emulation */
    SH_LOG("Initializing PSX memory emulation...");
    PsxMemory_Init();
    SH_LOG("PSX RAM base: %p", (void*)g_PsxRam);
    SH_LOG("TEMP_MEMORY_ADDR -> %p (offset 0x1A2600)", (void*)TEMP_MEMORY_ADDR);

    /* Initialize runtime data that depends on PSX memory addresses */
    PcPort_InitCharaAnimInfo();
    extern void PcPort_InitSdBuffers(void);
    PcPort_InitSdBuffers();

    /* Translate the Air Screamer per-keyframe AI rodata from PSX layout
     * (16-byte s_AnimInfo, 4-byte ptrs) to PC layout (32-byte s_AnimInfo,
     * 8-byte ptrs). Without this, every read past animInfo_0[] in
     * sharedData_800CAA98_0_s01 returns garbage from a wrong offset and
     * the AS AI breaks (or the earlier band-aids force a degraded
     * fallback). See pc_port/src/as_rodata_reformat.c. */
    extern void AsRodata_Reformat(void);
    AsRodata_Reformat();

    /* Populate GROANER_ANIM_INFOS — its playbackFunc fields point to
     * Anim_BlendLinear / Anim_PlaybackOnce / Anim_PlaybackLoop, which
     * MinGW won't accept in a static initializer (treats function
     * symbols from another TU as non-constant). Built at runtime. */
    extern void GroanerAnimInfos_Init(void);
    GroanerAnimInfos_Init();

    extern void BloodsuckerAnimInfos_Init(void);
    BloodsuckerAnimInfos_Init();
    extern void BloodyLisaAnimInfos_Init(void);
    BloodyLisaAnimInfos_Init();
    extern void AlessaAnimInfos_Init(void);
    AlessaAnimInfos_Init();
    extern void GhostChildAlessaAnimInfos_Init(void);
    GhostChildAlessaAnimInfos_Init();
    extern void LisaAnimInfos_Init(void);
    LisaAnimInfos_Init();
    extern void KaufmannAnimInfos_Init(void);
    KaufmannAnimInfos_Init();
    extern void DahliaAnimInfos_Init(void);
    DahliaAnimInfos_Init();
    extern void CatAnimInfos_Init(void);
    CatAnimInfos_Init();
    extern void PuppetNurseData_Init(void);
    PuppetNurseData_Init();
    extern void LarvalStalkerAnimInfos_Init(void);
    LarvalStalkerAnimInfos_Init();
    extern void HangedScratcherAnimInfos_Init(void);
    HangedScratcherAnimInfos_Init();
    extern void CreeperAnimInfos_Init(void);
    CreeperAnimInfos_Init();
    extern void SplitHeadAnimInfos_Init(void);
    SplitHeadAnimInfos_Init();
    extern void RomperAnimInfos_Init(void);
    RomperAnimInfos_Init();

    /* Binary-extracted batch (2026-06-10): bosses + late-game cast that were
     * still zero-stubs. Generated by pc_port/tools/extract_anim_infos.py. */
    extern void LockerDeadBodyAnimInfos_Init(void);
    LockerDeadBodyAnimInfos_Init();
    extern void TwinfeelerAnimInfos_Init(void);
    TwinfeelerAnimInfos_Init();
    extern void FloatstingerAnimInfos_Init(void);
    FloatstingerAnimInfos_Init();
    extern void MonsterCybilAnimInfos_Init(void);
    MonsterCybilAnimInfos_Init();
    extern void FlaurosAnimInfos_Init(void);
    FlaurosAnimInfos_Init();
    extern void ParasiteAnimInfos_Init(void);
    ParasiteAnimInfos_Init();
    extern void GhostDoctorAnimInfos_Init(void);
    GhostDoctorAnimInfos_Init();
    extern void BloodyIncubatorAnimInfos_Init(void);
    BloodyIncubatorAnimInfos_Init();
    extern void IncubatorAnimInfos_Init(void);
    IncubatorAnimInfos_Init();
    extern void LittleIncubusAnimInfos_Init(void);
    LittleIncubusAnimInfos_Init();
    extern void IncubusAnimInfos_Init(void);
    IncubusAnimInfos_Init();
    extern void Unkkown23AnimInfos_Init(void);
    Unkkown23AnimInfos_Init();

    extern void Map6S04ExtraAnimInfos_Init(void);
    Map6S04ExtraAnimInfos_Init();

    /* map7_s03 ending DMS phase pointers: D_800ED230[phase] selects which FS
     * buffer holds the active cutscene's reformatted DMS header. Was a zero-stub
     * (NULL) which forced the DMS redirect onto the single latest g_DmsHeapHeader,
     * desyncing the multi-phase ending. FS_BUFFER_* are g_PsxRam-relative so this
     * must run after PsxMemory_Init (above). */
    {
        extern void* D_800ED230[2];
        D_800ED230[0] = FS_BUFFER_20;
        D_800ED230[1] = FS_BUFFER_18;
    }

    /* Initialize overlay pointers to emulated PSX RAM */
#if VERSION_IS(JAP0)
    g_OvlDynamic  = PSX_ADDR(0x000CBAA8);
#else
    g_OvlDynamic  = PSX_ADDR(0x000C9578);
#endif
    g_OvlBodyprog = PSX_ADDR(0x00024B60);
    g_Demo_PlayFileBufferPtr = (s_DemoFrameData*)PSX_ADDR(0x000F5E00);

    /* Keyboard mapping is set by PsyCross defaults in PsyX_Initialise:
     * Cross=C, Circle=V, Triangle=Z, Square=X, Start=Enter, Select=Space
     * DPad=Arrow keys, L1=LShift, R1=RShift, L2=LCtrl, R2=RCtrl */

    /* Route PsyCross logging into our SilentHill.log handle (or silence it
     * when enable_debug_log=0) BEFORE PsyX_Initialise, so PsyCross never
     * creates its own "Silent Hill.log" and never fcloses our handle at
     * shutdown (it used to, leaving g_ShDebugLog dangling for any logging
     * after PsyX_Shutdown). */
    PsyX_Log_SetStream(g_PcConfig.enableDebugLog ? g_ShDebugLog : NULL);

    /* Initialize PsyCross (creates SDL2 window + OpenGL context) */
    SH_LOG("Initializing PsyCross (SDL2 + OpenGL)...");
    PsyX_Initialise("Silent Hill", windowWidth, windowHeight, g_PcConfig.fullscreen);

    SH_LOG("PsyCross initialized. Window: %dx%d", windowWidth, windowHeight);

    {
        const char* gl_renderer = (const char*)glGetString(GL_RENDERER);
        const char* gl_vendor   = (const char*)glGetString(GL_VENDOR);
        const char* gl_version  = (const char*)glGetString(GL_VERSION);
        SH_LOG("GL Renderer: %s", gl_renderer ? gl_renderer : "(null)");
        SH_LOG("GL Vendor:   %s", gl_vendor   ? gl_vendor   : "(null)");
        SH_LOG("GL Version:  %s", gl_version  ? gl_version  : "(null)");
    }

    /* Apply keyboard/controller bindings + movement/debug options from config
     * (overrides the PsyCross defaults set inside PsyX_Initialise). */
    Pc_ApplyControlConfig();

    /* Apply the saved control style + publish the style registry to config.cfg
     * so the launcher's Control Style dropdown reflects this build. */
    {
        extern void Pc_ControlStyleInit(void);
        Pc_ControlStyleInit();
    }

    /* Bring the game window to the foreground on launch. SilentHillPC.exe is a
     * console-subsystem app, so Windows spawns a console window at startup that
     * grabs focus before this SDL window exists (and, with console off, is then
     * FreeConsole'd). The launcher's SetForegroundWindow targets the process'
     * MainWindowHandle, which resolves to that console (or zero), so the game
     * window never reliably gets focus. Raise our own window here — the
     * launcher's AllowSetForegroundWindow grant lets this take the foreground. */
    {
        extern SDL_Window* g_window;
        if (g_window)
        {
            SDL_RaiseWindow(g_window);
        }
    }

    /* Apply refresh rate and vsync from config.
     * PsyCross defaults to vsync=off; we override via SDL directly.
     * Exclusive fullscreen only — borderless (fullscreen==2) runs at the
     * desktop mode, where SDL_SetWindowDisplayMode has no effect. */
    if (g_PcConfig.refreshRate > 0 && g_PcConfig.fullscreen == 1)
    {
        extern SDL_Window* g_window;
        SDL_DisplayMode mode;
        if (SDL_GetWindowDisplayMode(g_window, &mode) == 0)
        {
            mode.refresh_rate = g_PcConfig.refreshRate;
            if (SDL_SetWindowDisplayMode(g_window, &mode) == 0)
                SH_LOG("Display mode set to %d hz", g_PcConfig.refreshRate);
            else
                SH_LOG("Failed to set %d hz display mode: %s", g_PcConfig.refreshRate, SDL_GetError());
        }
    }
    if (g_PcConfig.vsync != 0)
    {
        SDL_GL_SetSwapInterval(g_PcConfig.vsync);
        SH_LOG("VSync set to %d", g_PcConfig.vsync);
    }

    /* Apply texture-filtering mode from config: 0 = neither, 1 = PSX
     * dither, 2 = bilinear. Mutually exclusive — bilinear softens
     * everything while dither keeps the original look but masks the
     * texture-page seam artifacts and adds the authentic PSX noise. */
    switch (g_PcConfig.psxDither) {
    case 1:  g_cfg_psxDither = 1; g_cfg_bilinearFiltering = 0; break;
    case 2:  g_cfg_psxDither = 0; g_cfg_bilinearFiltering = 1; break;
    default: g_cfg_psxDither = 0; g_cfg_bilinearFiltering = 0; break;
    }
    SH_LOG("Filtering: %s",
           g_cfg_psxDither ? "PSX dither" :
           g_cfg_bilinearFiltering ? "bilinear" : "off");

    /* PGXP master gate: PsyCross is compiled with USE_PGXP=1, but the
     * runtime path is opt-in via config.cfg use_pgxp. When 0, prim emit
     * writes a_zw=0 and the vertex shader takes the 2D-ortho branch
     * (PSX-affine look). When 1, GTE captures FP twins and shader does
     * perspective-correct projection via Projection3D + cache lookups.
     * (declared in PsyX/PsyX_public.h, defined in PsyX_render.cpp) */
    g_PsxUsePgxp = g_PcConfig.usePgxp ? 1 : 0;
    SH_LOG("PGXP: %s", g_PsxUsePgxp ? "ON (perspective-correct, WIP)" : "off (affine)");

    /* Initialize PSY-Q subsystems via PsyCross */
    SH_LOG("Initializing PSY-Q subsystems...");
    ResetCallback();
    SpuInit();

    /* Initialize CD filesystem - try loading from image or directory */
    SH_LOG("Initializing CD filesystem...");
    {
        /* Resolve the disc (US/PAL), select the region's file table, and open it. */
        const char* cdImagePath = PcPort_GetGameDiscPath();

        if (cdImagePath[0]) {
            SH_LOG("CD image found, initializing CDFS...");
            PsyX_CDFS_Init(cdImagePath, 0, 0);
        } else {
            SH_WARN("Game will not be able to load assets without a disc image.");
        }
    }

    /* Region-specific data tweaks now that g_GameRegion is known (e.g. PAL's
     * Grey-Child -> Mumbler model swap). */
    { extern void CharaData_ApplyRegionPatches(void); CharaData_ApplyRegionPatches(); }

    CdInit();

    /* Initialize GPU */
    SH_LOG("Initializing GPU...");
    ResetGraph(0);
    SetGraphDebug(0);

    /* Initialize file system queue */
    SH_LOG("Initializing filesystem queue...");
    Fs_QueueInitialize();

    /* Initialize map registry — sets g_pMapOverlayHeader based on config.cfg.
     * Must happen after PcPort_InitCharaAnimInfo (anim stubs) but before MainLoop. */
    SH_LOG("Initializing map registry...");
    MapRegistry_Init();
    SH_LOG("Active map: %s", g_PcConfig.mapName);

    SH_LOG("All subsystems initialized. Entering MainLoop...");

    /* The graphic-content warning ("There are violent and disturbing
     * images in this game") used to fire here, but it ran before
     * MainLoop's GsInitVcount/InitGeom/SD_Init so subsequent boot
     * states (Konami, KCET) saw a noticeable load gap. Moved into
     * MainLoop's startup phase right before the game-state loop —
     * see src/bodyprog/sys/game_main.c near the SD_Init block. */

    /*
     * On PSX, main() loads BODYPROG.BIN and B_KONAMI.BIN overlays,
     * then calls MainLoop() which is in BODYPROG.
     *
     * On PC, everything is statically linked, so we call MainLoop() directly.
     */
    MainLoop();

    /* Cleanup */
    SH_DBG("[SH] MainLoop exited normally. Shutting down...");
    PsyX_Shutdown();

    return 0;
}
