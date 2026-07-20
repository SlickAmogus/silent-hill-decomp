/*
 * xbox_compat_globals.c - Definitions for PC-port / PsyCross globals the shared
 * decomp reads under SH_PC_PORT. Xbox defaults are PSX-native: square pixels, no
 * widescreen, culling on, original (PSX) movement. g_PcConfig replaces the
 * config-file parser (pc_config.c, excluded) with fixed sane values.
 */
#include <string.h>
#include "pc_config.h"     /* s_PcConfig + g_PcConfig (defined in pc_config.c) */
#include "dbg_overlay.h"   /* s_CollStateDbg (SDL-free, SH_PC_PORT-gated) */

/* PsyCross runtime globals (normally set by main_pc.c / PsyX). */
float g_PsxPixelAspect       = 1.0f;   /* square pixels = PSX CRT */
float g_PsyX_FogColor[3]     = { 0.0f, 0.0f, 0.0f };
int   g_PsxSkipFramebufferStore = 0;   /* PC semantics: per-frame opt-out of the
                                        * framebuffer->VRAM readback. The game re-sets
                                        * it =1 each tick of a TIM-protect screen (paper
                                        * map: bodyprog_80085D78.c) and VSync auto-clears
                                        * it (GpuXbox_FbStoreFrameTick) — PsyX_EndScene
                                        * parity. The readback itself is demand-driven
                                        * (gpu_xbox.c FbReadback), never per-frame. */
int   g_PsxDitherSuppressed  = 0;
int   g_PsxPresentLastFrame  = 0;
int   g_rcnt2_timer_active   = 0;

/* PC-port master toggles — all off / PSX-faithful on Xbox. */
int g_PcAllowDebugControls   = 0;
int g_PcConsoleInputActive   = 0;
int g_PcConsoleSwallowInput  = 0;
int g_PcHorPlusEnabled       = 0;
int g_PcMenuPillarbox        = 0;
int g_windowWidth            = 640;
int g_windowHeight           = 480;
int g_CollVisEnabled         = 0;
/* g_PcTrustedBloodColor: defined by pc_config.c (compiled on Xbox now). */
s_CollStateDbg g_CollStateDbg = { 0 };

/* g_PcConfig itself is DEFINED by pc_port/src/pc_config.c, which the Xbox now
 * compiles (it is SDL-free). That gives us PC's complete default initializer for
 * free — as the PC port adds config fields, the Xbox inherits sane values instead
 * of silently zeroing them (a zeroed xaVolume would mute cutscene voices; a zeroed
 * fpsFov would break the FPS projection).
 *
 * What we MUST override are the PC defaults tuned for a 64-bit machine with
 * gigabytes of RAM. On a 64MB console several of them are fatal, not merely
 * suboptimal: texpackCacheMb=2048 and texpackBudgetMb=6144 are 32x and 96x the
 * console's total memory, and globalCharaPool/residentTextures deliberately keep
 * every monster's model+anim+texture resident.
 *
 * Called from main_xbox.c AFTER PcConfig_Load, so a config file on the console can
 * still tune anything we do not hard-pin here. */
void XboxConfig_ApplyOverrides(void)
{
    /* --- Memory: the non-negotiable ones (PC defaults would exhaust 64MB). --- */
    g_PcConfig.residentTextures = 0;   /* PC 1: expanded chunk-texture pool */
    g_PcConfig.texturePacks     = 0;   /* PC 1: HD DuckStation packs */
    g_PcConfig.texpackCacheMb   = 0;   /* PC 2048 (MB) */
    g_PcConfig.texpackBudgetMb  = 0;   /* PC 6144 (MB) */
    g_PcConfig.globalCharaPool  = 0;   /* PC 1: every chara's assets resident */
    g_PcConfig.preloadChunks    = 0;   /* PC 1: stream chunks like PSX instead */
    g_PcConfig.wholeMapExteriors = 0;  /* draws every loaded exterior chunk */
    g_PcConfig.randomizer       = 0;   /* would re-force globalCharaPool=1 if the
                                        * rando init were ever wired up here */

    /* --- Untested-on-Xbox I/O paths. --- */
    g_PcConfig.allowLooseFiles  = 0;   /* fsqueue loose-file probe per read */

    /* --- Performance: the 733MHz CPU / 233MHz NV2A cannot absorb these. --- */
    g_PcConfig.disableCulling = 0;     /* PC 1: keep the PSX subcell PVS on */
    g_PcConfig.msaaSamples    = 0;
    g_PcConfig.postProcess    = 0;
    g_PcConfig.tonemap        = 0;

    /* --- Presentation: PSX-faithful 4:3, and the user's requested pillarbox. --- */
    g_PcConfig.widescreenMode = 0;     /* PC 1 (Hor+): pillarbox instead */
    g_PcConfig.menuPillarbox  = 1;     /* 2D screens keep their 4:3 bars */
    g_PcConfig.windowWidth    = 640;
    g_PcConfig.windowHeight   = 480;
    g_PcConfig.fullscreen     = 0;
    g_PcConfig.vsync          = 1;
    /* THE frame-pacing setting, and the reason for the reported stutter. The
     * pacing branch in game_main.c is:
     *     if (vsync != 0 && refreshRate > 0) effectiveFps = refreshRate;
     *     else if (...) ... else effectiveFps = fpsCap;
     * refreshRate wins outright, so the old vsync=1 + refreshRate=60 asked a
     * 733MHz CPU for 60fps and fpsCap=30 was never read. The console cannot hold
     * 60 in the town, so frame time oscillated between the 1- and 2-vblank
     * quantisations — "sometimes a solid 60 but constantly stuttering". 30 is
     * also the PSX-native rate: effectiveFps=30 -> effectiveMin = 60/30 = 2. */
    g_PcConfig.refreshRate    = 30;
    g_PcConfig.fpsCap         = 30;

    /* --- GL-shader features with no NV2A implementation. --- */
    g_PcConfig.usePgxp            = 0;
    g_PcConfig.perPixelFlashlight = 0;
    g_PcConfig.flashlightShadows  = 0; /* PC 1 */
    g_PcConfig.flashlightMode     = 0; /* Classic (PSX) */

    /* --- No mouse, no console window, no launcher. --- */
    g_PcConfig.mouseCursor         = 0; /* PC 1 */
    g_PcConfig.allowMouseSecondary = 0; /* PC 1 */
    g_PcConfig.showConsole         = 0;
    g_PcConfig.allowDebugControls  = 0;

    /* --- We are blind to the screen: the D: log is the only diagnostic. --- */
    g_PcConfig.enableDebugLog = 1;      /* PC 0 */

    /* --- Audio: 0 = auto; dsound_xbox.c reads the EEPROM for the real layout. --- */
    g_PcConfig.audioOutput = 0;
    g_PcConfig.adsr        = 1;

    strncpy(g_PcConfig.mapName, "map0_s00", sizeof(g_PcConfig.mapName) - 1);
    g_PcConfig.mapName[sizeof(g_PcConfig.mapName) - 1] = '\0';
}
