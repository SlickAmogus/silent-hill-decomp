/*
 * main_xbox.c - Original Xbox (NXDK / NV2A) entry point for the Silent Hill 1 port.
 *
 * Mirror of pc_port/src/main_pc.c: brings up the Xbox HAL (video / pbkit / NV2A /
 * D: log), initializes the PSX-RAM-emulated runtime data the way the PC port does
 * (PsxMemory_Init + the anim-info builders + overlay pointers), then hands control
 * to the shared game MainLoop(). Frame presentation is driven from VSync()
 * (psx_libgpu_xbox.c) onto GpuNv2a_FrameBegin/FrameEnd.
 *
 * We do NOT include game.h here: it pulls the decomp `byte` typedef which clashes
 * with <windows.h>. Game entry points are declared extern instead (as main_pc.c
 * also does for the data builders).
 */
#include <hal/debug.h>
#include <hal/video.h>
#include <pbkit/pbkit.h>
#include <windows.h>

#include "gpu_nv2a.h"
#include "sh_log.h"
#include "psx_memory.h"   /* PSX_ADDR, PsxMemory_Init (includes only <stdint.h>) */

extern void Crash_InstallSehFrame(void* stackFrame); /* crash_xbox.c */
extern void XboxFs_MountHomeDrive(void);
extern void Gte_SelfTest(void);
extern void Pad_XboxInit(void);   /* USB controller init (pad_xbox.c) */
extern void Cd_XboxInit(void);    /* open the BIN disc image on D: (cd_xbox.c) */
extern void Fs_InitFileTableForRegion(int region);  /* fill g_FileTable (USA=0) */

/* Game entry + PSX subsystem init (defined in the shared decomp / pc_port data). */
extern void MainLoop(void);
extern void Fs_QueueInitialize(void);
extern void ResetGraph(int mode);
extern void SetGraphDebug(int level);
extern void SpuInit(void);
extern void Audio_XboxInit(void);   /* RXDK DirectSound HAL (dsound_xbox.c) */
extern void PcPort_InitCharaAnimInfo(void);
extern void PcPort_InitSdBuffers(void);

/* Overlay base pointers — on PSX these are fixed RAM addresses; here they point
 * into the emulated PSX RAM (g_PsxRam) via PSX_ADDR, matching main_pc.c (USA). */
extern void* g_OvlDynamic;
extern void* g_OvlBodyprog;
typedef struct s_DemoFrameData s_DemoFrameData;
extern s_DemoFrameData* g_Demo_PlayFileBufferPtr;

/* Runtime data builders (zero-stub anim infos + rodata reformat). main_pc.c calls
 * these before MainLoop because MinGW/clang reject function pointers in static
 * initializers; the data is assembled at runtime instead. */
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
    /* PSX memory emulation first — everything below is g_PsxRam-relative. */
    PsxMemory_Init();
    SH_DBG("[SH-XBOX] PSX RAM @ %p", (void*)g_PsxRam);

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

    SH_DBG("[SH-XBOX] game data initialised");
}

int main(void)
{
    /* Unhandled-exception logger frame. MUST be a main() local: the kernel's
     * SEH dispatcher validates registration records against the thread's
     * stack bounds, and main()'s frame lives for the program's life (MainLoop
     * never returns). {prev, handler} — filled in by Crash_InstallSehFrame. */
    struct { void* prev; void* handler; } sehFrame = { 0, 0 };

    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);

    XboxFs_MountHomeDrive();
    SH_DebugLogInit();
    Crash_InstallSehFrame(&sehFrame); /* from here on, any fault logs [FATAL] + flushes */
    debugPrint("Silent Hill (Xbox) booting...\n");
    SH_DBG("[SH-XBOX] boot: video 640x480x32");

    Gte_SelfTest();

    int status = pb_init();
    if (status) {
        debugPrint("pb_init failed: %d\n", status);
        SH_DBG("[SH-XBOX] pb_init FAILED (%d)", status);
        Sleep(5000);
        return 1;
    }
    pb_show_front_screen();
    SH_DBG("[SH-XBOX] pbkit initialised");

    GpuNv2a_Init();

    /* Bring up the USB controller (safe no-op if none is connected). */
    Pad_XboxInit();
    SH_DBG("[SH-XBOX] USB pad init done");

    /* Open the BIN disc image on D: for real asset loading (libcd reads). */
    Cd_XboxInit();

    /* Populate g_FileTable with the USA disc layout. Under SH_PC_PORT this table
     * ships empty and must be filled at runtime (main_pc.c does the same). Without
     * it every file's startSector is 0, so every CD read hits sector 0 (the ISO
     * volume descriptor) and returns garbage. */
    Fs_InitFileTableForRegion(0 /* Region_USA */);

    /* PSX subsystem init (mirrors main_pc.c order). */
    Sh_InitGameData();
    SpuInit();
    Audio_XboxInit();               /* bring up RXDK DirectSound + start the APU ring */
    ResetGraph(0);
    SetGraphDebug(0);
    Fs_QueueInitialize();
    SH_DBG("[SH-XBOX] subsystems up; entering MainLoop");

    /* Open the first NV2A frame; VSync() presents + opens the next each frame. */
    GpuNv2a_FrameBegin();

    /* Hands off to the shared game code. On PSX this is in BODYPROG; on PC/Xbox
     * everything is statically linked so we call it directly. Does not return. */
    MainLoop();

    SH_DBG("[SH-XBOX] MainLoop returned (unexpected); shutting down");
    pb_kill();
    return 0;
}
