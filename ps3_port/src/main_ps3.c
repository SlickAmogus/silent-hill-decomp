/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * main_ps3.c - PS3 entry point (mirror of xbox_port/src/main_xbox.c).
 *
 * Boot order follows main_xbox.c, which follows main_pc.c. The ordering is not
 * arbitrary: the log opens before the config parse so the [CFG] trace is
 * captured, and Fs_InitFileTableForRegion must run before anything reads a file
 * or every startSector is 0 and every CD read returns the volume descriptor.
 *
 * Everything up to MainLoop() is instrumented and FLUSHED, because MainLoop does
 * not return and a hang inside it must still leave the boot trace on disk.
 *
 * This TU includes NO decomp game headers -- every entry point below is declared
 * locally -- which is what lets it also include PSL1GHT. See ps3_hal.h for why
 * the two cannot meet.
 */
#include <stdio.h>

#include <sysmodule/sysmodule.h>

#include "sh_log.h"
#include "fs_ps3.h"
#include "ps3_hal.h"

/* sh_log_ps3.c (port-specific, so not in the shared sh_log.h). */
extern const char* g_ShLogPath;
extern void        SH_DebugLogFlush(void);

/* cd_ps3.c */
extern char g_CdBinPath[];
extern void Cd_XboxInit(void);
extern int  Cd_XboxSelfTest(char* idOut, int idOutSize);

/* Declared locally rather than by including the game headers, matching
 * main_xbox.c: this TU only needs the entry points, not the decomp's types. */
extern void PcConfig_Load(const char* path);
extern void Gte_SelfTest(void);
extern void GpuNv2a_Init(void);
extern void GpuNv2a_FrameBegin(void);
extern void Pad_XboxInit(void);
extern void Mcard_XboxInit(void);
extern void Fs_InitFileTableForRegion(int region);
extern void SpuInit(void);
extern void Audio360_Init(void);
extern void ResetGraph(int mode);
extern void SetGraphDebug(int level);
extern void Fs_QueueInitialize(void);
extern void MainLoop(void);

/* Runtime data builders. main_pc.c and main_xbox.c call these before MainLoop
 * because the toolchains reject function pointers in static initializers, so
 * the tables are assembled at runtime instead. Sh_InitGameData is `static` in
 * main_xbox.c, so it is replicated here rather than shared. */
#include "psx_memory.h"

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

static void Sh_InitGameData(void)
{
    /* PSX memory emulation first -- everything below is g_PsxRam-relative. */
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

int main(void)
{
    char cfgPath[SH_PS3_PATH_MAX * 2];
    char pvd[8];
    int  haveBin;

    /* printf goes to the TTY, which RPCS3 mirrors into log/TTY.log and ps3load
     * pipes back over the network. That is the only output that exists before
     * the log file opens, so the earliest failures are still visible. */
    printf("[SH-PS3] Silent Hill - PlayStation 3\n");

    /* The FS module is not loaded into a GameOS process by default, and every
     * path below (log, config, disc image) needs it. */
    sysModuleLoad(SYSMODULE_FS);

    haveBin = Sh3Fs_Init();
    SH_DebugLogInit();
    printf("[SH-PS3] log: %s\n", g_ShLogPath);

    SH_DBG("[BOOT] Silent Hill PS3, timebase=%llu Hz", Ps3_TimebaseFreq());
    SH_DBG("[BOOT] root='%s' bin='%s' found=%d",
           Sh3Fs_DataRoot(), Sh3Fs_BinName(), haveBin);
    SH_DebugLogFlush();

    /* No launcher on this platform, so the cfg beside the data is the only way
     * to tune anything; absent, the PC defaults apply. */
    snprintf(cfgPath, sizeof(cfgPath), "%ssilenthill.cfg", Sh3Fs_DataRoot());
    SH_DBG("[CFG] config file: %s", cfgPath);
    PcConfig_Load(cfgPath);

    /* Fixed-point/GTE sanity before anything depends on it. On a new
     * architecture this is the single most likely thing to be silently wrong,
     * and every transform in the game rides on it. */
    Gte_SelfTest();

    Cd_XboxInit();
    SH_DBG("[CD] BIN: %s", g_CdBinPath);
    if (Cd_XboxSelfTest(pvd, sizeof(pvd))) {
        SH_DBG("[CD] PVD id='%s' (expect CD001)", pvd);
        printf("[SH-PS3] bin ok, PVD %s\n", pvd);
    } else {
        SH_DBG("[CD] BIN NOT OPEN - the game cannot load anything");
        printf("[SH-PS3] BIN NOT OPEN\n");
    }

    GpuNv2a_Init();
    Pad_XboxInit();
    Mcard_XboxInit();

    /* Under SH_PC_PORT g_FileTable ships EMPTY and must be filled at runtime.
     * Without this every file's startSector is 0, so every read lands on the
     * ISO volume descriptor and returns garbage. */
    Fs_InitFileTableForRegion(0 /* Region_USA */);

    Sh_InitGameData();
    SpuInit();
    Audio360_Init();
    ResetGraph(0);
    SetGraphDebug(0);
    Fs_QueueInitialize();

    SH_DBG("[BOOT] subsystems up; entering MainLoop");
    /* Last chance to get the boot trace on disk: MainLoop never returns, and if
     * it hangs this is all the evidence there will be. */
    SH_DebugLogFlush();
    printf("[SH-PS3] entering MainLoop\n");

    GpuNv2a_FrameBegin();
    MainLoop();

    SH_DBG("[BOOT] MainLoop returned (unexpected)");
    SH_DebugLogFlush();
    return 0;
}
