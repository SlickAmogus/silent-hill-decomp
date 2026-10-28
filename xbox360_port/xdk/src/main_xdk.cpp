/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * main_xdk.cpp - Xbox 360 entry point for the XDK build.
 *
 * Boot order follows main_xbox360.c (libXenon), which follows main_xbox.c and
 * main_pc.c. The ordering is not arbitrary: the log opens before the config
 * parse so the [CFG] trace is captured, and Fs_InitFileTableForRegion must
 * run before anything reads a file or every startSector is 0.
 *
 * Everything up to MainLoop() is instrumented and FLUSHED, because MainLoop
 * does not return and a hang inside it must still leave the boot trace.
 *
 * Crashes: the whole run sits under a structured exception handler that
 * writes the exception code, address and registers to the log before
 * returning to the dashboard. That replaces the libXenon console tee.
 */
#include <xtl.h>
#include <stdio.h>

extern "C" {
#include "sh_log.h"
#include "fs_xbox360.h"
#include "psx_memory.h"
#include "pc_config.h"

extern const char* g_ShLogPath;
extern void        SH_DebugLogFlush(void);

extern char g_CdBinPath[];
extern void Cd_XboxInit(void);
extern int  Cd_XboxSelfTest(char* idOut, int idOutSize);

extern void PcConfig_Load(const char* path);
extern void XboxConfig_ApplyOverrides(void);
extern void Gte_SelfTest(void);
extern void GpuNv2a_Init(void);
extern void GpuNv2a_FrameBegin(void);
extern void Gpu360_SelfTest(void);
extern void Pad_XboxInit(void);
extern void Mcard_XboxInit(void);
extern void Fs_InitFileTableForRegion(int region);
extern void SpuInit(void);
extern void Audio360_Init(void);
extern void ResetGraph(int mode);
extern void SetGraphDebug(int level);
extern void Fs_QueueInitialize(void);
extern void MainLoop(void);

extern void* g_OvlDynamic;
extern void* g_OvlBodyprog;
typedef struct s_DemoFrameData s_DemoFrameData;
extern s_DemoFrameData* g_Demo_PlayFileBufferPtr;

extern void PcPort_InitCharaAnimInfo(void);
extern void PcPort_InitSdBuffers(void);
extern void AsRodata_Reformat(void);
extern void GroanerAnimInfos_Init(void);
extern void BloodsuckerAnimInfos_Init(void);
extern void BloodyLisaAnimInfos_Init(void);
extern void AlessaAnimInfos_Init(void);
extern void GhostChildAlessaAnimInfos_Init(void);
extern void LisaAnimInfos_Init(void);
extern void KaufmannAnimInfos_Init(void);
extern void DahliaAnimInfos_Init(void);
extern void CatAnimInfos_Init(void);
extern void PuppetNurseData_Init(void);
extern void LarvalStalkerAnimInfos_Init(void);
extern void HangedScratcherAnimInfos_Init(void);
extern void CreeperAnimInfos_Init(void);
extern void SplitHeadAnimInfos_Init(void);
extern void RomperAnimInfos_Init(void);
extern void LockerDeadBodyAnimInfos_Init(void);
extern void TwinfeelerAnimInfos_Init(void);
extern void FloatstingerAnimInfos_Init(void);
extern void MonsterCybilAnimInfos_Init(void);
extern void FlaurosAnimInfos_Init(void);
extern void ParasiteAnimInfos_Init(void);
extern void GhostDoctorAnimInfos_Init(void);
extern void BloodyIncubatorAnimInfos_Init(void);
extern void IncubatorAnimInfos_Init(void);
extern void LittleIncubusAnimInfos_Init(void);
extern void IncubusAnimInfos_Init(void);
extern void Unkkown23AnimInfos_Init(void);
extern void Map6S04ExtraAnimInfos_Init(void);
extern int  sh_c89_init_count;
extern void Sh_RunC89Inits(void);
}

/* Runtime data builders. main_pc.c and main_xbox.c call these before MainLoop
 * because the toolchains reject function pointers in static initializers, so
 * the tables are assembled at runtime instead. */
static void Sh_InitGameData(void)
{
    PsxMemory_Init();
    SH_DBG("[BOOT] PSX RAM @ %p", (void*)g_PsxRam);

    PcPort_InitCharaAnimInfo();
    PcPort_InitSdBuffers();
    AsRodata_Reformat();

    GroanerAnimInfos_Init();
    BloodsuckerAnimInfos_Init();
    BloodyLisaAnimInfos_Init();
    AlessaAnimInfos_Init();
    GhostChildAlessaAnimInfos_Init();
    LisaAnimInfos_Init();
    KaufmannAnimInfos_Init();
    DahliaAnimInfos_Init();
    CatAnimInfos_Init();
    PuppetNurseData_Init();
    LarvalStalkerAnimInfos_Init();
    HangedScratcherAnimInfos_Init();
    CreeperAnimInfos_Init();
    SplitHeadAnimInfos_Init();
    RomperAnimInfos_Init();
    LockerDeadBodyAnimInfos_Init();
    TwinfeelerAnimInfos_Init();
    FloatstingerAnimInfos_Init();
    MonsterCybilAnimInfos_Init();
    FlaurosAnimInfos_Init();
    ParasiteAnimInfos_Init();
    GhostDoctorAnimInfos_Init();
    BloodyIncubatorAnimInfos_Init();
    IncubatorAnimInfos_Init();
    LittleIncubusAnimInfos_Init();
    IncubusAnimInfos_Init();
    Unkkown23AnimInfos_Init();
    Map6S04ExtraAnimInfos_Init();

    /* Overlay base pointers into emulated PSX RAM (USA addresses, per main_pc.c). */
    g_OvlDynamic             = PSX_ADDR(0x000C9578);
    g_OvlBodyprog            = PSX_ADDR(0x00024B60);
    g_Demo_PlayFileBufferPtr = (s_DemoFrameData*)PSX_ADDR(0x000F5E00);

    SH_DBG("[BOOT] game data initialised");
}

extern "C" void Xbox_QuitToDashboard(void)
{
    SH_DBG("[SYS] quit requested - returning to dashboard");
    SH_DebugLogFlush();
    XLaunchNewImage(XLAUNCH_KEYWORD_DEFAULT_APP, 0);
}

/* The exception report. Written line by line with a flush after each, since
 * the next line may be the one that cannot be written. */
static LONG CrashFilter(EXCEPTION_POINTERS* ep)
{
    EXCEPTION_RECORD* er = ep->ExceptionRecord;
    CONTEXT*          cx = ep->ContextRecord;
    int i;

    SH_DebugLogFlush();
    if (!g_ShDebugLog)
        return EXCEPTION_EXECUTE_HANDLER;
    fprintf(g_ShDebugLog, "\n======== CRASH ========\n");
    fprintf(g_ShDebugLog, "[CRASH] code=0x%08x address=%p\n",
            (unsigned)er->ExceptionCode, er->ExceptionAddress);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        fprintf(g_ShDebugLog, "[CRASH] access violation: %s at 0x%08x\n",
                er->ExceptionInformation[0] ? "write" : "read",
                (unsigned)er->ExceptionInformation[1]);
    SH_DebugLogFlush();
    fprintf(g_ShDebugLog, "[CRASH] iar=%08x lr=%08x ctr=%08x msr=%08x\n",
            (unsigned)cx->Iar, (unsigned)cx->Lr, (unsigned)cx->Ctr, (unsigned)cx->Msr);
    {   /* Gpr0..Gpr31 are consecutive ULONGLONG fields. */
        const ULONGLONG* g = &cx->Gpr0;
        for (i = 0; i < 32; i += 4)
            fprintf(g_ShDebugLog, "[CRASH] r%02d=%08x r%02d=%08x r%02d=%08x r%02d=%08x\n",
                    i, (unsigned)g[i], i + 1, (unsigned)g[i + 1],
                    i + 2, (unsigned)g[i + 2], i + 3, (unsigned)g[i + 3]);
    }
    fprintf(g_ShDebugLog, "[CRASH] resolve iar/lr against the .map (link base 0x82000000)\n");
    SH_DebugLogFlush();
    return EXCEPTION_EXECUTE_HANDLER;
}

static void Boot(void)
{
    char cfgPath[SH360_PATH_MAX * 2];
    char pvd[8];
    int  haveBin;

    /* Before anything reads a data table: the stores C89 could not spell. */
    Sh_RunC89Inits();

    haveBin = Sh360Fs_Init();
    SH_DebugLogInit();

    SH_DBG("[BOOT] Silent Hill 360 (XDK build), tick=%u", (unsigned)GetTickCount());
    SH_DBG("[BOOT] root='%s' bin='%s' found=%d log='%s'",
           Sh360Fs_DataRoot(), Sh360Fs_BinName(), haveBin, g_ShLogPath);
    {   /* Transpiler-generated startup stores (union members C89 cannot
         * initialise). Zero here means every such table is missing data. */
        SH_DBG("[BOOT] c89 startup initialisers run: %d", sh_c89_init_count);
    }
    SH_DebugLogFlush();

    /* No launcher on this platform, so the cfg beside the disc image is the
     * only way to tune anything; absent, the PC defaults apply. */
    _snprintf(cfgPath, sizeof(cfgPath), "%ssilenthill.cfg", Sh360Fs_DataRoot());
    cfgPath[sizeof(cfgPath) - 1] = '\0';
    SH_DBG("[CFG] config file: %s", cfgPath);
    PcConfig_Load(cfgPath);

    /* MUST run after PcConfig_Load: pins the settings whose PC defaults are
     * sized for a desktop (texture caches, chara pool, preload) to console
     * values. */
    XboxConfig_ApplyOverrides();

    Gte_SelfTest();

    Cd_XboxInit();
    SH_DBG("[CD] BIN: %s", g_CdBinPath);
    if (Cd_XboxSelfTest(pvd, sizeof(pvd)))
        SH_DBG("[CD] PVD id='%s' (expect CD001)", pvd);
    else
        SH_DBG("[CD] BIN NOT OPEN - the game cannot load anything");

    GpuNv2a_Init();
    Pad_XboxInit();
    Mcard_XboxInit();

    /* Under SH_PC_PORT g_FileTable ships EMPTY and must be filled at runtime.
     * Without this every file's startSector is 0. */
    Fs_InitFileTableForRegion(0 /* Region_USA */);

    Sh_InitGameData();
    SpuInit();
    Audio360_Init();
    ResetGraph(0);
    SetGraphDebug(0);
    Fs_QueueInitialize();

    SH_DBG("[BOOT] subsystems up; entering MainLoop");
    SH_DebugLogFlush();

    /* Draw a known image before the game gets a chance to hand the renderer
     * anything: proves the whole D3D path independently of game data. */
    Gpu360_SelfTest();

    GpuNv2a_FrameBegin();
    MainLoop();

    SH_DBG("[BOOT] MainLoop returned (unexpected)");
    SH_DebugLogFlush();
}

void __cdecl main()
{
    __try {
        Boot();
    } __except (CrashFilter(GetExceptionInformation())) {
        /* Report written; nothing to resume. */
    }
    Xbox_QuitToDashboard();
}
