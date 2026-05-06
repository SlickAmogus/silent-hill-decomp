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
#include "bodyprog/bodyprog.h"

#include <libgpu.h>
#include <libgte.h>
#include <libetc.h>
#include <libspu.h>
#include <libcd.h>

/* PsyCross public API */
#include <PsyX/PsyX_public.h>

/* Forward declarations from game code */
extern void MainLoop(void);
extern void Fs_QueueInitialize(void);
extern void PcPort_InitCharaAnimInfo(void);

/* Overlay pointers from main.c - need runtime init on PC */
extern void* g_OvlDynamic;
extern void* g_OvlBodyprog;

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
void SH_DebugLogInit(void)
{
    if (!g_ShDebugLog) {
        g_ShDebugLog = fopen("SilentHill.log", "w");
        if (!g_ShDebugLog) {
            /* Last resort — fall back to stdout so we don't crash on the
             * first SH_DBG. Caller (main) normally pre-opens this. */
            g_ShDebugLog = stdout;
        } else {
            /* Line-buffer (was _IONBF). With unbuffered, every SH_DBG hit
             * disk; the knife combat path emits 2-3k logs/frame and that
             * halved framerate. _IOLBF flushes only on '\n' which we add
             * to every log line, plus stdio auto-flushes on crash via
             * atexit. Set a 64KB buffer to amortize across multi-line
             * burst logs. */
            static char s_logBuf[64 * 1024];
            setvbuf(g_ShDebugLog, s_logBuf, _IOLBF, sizeof(s_logBuf));
        }
    }
}

/* Final-flush hook: ensure the log is flushed before the process exits
 * (normal or crash). Stdio also runs this via atexit but registering
 * explicitly makes the intent obvious. */
static void Sh_LogAtExitFlush(void) {
    if (g_ShDebugLog && g_ShDebugLog != stdout) fflush(g_ShDebugLog);
}


/* Game data path - where the extracted game files are located */
static char g_GameDataPath[512] = "./gamedata";

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
    /* Open the log file FIRST so any SH_DBG before config-load reaches it. */
    SH_DebugLogInit();
    atexit(Sh_LogAtExitFlush);
    SH_DBG("[SH] main() entered");

    PrintBanner();
    SH_DBG("[SH] sizeof(s_WorldGfxWork) = %zu", sizeof(s_WorldGfxWork));
    ParseArgs(argc, argv);

    /* Load config file */
    PcConfig_Load("config.cfg");

    /* Apply pixel-aspect mode to PsyCross's runtime PAR global. */
    {
        extern float g_PsxPixelAspect;
        switch (g_PcConfig.pixelAspectMode) {
            case 2:  g_PsxPixelAspect = 1.0f;     break; /* square */
            case 3:  g_PsxPixelAspect = 1.143f;   break; /* 8:7 */
            case 1:
            default: g_PsxPixelAspect = 1.09375f; break; /* CRT NTSC */
        }
    }

    /* Console window: hide by default + redirect stdout/stderr -> log so
     * stray printf/fprintf get captured.  When show_console=1, leave the
     * console window visible and let stdout/stderr stay pointed at it.
     * SH_DBG always writes to g_ShDebugLog (a separately-fopen'd FILE*)
     * so the log file gets the full stream either way. */
    if (g_PcConfig.showConsole) {
        g_ShDebugEchoStdout = 1;
        setvbuf(stdout, NULL, _IONBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
        SH_DBG_ECHO("[SH] show_console=1 — console window left visible, SH_DBG_ECHO mirrored to stdout");
    } else {
        freopen("SilentHill.log", "a", stdout);
        freopen("SilentHill.log", "a", stderr);
        setvbuf(stdout, NULL, _IONBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
        /* Hide console window — raw Win32 to avoid windows.h conflicts */
        {
            typedef void* HWND;
            extern __declspec(dllimport) HWND __stdcall GetConsoleWindow(void);
            extern __declspec(dllimport) int  __stdcall ShowWindow(HWND, int);
            HWND con = GetConsoleWindow();
            if (con) ShowWindow(con, /*SW_HIDE*/ 0);
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

    /* Initialize PsyCross (creates SDL2 window + OpenGL context) */
    SH_LOG("Initializing PsyCross (SDL2 + OpenGL)...");
    PsyX_Initialise("Silent Hill", windowWidth, windowHeight, g_PcConfig.fullscreen);

    /* Redirect PsyCross log into our SilentHill.log (g_ShDebugLog) instead
     * of a separate "Silent Hill.log" file.  Pointing at g_ShDebugLog
     * directly (not stdout) means PsyCross logs always reach the file even
     * in show_console=1 mode where stdout stays connected to the console. */
    {
        extern FILE* g_logStream;
        if (g_logStream && g_logStream != g_ShDebugLog) {
            fclose(g_logStream);
            remove("Silent Hill.log");
        }
        g_logStream = g_ShDebugLog;
    }

    SH_LOG("PsyCross initialized. Window: %dx%d", windowWidth, windowHeight);

    /* Apply refresh rate and vsync from config.
     * PsyCross defaults to vsync=off; we override via SDL directly. */
    if (g_PcConfig.refreshRate > 0 && g_PcConfig.fullscreen)
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

    /* Initialize PSY-Q subsystems via PsyCross */
    SH_LOG("Initializing PSY-Q subsystems...");
    ResetCallback();
    SpuInit();

    /* Initialize CD filesystem - try loading from image or directory */
    SH_LOG("Initializing CD filesystem...");
    {
        char cdImagePath[512];
        snprintf(cdImagePath, sizeof(cdImagePath), "%s/Silent Hill (USA).bin", g_GameDataPath);

        SH_LOG("Looking for CD image: %s", cdImagePath);

        FILE* f = fopen(cdImagePath, "rb");
        if (f) {
            fclose(f);
            SH_LOG("CD image found, initializing CDFS...");
            PsyX_CDFS_Init(cdImagePath, 0, 0);
        } else {
            SH_WARN("CD image not found at %s", cdImagePath);
            SH_WARN("Game will not be able to load assets without a disc image.");
        }
    }

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

    /* Graphic-content warning ("There are violent and disturbing images
     * in this game"). Plays before MainLoop so it shows before the
     * Konami logo, mirroring the PSX boot order. Uses the OT-based
     * pipeline rather than PSX's immediate-mode DrawPrim because
     * PsyCross's DrawPrim doesn't present without a full
     * GsSwapDispBuff+GsDrawOt frame. Skipped if skip_intros=1 in
     * config.cfg. */
    {
        extern void Pc_PlayWarningScreen(void);
        Pc_PlayWarningScreen();
    }

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
    SH_DBG("[SH] Clean shutdown complete.");

    return 0;
}
