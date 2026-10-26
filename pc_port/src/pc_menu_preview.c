/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "pc_menu_preview.h"
#include "pc_config.h"
#include "pc_options_help.h"
#include "control_style.h"
#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/player.h"
#include "bodyprog/item_screens.h"
#include "bodyprog/screen/screen_data.h"
#include "bodyprog/view/vw_system.h"
#include "bodyprog/screen/screen_draw.h"
#include <SDL.h>
#include <PsyX/common/glad.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__linux__) && !defined(__ANDROID__)
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <sys/syscall.h>
/* The PSY-Q compatibility layer defines open/close symbols for memory cards.
 * Use Linux syscalls here so a shared-memory fd cannot reach those stubs. */
#define open(path,flags,...) syscall(SYS_openat,AT_FDCWD,path,flags,##__VA_ARGS__)
#define close(fd) syscall(SYS_close,fd)
extern char** environ;
enum { PREVIEW_W=640, PREVIEW_H=360 };
typedef struct {
    int pgxp, filter, dither, post, tone, flashlight, culling, camera, collision, weather;
    float fog, intensity, size, fpsFov, tpsFov, otsFov;
} PreviewSettings;
typedef struct {
    int lock;
    unsigned serial, heartbeat;
    int paused, animated;
    PreviewSettings settings;
    unsigned char pixels[PREVIEW_W*PREVIEW_H*4];
} PreviewShared;
static PreviewShared* s_shared;
static pid_t s_child;
static int s_childMode, s_failed, s_frozen, s_scene, s_childScene;
static int s_dirtyFrames, s_animated;
static char s_directory[PATH_MAX];
static unsigned char s_pixels[PREVIEW_W*PREVIEW_H*4];
static unsigned s_serial, s_frameVersion;
static int s_camera;
static VW_VIEW_WORK s_schoolView;
static VC_WORK s_schoolCamera;
typedef struct {
    PreviewShared* shared;
    pid_t child;
    char directory[PATH_MAX];
    unsigned serial;
    int failed;
    unsigned char pixels[PREVIEW_W*PREVIEW_H*4];
} PreviewCache;
static PreviewCache s_cache[2];
static int s_worker;
static pid_t s_retired[16];

static int preview_reap(void)
{
    int i,count=0;
    for(i=0;i<16;i++) {
        if(s_retired[i]) {
            pid_t result=waitpid(s_retired[i],NULL,WNOHANG);
            if(result==s_retired[i] || (result<0 && errno==ECHILD))s_retired[i]=0;
        }
        if(s_retired[i])count++;
    }
    return count;
}

static int preview_alive(pid_t* child)
{
    int result;
    if(!*child)return 0;
    result=waitpid(*child,NULL,WNOHANG);
    if(result==*child || (result<0 && errno==ECHILD)){*child=0;return 0;}
    return 1;
}

static void preview_heartbeat(int active)
{
    int i;
    preview_reap();
    for(i=0;i<2;i++) {
        PreviewShared* p=i==s_worker?s_shared:s_cache[i].shared;
        if(!p)continue;
        __atomic_store_n(&p->heartbeat,(unsigned)time(NULL),__ATOMIC_RELEASE);
        /* A newly prefetched worker must finish its first frame even though
         * the current row has no image. Once ready it sleeps like any cache. */
        __atomic_store_n(&p->paused,!(active && i==s_worker) &&
            __atomic_load_n(&p->serial,__ATOMIC_ACQUIRE)!=0,__ATOMIC_RELEASE);
    }
}

void Pc_MenuPreview_Idle(void){if(!s_childMode)preview_heartbeat(0);}

void Pc_MenuPreview_Camera(int mode){s_camera=mode;}

void Pc_MenuPreview_Select(const char* key)
{
    int scene=Pc_OptionsPreviewScene(key);
    int worker=scene==PC_SCENE_SCHOOL?0:1;
    if(s_childMode)return;
    if(scene && worker!=s_worker) {
        PreviewCache* old=&s_cache[s_worker];
        old->shared=s_shared;old->child=s_child;old->serial=s_serial;old->failed=s_failed;
        memcpy(old->directory,s_directory,sizeof(s_directory));
        memcpy(old->pixels,s_pixels,sizeof(s_pixels));
        old=&s_cache[worker];
        s_shared=old->shared;s_child=old->child;s_serial=old->serial;s_failed=old->failed;
        memcpy(s_directory,old->directory,sizeof(s_directory));
        memcpy(s_pixels,old->pixels,sizeof(s_pixels));
        memset(old,0,sizeof(*old));s_worker=worker;
        if(s_serial && !++s_frameVersion)++s_frameVersion;
    }
    s_scene=scene;
    preview_heartbeat(scene!=PC_SCENE_NONE);
}

static PreviewSettings preview_settings(void)
{
    PreviewSettings s={g_PcConfig.usePgxp,g_PcConfig.textureFilter,g_PcConfig.dithering,
        g_PcConfig.postProcess,g_PcConfig.tonemap,g_PcConfig.flashlightMode,
        g_PcConfig.disableCulling,s_camera,g_PcConfig.tpsCameraCollision,g_PcConfig.weatherSimHz,g_PcConfig.fogStrength,
        g_PcConfig.flashlightIntensity,g_PcConfig.flashlightSize,
        g_PcConfig.fpsFov,g_PcConfig.tpsFov,g_PcConfig.otsFov};
    return s;
}

int Pc_MenuPreview_IsChild(void)
{
    return s_childMode || getenv("SH_OPTIONS_PREVIEW")!=NULL;
}

int Pc_MenuPreview_Available(void)
{
    const char* disabled=getenv("SH_MENU_PREVIEW_DISABLE");
    return !s_failed && !(disabled && strcmp(disabled,"0"));
}

void Pc_MenuPreview_Configure(void)
{
    const char* path=getenv("SH_OPTIONS_PREVIEW");
    int fd;
    if (!path) return;
    s_childMode=1;
    s_childScene=getenv("SH_OPTIONS_PREVIEW_SCENE")?atoi(getenv("SH_OPTIONS_PREVIEW_SCENE")):PC_SCENE_SCHOOL;
    if(s_childScene<PC_SCENE_SCHOOL || s_childScene>PC_SCENE_WEATHER)exit(1);
    fd=open(path,O_RDWR|O_NOFOLLOW);
    if (fd<0) exit(1);
    { struct stat st; if (fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size!=sizeof(PreviewShared) ||
        st.st_uid!=getuid()) { close(fd);exit(1); } }
    s_shared=mmap(NULL,sizeof(*s_shared),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    close(fd);
    if (s_shared==MAP_FAILED) exit(1);
    /* Never inherit a player's account, plugins, randomizer, input or saves. */
    g_PcConfig.retroAchievements=0;g_PcConfig.raUsername[0]=g_PcConfig.raToken[0]=0;
    g_PcConfig.discordRichPresence=0;g_PcConfig.enablePlugins=0;
    g_PcConfig.randomizer=0;g_PcConfig.darkHarry=0;
    g_PcConfig.windowWidth=PREVIEW_W;g_PcConfig.windowHeight=PREVIEW_H;
    g_PcConfig.fullscreen=0;g_PcConfig.confineCursor=0;
    g_PcConfig.skipIntros=2;g_PcConfig.attractDemos=0;g_PcConfig.allowDebugControls=0;
    /* Hidden boot need not spend seconds on retail fades at 30 FPS. */
    g_PcConfig.fpsCap=240;g_PcConfig.vsync=0;g_PcConfig.msaaSamples=0;
    {extern s32 g_PcTxnHoldFrames;g_PcTxnHoldFrames=0;}
    g_PcConfig.nativeResolution=0;
    strcpy(g_PcConfig.mapName,s_childScene==PC_SCENE_SCHOOL?"map1_s00":"map2_s00");
    SDL_setenv("SDL_AUDIODRIVER","dummy",1);
    SDL_setenv("ALSOFT_DRIVERS","null",1);
    /* No physical controller belongs to the showcase, regardless of vendor. */
    SDL_setenv("SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT","0x0000/0x0000",1);
}

static void preview_start(void)
{
    char path[PATH_MAX+64],exe[PATH_MAX],cwd[PATH_MAX],envPath[PATH_MAX+32],envScene[64];
    char** env;
    int fd,i,n=0;
    ssize_t len;
    posix_spawn_file_actions_t actions;
    if (s_childMode || s_child || !Pc_MenuPreview_Available() || !s_scene) return;
    /* Reserve room for both live workers to retire without a blocking wait.
     * If a driver leaves many killed children stuck, do not spawn endlessly. */
    if(preview_reap()>12)return;
    s_failed=1;
    if (!getcwd(cwd,sizeof(cwd))) return;
    snprintf(s_directory,sizeof(s_directory),"%s/sh-menu-preview-XXXXXX",
        getenv("TMPDIR")?getenv("TMPDIR"):
        (getenv("XDG_RUNTIME_DIR")?getenv("XDG_RUNTIME_DIR"):"/tmp"));
    if (!mkdtemp(s_directory)) {s_directory[0]=0;return;}
    {static int registered;
     if(!registered){atexit(Pc_MenuPreview_Stop);registered=1;}}
    snprintf(path,sizeof(path),"%s/shared",s_directory);
    fd=open(path,O_RDWR|O_CREAT|O_EXCL,0600);
    if (fd<0) return;
    if (ftruncate(fd,sizeof(PreviewShared))) {close(fd);return;}
    s_shared=mmap(NULL,sizeof(*s_shared),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    close(fd);
    if (s_shared==MAP_FAILED) {s_shared=NULL;return;}
    memset(s_shared,0,sizeof(*s_shared));
    s_shared->animated=s_scene==PC_SCENE_WEATHER;
    s_shared->settings=preview_settings();s_shared->heartbeat=(unsigned)time(NULL);
    /* Only read-only runtime assets are linked. There is deliberately NO link
     * to config.cfg, memory cards, plugins, achievements or account files. */
    { const char* assets[]={"maps","gamedata"};
      for(i=0;i<2;i++) {
        char source[PATH_MAX+16];
        snprintf(source,sizeof(source),"%s/%s",cwd,assets[i]);
        snprintf(path,sizeof(path),"%s/%s",s_directory,assets[i]);
        if (i==0) {if(symlink(source,path))return;}
        else {
            /* Mount the image explicitly; give the child its own empty save
             * directory instead of sharing gamedata/save through a symlink. */
            if(mkdir(path,0700))return;
            snprintf(path,sizeof(path),"%s/gamedata/save",s_directory);if(mkdir(path,0700))return;
        }
      }
    }
    snprintf(path,sizeof(path),"%s/config.cfg",s_directory);
    { extern const char* PcPort_GetGameDiscPath(void);
      const char* disc=PcPort_GetGameDiscPath();
      FILE* f=fopen(path,"w");
      char absolute[PATH_MAX];
      if (!f || !disc || !realpath(disc,absolute)) {if(f)fclose(f);return;}
      snprintf(path,sizeof(path),"%s/gamedata/preview.bin",s_directory);
      if(symlink(absolute,path)){fclose(f);return;}
      fprintf(f,"disc_image = preview.bin\nskip_intros = 2\nmap = map1_s00\npreload_chunks = 1\nenable_debug_log = 0\n");
      fclose(f);
    }
    len=readlink("/proc/self/exe",exe,sizeof(exe)-1);
    if(len<=0)return;
    exe[len]=0;
    while(environ[n])n++;
    env=calloc((size_t)n+3,sizeof(char*));if(!env)return;
    for(i=0;i<n;i++)env[i]=environ[i];
    snprintf(envPath,sizeof(envPath),"SH_OPTIONS_PREVIEW=%s/shared",s_directory);env[n]=envPath;
    snprintf(envScene,sizeof(envScene),"SH_OPTIONS_PREVIEW_SCENE=%d",s_scene);env[n+1]=envScene;
    if(posix_spawn_file_actions_init(&actions)){free(env);return;}
    snprintf(path,sizeof(path),"%s/renderer.log",s_directory);
    if(posix_spawn_file_actions_addchdir_np(&actions,s_directory) ||
       posix_spawn_file_actions_addopen(&actions,1,path,O_WRONLY|O_CREAT|O_TRUNC,0600) ||
       posix_spawn_file_actions_adddup2(&actions,1,2) ||
       posix_spawn_file_actions_addclosefrom_np(&actions,3)) {
        posix_spawn_file_actions_destroy(&actions);free(env);return;
    }
    { char* args[]={exe,NULL};
      if(posix_spawn(&s_child,exe,&actions,NULL,args,env))s_child=0;
    }
    posix_spawn_file_actions_destroy(&actions);free(env);
    if(s_child)s_failed=0;
}

void Pc_MenuPreview_Preload(void)
{
    if(s_childMode)return;
    s_camera=ControlStyle_Classic;
    Pc_MenuPreview_Select("use_pgxp");
    preview_start();
    Pc_MenuPreview_Idle();
}

static void preview_stop_current(void)
{
    if(preview_alive(&s_child)) {
        int i;
        /* Disposable worker: never let a driver stall block the menu thread. */
        kill(s_child,SIGKILL);
        if(waitpid(s_child,NULL,WNOHANG)==0)
            for(i=0;i<16;i++)if(!s_retired[i]){s_retired[i]=s_child;break;}
        s_child=0;
    }
    if(s_shared) {munmap(s_shared,sizeof(*s_shared));s_shared=NULL;}
    if(s_directory[0]) {
        /* Only our exact, private files: never follow/remove linked assets. */
        const char* files[]={"shared","config.cfg","SilentHill.log","maps","renderer.log"};int i;
        char p[PATH_MAX+64];
        for(i=0;i<5;i++){snprintf(p,sizeof(p),"%s/%s",s_directory,files[i]);unlink(p);}
        snprintf(p,sizeof(p),"%s/gamedata/save/0.MCD",s_directory);unlink(p);
        snprintf(p,sizeof(p),"%s/gamedata/save/8.MCD",s_directory);unlink(p);
        snprintf(p,sizeof(p),"%s/gamedata/save",s_directory);rmdir(p);
        snprintf(p,sizeof(p),"%s/gamedata/preview.bin",s_directory);unlink(p);
        snprintf(p,sizeof(p),"%s/gamedata",s_directory);rmdir(p);
        rmdir(s_directory);s_directory[0]=0;
    }
    s_serial=0;s_failed=0;
}

void Pc_MenuPreview_Stop(void)
{
    int i;
    if(s_childMode)return;
    preview_stop_current();
    for(i=0;i<2;i++)if(s_cache[i].shared || s_cache[i].directory[0]) {
        s_shared=s_cache[i].shared;s_child=s_cache[i].child;
        memcpy(s_directory,s_cache[i].directory,sizeof(s_directory));
        memset(&s_cache[i],0,sizeof(s_cache[i]));preview_stop_current();
    }
    preview_heartbeat(0);
}

const unsigned char* Pc_MenuPreview_Frame(unsigned* serial,int* width,int* height)
{
    if(s_childMode)return NULL;
    preview_start();
    preview_heartbeat(1);
    if(!s_child || !s_shared)return NULL;
    if(!preview_alive(&s_child)){s_failed=1;return NULL;}
    if(!__atomic_exchange_n(&s_shared->lock,1,__ATOMIC_ACQUIRE)) {
        s_shared->settings=preview_settings();
        s_shared->animated=s_scene==PC_SCENE_WEATHER;
        if(s_shared->serial && s_serial!=s_shared->serial) {
            memcpy(s_pixels,s_shared->pixels,sizeof(s_pixels));s_serial=s_shared->serial;
            if(!++s_frameVersion)++s_frameVersion;
        }
        __atomic_store_n(&s_shared->lock,0,__ATOMIC_RELEASE);
    }
    *serial=s_frameVersion;*width=PREVIEW_W;*height=PREVIEW_H;
    return s_serial?s_pixels:NULL;
}

void Pc_MenuPreview_Tick(void)
{
    static int warmup;
    static PreviewSettings applied;
    PreviewSettings s;
    extern int g_PcQuickOptionsActive,g_PsxUsePgxp,g_cfg_textureFilter,g_cfg_bilinearFiltering;
    extern int g_cfg_psxDither,g_cfg_anisoLevel,g_cfg_postProcess,g_cfg_tonemap;
    extern float g_PsyX_FogStrength,g_PsyX_FlashlightIntensity,g_PsyX_FlashlightSize;
    if(!s_childMode){preview_reap();return;}
    if(!s_shared)return;
    /* The hidden showcase has no visible transitions to fade. Keep the stock
     * boot/load initialisation, but complete each retail fade in one tick. */
    g_ScreenFadeTimestep=Q12(1000.0f);
    /* A static preset is a cached real frame. Wait BEFORE world submission,
     * rather than repeatedly drawing the same room and reading its pixels.
     * Only weather needs animation; all other scenes wake on parameter changes. */
    for(;;) {
        if((unsigned)time(NULL)-__atomic_load_n(&s_shared->heartbeat,__ATOMIC_ACQUIRE)>60)exit(0);
        if(__atomic_load_n(&s_shared->paused,__ATOMIC_ACQUIRE)) {SDL_Delay(16);continue;}
        if(__atomic_exchange_n(&s_shared->lock,1,__ATOMIC_ACQUIRE)) {SDL_Delay(8);continue;}
        s=s_shared->settings;
        if(s_animated!=s_shared->animated) {s_animated=s_shared->animated;s_dirtyFrames=4;}
        __atomic_store_n(&s_shared->lock,0,__ATOMIC_RELEASE);
        if(memcmp(&s,&applied,sizeof(s))) {applied=s;s_dirtyFrames=4;}
        if(!s_frozen || s_dirtyFrames || s_animated)break;
        SDL_Delay(16);
    }
    g_PcConfig.usePgxp=g_PsxUsePgxp=s.pgxp;
    g_PcConfig.textureFilter=s.filter;g_cfg_textureFilter=s.filter>=3?3:s.filter;
    g_cfg_bilinearFiltering=s.filter>0;g_cfg_anisoLevel=s.filter>=3?2<<(s.filter-3):1;
    g_cfg_psxDither=s.dither;g_PcConfig.postProcess=g_cfg_postProcess=s.post;
    g_PcConfig.tonemap=g_cfg_tonemap=s.tone;
    Pc_FlashlightModeApply(s.flashlight,0);
    g_PcConfig.fogStrength=g_PsyX_FogStrength=s.fog;
    g_PcConfig.flashlightIntensity=g_PsyX_FlashlightIntensity=s.intensity;
    g_PcConfig.flashlightSize=g_PsyX_FlashlightSize=s.size;
    g_PcConfig.disableCulling=s.culling;
    g_PcConfig.fpsFov=s.fpsFov;g_PcConfig.tpsFov=s.tpsFov;g_PcConfig.otsFov=s.otsFov;
    g_PcConfig.tpsCameraCollision=s.collision;
    g_PcConfig.weatherSimHz=s.weather;
    if(s_frozen && g_ControlStyle!=s.camera)Pc_ControlStyleSet(s.camera);
    if(s_frozen && !s.camera) {
        /* Leaving an FOV example must restore the authored preset shot, not
         * leave the previous TPS eye in the classic scene. */
        vwViewPointInfo=s_schoolView;
        vcWork=s_schoolCamera;
        vwSetViewInfo();
    }
    if(g_GameWork.gameState==GameState_InGame && s_childScene!=PC_SCENE_SCHOOL &&
       g_SysWork.sysState==SysState_EventCallback) {
        /* A fresh street start normally runs the cafe-exit story event. The
         * disposable preset must never wait for that cinematic to complete. */
        g_SysWork.sysState=SysState_Gameplay;
        g_SysWork.sysFlags&=~SysFlag_CutsceneActive;
    }
    if(g_GameWork.gameState==GameState_InGame && g_SysWork.sysState==SysState_Gameplay) {
        if(++warmup==4) {
            /* This inventory belongs ONLY to the disposable showcase process. */
            extern void Pc_ConsoleExec(const char*);
            extern void Game_TurnFlashlightOn(void);
            Pc_ConsoleExec("GIVE PIPE");
            g_SavegamePtr->equippedWeapon=InvItemId_SteelPipe;
            g_Inventory_EquippedItem=InvItemId_SteelPipe;
            g_SysWork.playerCombat.weaponAttack=EquippedWeaponId_SteelPipe;
            GameFs_WeaponInfoUpdate();
            Gfx_PlayerHeldItemAttach(g_SysWork.playerCombat.weaponAttack);
            WorldGfx_PlayerPrevHeldItem(&g_SysWork.playerCombat);func_8003D01C();
            Game_TurnFlashlightOn();
        }
        if(warmup==40){s_schoolView=vwViewPointInfo;s_schoolCamera=vcWork;}
        if(warmup>=40) {if(!s_frozen)s_dirtyFrames=4;s_frozen=1;g_PcQuickOptionsActive=1;
            g_PcConfig.fpsCap=s_animated?60:120;}
        g_SysWork.sysFlags|=SysFlag_NoEnemySpawn;
    }
}

void Pc_MenuPreview_WeatherDraw(void)
{
    static Uint32 previous;
    if(s_childMode && s_animated && g_MapOverlayHdr.particlesUpdate) {
        s32 dt=g_DeltaTime,raw=g_DeltaTimeRaw;
        Uint32 now=SDL_GetTicks(),elapsed=previous?now-previous:16;
        previous=now;
        /* Only the isolated weather simulation advances, not Harry/events/AI.
         * This is the game's actual 30-Hz vs per-render-frame particle path. */
        /* The menu freezes the game clock; weather has its own. */
        if(elapsed>67)elapsed=67;
        g_DeltaTime=g_DeltaTimeRaw=(s32)(elapsed*4096/1000);
        g_MapOverlayHdr.particlesUpdate(0,g_SavegamePtr->mapIdx,1);
        g_DeltaTime=dt;g_DeltaTimeRaw=raw;
    }
}

void Pc_MenuPreview_Capture(void)
{
    GLint align;
    static unsigned char pixels[PREVIEW_W*PREVIEW_H*4];
    if(!s_childMode || !s_shared || !s_frozen)return;
    /* GPU readback can stall during a shader/PGXP transition. Never hold the
     * IPC lock across a driver call: the UI must keep exchanging heartbeats. */
    glGetIntegerv(GL_PACK_ALIGNMENT,&align);glPixelStorei(GL_PACK_ALIGNMENT,1);
    glReadPixels(0,0,PREVIEW_W,PREVIEW_H,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
    glPixelStorei(GL_PACK_ALIGNMENT,align);
    if(__atomic_exchange_n(&s_shared->lock,1,__ATOMIC_ACQUIRE))return;
    memcpy(s_shared->pixels,pixels,sizeof(pixels));
    __atomic_add_fetch(&s_shared->serial,1,__ATOMIC_RELEASE);
    if(s_dirtyFrames>0)s_dirtyFrames--;
    __atomic_store_n(&s_shared->lock,0,__ATOMIC_RELEASE);
}
#else
int Pc_MenuPreview_IsChild(void){return 0;}
int Pc_MenuPreview_Available(void){return 0;}
void Pc_MenuPreview_Configure(void){}
void Pc_MenuPreview_Tick(void){}
void Pc_MenuPreview_Capture(void){}
void Pc_MenuPreview_Stop(void){}
void Pc_MenuPreview_Camera(int mode){}
void Pc_MenuPreview_Select(const char* key){}
void Pc_MenuPreview_Preload(void){}
void Pc_MenuPreview_Idle(void){}
void Pc_MenuPreview_WeatherDraw(void){}
const unsigned char* Pc_MenuPreview_Frame(unsigned* s,int* w,int* h){return NULL;}
#endif
