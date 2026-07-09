/*
 * xbox_compat_globals.c - Definitions for PC-port / PsyCross globals the shared
 * decomp reads under SH_PC_PORT. Xbox defaults are PSX-native: square pixels, no
 * widescreen, culling on, original (PSX) movement. g_PcConfig replaces the
 * config-file parser (pc_config.c, excluded) with fixed sane values.
 */
#include "pc_config.h"     /* s_PcConfig (no includes; SDL-free) */
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
unsigned char g_PcTrustedBloodColor = 0;
s_CollStateDbg g_CollStateDbg = { 0 };

/* Fixed config (PSX-native). Fields the decomp reads under SH_PC_PORT; the rest
 * default to 0 = off. windowWidth/Height must be non-zero (aspect-ratio math). */
s_PcConfig g_PcConfig = {
    .windowWidth     = 640,
    .windowHeight    = 480,
    .pixelAspectMode = 1,   /* 1.0 = square = PSX (per main_pc.c) */
    .disableCulling  = 0,   /* PSX-accurate. (The sliver is a GTE projection issue,
                             * not this flag — flipping it had no visible effect.) */
    .preloadChunks   = 0,   /* stream chunks like PSX */
    .vsync           = 1,
    .refreshRate     = 60,
    .fpsCap          = 30,  /* PSX-accurate */
    /* Skip the Konami/Kcet logos + intro movie and jump Boot->MainMenu. This is a
     * supported PC-port path (Settings_RestoreDefaults runs in its place). We use
     * it for now because the logos' "checking memory card" step spins forever on
     * the stubbed memcard HAL (func_80033548 never reports ready). Revisit once a
     * real memcard HAL (HDD-backed) + FMV are implemented. */
    .skipIntros      = 1,
    .widescreenMode  = 0,   /* pillarbox / PSX-faithful */
    .menuPillarbox   = 0,
    .allowLooseFiles = 0,
    .movementOriginal = 1,  /* PSX lower-body movement */
    .mapName         = "map0_s00",
};
