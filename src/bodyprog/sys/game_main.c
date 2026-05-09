#include "game.h"

#ifdef SH_PC_PORT
#include "sh_log.h"
#include "pc_config.h"
#include "xa_player.h"
#include <SDL_timer.h>
extern void PsyX_EndScene(void);
extern void PsyX_UpdateInput(void);
extern float g_PsyX_FogColor[3];
extern int g_PcHorPlusEnabled;
extern int g_PsxSkipFramebufferStore;
#include <stdio.h>
#include <SDL_scancode.h>
#include <SDL_mouse.h>
extern u8 g_WorldEnvWork[];
#define PC_WorldEnvWork (*(s_WorldEnvWork*)g_WorldEnvWork)
#include "bodyprog/view/vw_main.h"
#include "bodyprog/view/structs.h"
extern VC_WORK vcWork;
extern void vcGetNowCamPos(VECTOR3* cam_pos);
extern void vcGetNowWatchPos(VECTOR3* watch_pos);
extern const unsigned char* g_sdlKeyboardState;
#include "debug_console.h"
#include "map_registry.h"
#endif
#include <psyq/libetc.h>

#include "bodyprog/bodyprog.h"
#include "bodyprog/demo.h"
#include "bodyprog/events/events_main.h"
#include "bodyprog/screen/background_draw.h"
#include "bodyprog/screen/screen_data.h"
#include "bodyprog/screen/screen_draw.h"
#include "bodyprog/screen/vsync.h"
#include "bodyprog/sys/joy.h"
#include "bodyprog/text/text_draw.h"
#include "bodyprog/math/math.h"
#include "bodyprog/memcard.h"
#include "bodyprog/sound_system.h"
#include "screens/b_konami/b_konami.h"

#include "bodyprog/memcard.h"
#include "bodyprog/sys/game_main.h"
#include "screens/saveload.h"


// ========================================
// GLOBAL VARIABLES
// ========================================

s32 g_Demo_FrameCount = 0;
s32 g_WarmBootTimer = 0;

// ========================================
// STATIC VARIABLES
// ========================================

static s32 g_PrevVBlanks = 0;

#ifdef SH_PC_PORT
/* Packet buffer corruption detection canaries */
/* With preloading, up to ~25 chunks render (5x5 grid around player).
 * Each chunk uses ~40KB of primitives. 25 * 40 = 1MB. 2MB gives headroom. */
#define PC_PKTBUF_SIZE (2 * 1024 * 1024)
#define PC_CANARY_SIZE 64
#define PC_CANARY_VAL  0xDE
static PACKET* s_PcPacketBufs[2] = { NULL, NULL };
static PACKET* s_PcPacketBufEnds[2] = { NULL, NULL };
#endif

// Audio task for `SD_Call` meant to load base VAB audios.
static u16 g_baseVabAudiosTaskId[] = {
   160,
   162,
   0
};

static void (*g_GameStateUpdateFuncs[])(void) = {
    GameState_Boot_Update,
    GameState_KonamiLogo_Update,
    GameState_KcetLogo_Update,
    GameState_MovieIntroFadeIn_Update,
    GameState_AutoLoadSavegame_Update,
    GameState_MovieIntroAlternate_Update,
    GameState_MovieIntro_Update,
    GameState_MainMenu_Update,
    GameState_LoadSavegameScreen_Update,
    GameState_MovieOpening_Update,
    GameState_LoadScreen_Update,
    GameState_InGame_Update,
    GameState_MapEvent_Update,
    GameState_ExitMovie_Update,
    GameState_ItemScreens_Update,
    GameState_MapScreen_Update,
    GameState_LoadSavegameScreen_Update,
    GameState_DebugMoviePlayer_Update,
    GameState_Options_Update,
    GameState_LoadStatusScreen_Update,
    GameState_LoadMapScreen_Update,
    GameState_Unk15_Update
};

// ========================================
// DEBUG CAMERA (PC PORT)
// ========================================

#ifdef SH_PC_PORT
int g_DebugCamEnabled = 0;  /* 0 = normal camera, 1 = debug camera */
int g_DebugFogDisabled = 0; /* 0 = fog normal, 1 = fog forced off (debug cam only) */
int g_DebugNoWallCollision = 0;  /* 0 = wall collision on, 1 = walk through walls */
int g_DebugNoFloorCollision = 0; /* 0 = floor collision on, always on (toggle removed) */
int g_DebugThirdPersonCam = 0;   /* 0 = game camera, 1 = static third-person follow cam */
s32 g_TpsCamYaw = 0;             /* TPS orbit yaw (Q12), independent from Harry's body */
s32 g_TpsCamPitch = 0;           /* TPS orbit pitch (Q12) */
int g_SH_PostFireTrace = 0;      /* Frames remaining of verbose post-fire main-loop tracing */
int g_SH_AlwaysMlTrace = 1;      /* 1 = unconditional ML_TRACE every frame; flip to 0 once silent crashes are diagnosed */
int g_DebugUnlockFps = 0;        /* 0 = fps_cap from config, 1 = uncapped (debug toggle) */
static int g_DebugCamInited = 0;
static int g_DebugCamTogglePrev = 0; /* for edge detection on toggle key */
static int g_DebugFogTogglePrev = 0;
static VECTOR3 g_DebugCamPos;
static VECTOR3 g_DebugCamLookAt;
static q3_12 g_DebugCamAngleY = 0;
static q3_12 g_DebugCamAngleX = 0; /* pitch/tilt: positive = look down, negative = look up */
static VECTOR3 g_DebugCamSavedHarryPos; /* Harry's position when debug cam was enabled */
static s32 g_DebugCamSavedHarryPosY;    /* Separate Y for collision restore */

/* ---- Normal-camera "nudge" state (numpad-driven manual cam tweaking) ----
 * vcMoveAndSetCamera produces a camera each frame; we want to let the user
 * nudge that output in real time so they can find a good camera and log it
 * via top-row 4/5. The nudges are an additive delta on top of whatever the
 * normal cam computed:
 *     final cam_pos     = vcWork.cam_pos       + g_PcCamNudgePos
 *     final watch_tgt   = (rotate watch_tgt around cam_pos by yaw nudge,
 *                          pitch yaw by pitch nudge) + g_PcCamNudgePos
 * Numpad 3 zeroes the nudges, restoring the scene's default cam.
 *
 * "Default cam" tracking: every frame BEFORE we apply nudges we record
 * vcWork.cam_pos / watch_tgt_pos in g_DefaultCam. That way the natural
 * cam-road interpolation drives the default and Numpad 3 simply discards
 * accumulated tweaks. */
typedef struct {
    VECTOR3 pos;
    VECTOR3 lookAt;
    s32     valid;
} s_DefaultCamera;

static VECTOR3       g_PcCamNudgePos     = {0, 0, 0};
static s32           g_PcCamNudgeYaw     = 0; /* Q3.12 added to cam yaw */
static s32           g_PcCamNudgePitch   = 0; /* Q3.12 added to cam pitch */
static s_DefaultCamera g_DefaultCam      = {{0,0,0}, {0,0,0}, 0};

void DebugCamera_Update(void)
{
    #define DBG_CAM_MOVE_SPEED 512   /* Q12(0.125) */
    #define DBG_CAM_TURN_SPEED 16
    #define DBG_CAM_VERT_SPEED 256

    if (!g_sdlKeyboardState) return;
    if (g_GameWork.gameState != GameState_InGame) return;

    /* Numpad *: toggle debug camera on/off (edge-triggered) */
    {
        int cur = g_sdlKeyboardState[SDL_SCANCODE_KP_MULTIPLY];
        if (cur && !g_DebugCamTogglePrev) {
            g_DebugCamEnabled = !g_DebugCamEnabled;
            if (g_DebugCamEnabled) {
                /* Capture current camera as starting point */
                vcGetNowCamPos(&g_DebugCamPos);
                g_DebugCamAngleY = g_SysWork.cameraAngleY;
                g_DebugCamAngleX = 0;
                g_DebugCamInited = 1;
                /* Save Harry's position to restore when debug cam is disabled */
                g_DebugCamSavedHarryPos = g_SysWork.playerWork.player.position;
                g_DebugCamSavedHarryPosY = g_SysWork.playerWork.player.properties.player.positionY_EC;
                /* Keep Harry visible at his original position so we can
                 * see him while flying the debug cam — useful for
                 * marking corrected camera positions relative to him.
                 * (Was hidden + teleport-followed in the prior design.) */
                SH_DBG("[DBGCAM] ENABLED pos=(%ld,%ld,%ld) harryPos saved=(%ld,%ld,%ld)",
                    (long)g_DebugCamPos.vx, (long)g_DebugCamPos.vy, (long)g_DebugCamPos.vz,
                    (long)g_DebugCamSavedHarryPos.vx, (long)g_DebugCamSavedHarryPos.vy, (long)g_DebugCamSavedHarryPos.vz);
            } else {
                /* Restore Harry's original position + visibility */
                g_SysWork.playerWork.player.position = g_DebugCamSavedHarryPos;
                g_SysWork.playerWork.player.properties.player.positionY_EC = g_DebugCamSavedHarryPosY;
                g_SysWork.playerWork.player.model.anim.flags |= AnimFlag_Visible;
                g_SysWork.playerWork.extra.model.anim.flags |= AnimFlag_Visible;
                SH_DBG("[DBGCAM] DISABLED — restored harry to (%ld,%ld,%ld)",
                    (long)g_DebugCamSavedHarryPos.vx, (long)g_DebugCamSavedHarryPos.vy, (long)g_DebugCamSavedHarryPos.vz);
            }
        }
        g_DebugCamTogglePrev = cur;
    }

    /* Fog toggle moved to main loop (runs after game sets fog params) */

    /* Numpad 0: cycle to next map overlay (edge-triggered)
     * DISABLED: runtime map switching crashes (map data not safely teardown-able).
     * Use config.cfg map= setting instead. */
#if 0
    {
        static int prevKey = 0;
        int cur = g_sdlKeyboardState[SDL_SCANCODE_KP_0];
        if (cur && !prevKey) {
            int curId = (int)g_SavegamePtr->mapOverlayId_A4;
            int nextId = (curId + 1) % (MapOverlayId_MAPX_S00 + 1);
            g_SavegamePtr->mapOverlayId_A4 = nextId;
            MapRegistry_Load((e_MapOverlayId)nextId);
            extern void GameBoot_MapLoad(s32 mapIdx);
            GameBoot_MapLoad(nextId);
            SH_DBG("[DEBUG] Switched to map %s (overlay %d)",
                MapRegistry_GetName(nextId), nextId);
        }
        prevKey = cur;
    }
#endif

    /* Numpad 1: toggle wall collision (edge-triggered) */
    {
        static int prevKey = 0;
        int cur = g_sdlKeyboardState[SDL_SCANCODE_KP_1];
        if (cur && !prevKey) {
            g_DebugNoWallCollision = !g_DebugNoWallCollision;
            SH_DBG("[DEBUG] Wall collision: %s", g_DebugNoWallCollision ? "OFF (noclip)" : "ON");
        }
        prevKey = cur;
    }
    /* Numpad 2: toggle third-person follow camera (edge-triggered) */
    {
        static int prevKey = 0;
        int cur = g_sdlKeyboardState[SDL_SCANCODE_KP_2];
        if (cur && !prevKey) {
            g_DebugThirdPersonCam = !g_DebugThirdPersonCam;
            /* Capture/release mouse for TPS mode */
            SDL_SetRelativeMouseMode(g_DebugThirdPersonCam ? SDL_TRUE : SDL_FALSE);
            SH_DBG("[DEBUG] Third-person camera: %s (mouse %s)", g_DebugThirdPersonCam ? "ON" : "OFF",
                   g_DebugThirdPersonCam ? "captured" : "released");
        }
        prevKey = cur;
    }

    /* Number key 1: kill Harry (triggers death animation) */
    {
        static int prevKey = 0;
        int cur = g_sdlKeyboardState[SDL_SCANCODE_1];
        if (cur && !prevKey) {
            g_SysWork.playerWork.player.health = -Q12(1.0f);
            SH_DBG("[DEBUG] Key 1: KILL HARRY — health set to -Q12(1.0)");
        }
        prevKey = cur;
    }
    /* Number keys 4/5: log BAD / GOOD camera position.
     * Reads whichever camera is currently active (normal vcMoveAndSetCamera,
     * debug free-fly, or TPS) and writes pos/lookAt/pitch/yaw/FOV/mode.
     * Implemented as a shared block so both keys produce identical fields. */
    {
        static int prevKey4 = 0, prevKey5 = 0;
        int cur4 = g_sdlKeyboardState[SDL_SCANCODE_4];
        int cur5 = g_sdlKeyboardState[SDL_SCANCODE_5];
        const char* tag = NULL;
        if (cur4 && !prevKey4) tag = "BAD CAMERA POSITION";
        else if (cur5 && !prevKey5) tag = "GOOD CAMERA POSITION";
        if (tag) {
            VECTOR3 camPos, lookAt;
            s32     pitch, yaw;
            const char* mode;
            if (g_DebugCamEnabled) {
                /* Free-fly debug cam — read its own state. */
                camPos = g_DebugCamPos;
                lookAt = g_DebugCamLookAt;
                yaw    = (s32)g_DebugCamAngleY;
                pitch  = (s32)g_DebugCamAngleX;
                mode   = "DEBUG";
            } else if (g_DebugThirdPersonCam) {
                /* TPS orbit cam — re-read live vcWork (TPS calls
                 * Vw_SetLookAtMatrix, so vcWork may not be in sync;
                 * use TPS state directly). */
                vcGetNowCamPos(&camPos);
                vcGetNowWatchPos(&lookAt);
                yaw   = g_TpsCamYaw;
                pitch = g_TpsCamPitch;
                mode  = "TPS";
            } else {
                /* Normal cam (vcMoveAndSetCamera + nudges already applied
                 * earlier this frame). vcWork is the authoritative source. */
                camPos = vcWork.cam_pos;
                lookAt = vcWork.watch_tgt_pos;
                yaw    = (s32)vcWork.cam_mat_ang.vy;
                pitch  = (s32)vcWork.cam_mat_ang.vx;
                mode   = "NORMAL";
            }
            SH_DBG("[%s] tick=%u map=%d mode=%s harry=(%ld,%ld,%ld) bodyYaw=%d",
                tag,
                (unsigned)SDL_GetTicks(),
                (int)g_SavegamePtr->mapOverlayId_A4, mode,
                (long)g_SysWork.playerWork.player.position.vx,
                (long)g_SysWork.playerWork.player.position.vy,
                (long)g_SysWork.playerWork.player.position.vz,
                (int)g_SysWork.playerWork.player.rotation.vy);
            SH_DBG("[%s] pos=(%ld,%ld,%ld) lookAt=(%ld,%ld,%ld) pitch=%d yaw=%d FOV=%d nudge=(%ld,%ld,%ld) yawN=%d pitchN=%d",
                tag,
                (long)camPos.vx, (long)camPos.vy, (long)camPos.vz,
                (long)lookAt.vx, (long)lookAt.vy, (long)lookAt.vz,
                (int)pitch, (int)yaw, (int)vcWork.geom_screen_dist,
                (long)g_PcCamNudgePos.vx, (long)g_PcCamNudgePos.vy, (long)g_PcCamNudgePos.vz,
                (int)g_PcCamNudgeYaw, (int)g_PcCamNudgePitch);
        }
        prevKey4 = cur4;
        prevKey5 = cur5;
    }

    /* Number key 6: log Harry's current world position (works in any cam
     * mode, not just TPS). Useful for capturing fall-through-floor or
     * stuck-collision spots so we can build a fix from real coordinates. */
    {
        static int prevKey6 = 0;
        int cur6 = g_sdlKeyboardState[SDL_SCANCODE_6];
        if (cur6 && !prevKey6) {
            VECTOR3* p = &g_SysWork.playerWork.player.position;
            SH_DBG("[POS-LOG] mapId=%d harryPos=(%ld,%ld,%ld) yaw=%d health=%ld fallSpeed=%ld",
                (int)g_SavegamePtr->mapOverlayId_A4,
                (long)p->vx, (long)p->vy, (long)p->vz,
                (int)g_SysWork.playerWork.player.rotation.vy,
                (long)g_SysWork.playerWork.player.health,
                (long)g_SysWork.playerWork.player.fallSpeed);
        }
        prevKey6 = cur6;
    }

    /* Room-enter logging + Numpad 3 rescue-Y teleport.
     *
     * On room-index transition, snapshot Harry's current Y as the rescue
     * target. Numpad 3 teleports vy back to that snapshot. We deliberately
     * do NOT continuously refresh on fallSpeed==0 — fallSpeed momentarily
     * hits 0 mid-fall (after collision detection fails and Harry teleports
     * to a fall-through position), which would overwrite the safe Y with
     * the very Y we want to escape from. The room-enter snapshot is the
     * authoritative "last known good ground Y" and only updates on actual
     * room boundaries. */
    {
        static s32 _lastSafeY      = 0;
        static int _haveSafeY      = 0;
        static s8  _prevRoomIdx    = -1;
        static s8  _prevMapId      = -1;
        static int prevKp3         = 0;

        if (g_GameWork.gameState == GameState_InGame) {
            VECTOR3*  p   = &g_SysWork.playerWork.player.position;
            s_SubCharacter* pl = &g_SysWork.playerWork.player;
            s8 curRoom    = g_SavegamePtr->mapRoomIdx_A5;
            s8 curMap     = g_SavegamePtr->mapOverlayId_A4;

            /* Snapshot rescue Y on map or room change. */
            if (curMap != _prevMapId || curRoom != _prevRoomIdx) {
                SH_DBG("[ROOM-ENTER] mapId=%d roomIdx=%d harryPos=(%ld,%ld,%ld) — saving Y=%ld as rescue target",
                    (int)curMap, (int)curRoom,
                    (long)p->vx, (long)p->vy, (long)p->vz, (long)p->vy);
                _lastSafeY = p->vy;
                _haveSafeY = 1;
                _prevMapId   = curMap;
                _prevRoomIdx = curRoom;
            }

            /* Numpad 3 (edge-detect): teleport vy to last safe Y AND push
             * Harry backward Q12(2.5f) world units in the direction he came
             * from (opposite of facing). Without the X/Z push he'd land on
             * the same broken floor spot and immediately fall again — log
             * showed exactly that, vy 32768 → -608 → 32768 → -608 looping. */
            int curKp3 = g_sdlKeyboardState[SDL_SCANCODE_KP_3];
            if (curKp3 && !prevKp3 && _haveSafeY) {
                s32 oldY = p->vy;
                s32 oldX = p->vx;
                s32 oldZ = p->vz;
                s32 facing = pl->rotation.vy;
                s32 sinV = Math_Sin(facing);
                s32 cosV = Math_Cos(facing);
                s32 pushDist = Q12(2.5f);
                p->vy = _lastSafeY;
                pl->fallSpeed = 0;
                /* Push backward — sin/cos give forward direction, subtract. */
                p->vx -= (s32)((s64)sinV * pushDist >> 12);
                p->vz -= (s32)((s64)cosV * pushDist >> 12);
                SH_DBG("[RESCUE-Y] Numpad 3: pos (%ld,%ld,%ld) → (%ld,%ld,%ld) yaw=%d (vy reset + 2.5u backward push)",
                    (long)oldX, (long)oldY, (long)oldZ,
                    (long)p->vx, (long)p->vy, (long)p->vz,
                    (int)facing);
            }
            prevKp3 = curKp3;
        }
    }

    /* F-key debug hotkeys removed: F4 (handgun) was freezing controls and
     * the test map already has knife + pistol pickups, so the bindings
     * are unnecessary here. Re-add when an in-progress later-map test
     * needs an item that isn't in the world yet. */

    /* ==== Normal-camera manual tweak (numpad nudges) + Numpad 3 reset ====
     * Active in NORMAL camera mode only (not debug-cam, not TPS). Reads
     * keyboard, accumulates an additive offset on top of vcMoveAndSetCamera
     * and applies it to vcWork via Vw_SetLookAtMatrix. ~5x less sensitive
     * than the debug-cam speeds so nudges are usable for fine-tuning.
     *
     * Numpad 3 zeroes the nudge accumulator, snapping the camera back to
     * vcMoveAndSetCamera's natural output (the "default" cam). Tracking a
     * separate g_DefaultCam isn't actually needed — vcWork already holds
     * the default before our nudges land — but we still update it each
     * frame so external observers (logging, future commands) can see the
     * pristine pre-nudge cam. */
    if (!g_DebugCamEnabled && !g_DebugThirdPersonCam &&
        g_GameWork.gameState == GameState_InGame)
    {
        #define PC_NUDGE_MOVE_SPEED  102   /* Q12(~0.025) — ~5x slower than debug 512 */
        #define PC_NUDGE_TURN_SPEED  3     /* ~5x slower than debug 16 */
        #define PC_NUDGE_VERT_SPEED  51    /* ~5x slower than debug 256 */

        /* Snapshot the pristine default cam BEFORE nudge application.
         * vcMoveAndSetCamera ran earlier this frame and put its result in
         * vcWork — that's our default. */
        g_DefaultCam.pos    = vcWork.cam_pos;
        g_DefaultCam.lookAt = vcWork.watch_tgt_pos;
        g_DefaultCam.valid  = 1;

        /* Numpad 3: reset nudge accumulator (held = repeats; cheap) */
        {
            static int prevKey3 = 0;
            int cur3 = g_sdlKeyboardState[SDL_SCANCODE_KP_3];
            if (cur3 && !prevKey3) {
                g_PcCamNudgePos.vx = 0;
                g_PcCamNudgePos.vy = 0;
                g_PcCamNudgePos.vz = 0;
                g_PcCamNudgeYaw    = 0;
                g_PcCamNudgePitch  = 0;
                SH_DBG("[CAM-RESET] nudges cleared — restored to default cam=(%ld,%ld,%ld) lookAt=(%ld,%ld,%ld)",
                    (long)g_DefaultCam.pos.vx, (long)g_DefaultCam.pos.vy, (long)g_DefaultCam.pos.vz,
                    (long)g_DefaultCam.lookAt.vx, (long)g_DefaultCam.lookAt.vy, (long)g_DefaultCam.lookAt.vz);
            }
            prevKey3 = cur3;
        }

        /* Read numpad nudge keys (held = continuous). Camera-relative
         * forward/strafe uses the cam's current yaw so 8 always pushes
         * "into the screen". */
        {
            s32 camYaw = (s32)vcWork.cam_mat_ang.vy + g_PcCamNudgeYaw;
            s32 sinY   = Math_Sin(camYaw);
            s32 cosY   = Math_Cos(camYaw);

            /* Numpad 8/5: forward / back (cam-relative XZ) */
            if (g_sdlKeyboardState[SDL_SCANCODE_KP_8]) {
                g_PcCamNudgePos.vx += (s32)((s64)PC_NUDGE_MOVE_SPEED * sinY >> 12);
                g_PcCamNudgePos.vz += (s32)((s64)PC_NUDGE_MOVE_SPEED * cosY >> 12);
            }
            if (g_sdlKeyboardState[SDL_SCANCODE_KP_5]) {
                g_PcCamNudgePos.vx -= (s32)((s64)PC_NUDGE_MOVE_SPEED * sinY >> 12);
                g_PcCamNudgePos.vz -= (s32)((s64)PC_NUDGE_MOVE_SPEED * cosY >> 12);
            }
            /* Numpad 4/6: strafe left / right */
            if (g_sdlKeyboardState[SDL_SCANCODE_KP_4]) {
                g_PcCamNudgePos.vx -= (s32)((s64)PC_NUDGE_MOVE_SPEED * cosY >> 12);
                g_PcCamNudgePos.vz += (s32)((s64)PC_NUDGE_MOVE_SPEED * sinY >> 12);
            }
            if (g_sdlKeyboardState[SDL_SCANCODE_KP_6]) {
                g_PcCamNudgePos.vx += (s32)((s64)PC_NUDGE_MOVE_SPEED * cosY >> 12);
                g_PcCamNudgePos.vz -= (s32)((s64)PC_NUDGE_MOVE_SPEED * sinY >> 12);
            }
            /* Numpad 7/9: turn left / right (yaw) */
            if (g_sdlKeyboardState[SDL_SCANCODE_KP_7]) {
                g_PcCamNudgeYaw -= PC_NUDGE_TURN_SPEED;
            }
            if (g_sdlKeyboardState[SDL_SCANCODE_KP_9]) {
                g_PcCamNudgeYaw += PC_NUDGE_TURN_SPEED;
            }
            /* Numpad +/-: tilt up / down (pitch). Matches debug cam.
             * The pitch nudge biases the lookAt Y at the apply site
             * (search g_PcCamNudgePitch). PSX +Y = down → KP_PLUS goes
             * up by subtracting from Y. */
            if (g_sdlKeyboardState[SDL_SCANCODE_KP_PLUS]) {
                g_PcCamNudgePitch -= PC_NUDGE_VERT_SPEED;
            }
            if (g_sdlKeyboardState[SDL_SCANCODE_KP_MINUS]) {
                g_PcCamNudgePitch += PC_NUDGE_VERT_SPEED;
            }
            /* Page Up / Page Down: vertical move (camera-Y) — also
             * matches debug cam's vertical bindings. */
            if (g_sdlKeyboardState[SDL_SCANCODE_PAGEUP]) {
                g_PcCamNudgePos.vy -= PC_NUDGE_VERT_SPEED;
            }
            if (g_sdlKeyboardState[SDL_SCANCODE_PAGEDOWN]) {
                g_PcCamNudgePos.vy += PC_NUDGE_VERT_SPEED;
            }
        }

        /* Numpad /: print current nudged camera (works in normal cam) */
        {
            static int npslPrev = 0;
            int cur = g_sdlKeyboardState[SDL_SCANCODE_KP_DIVIDE];
            if (cur && !npslPrev) {
                SH_DBG("[CAM] default pos=(%ld,%ld,%ld) lookAt=(%ld,%ld,%ld) yaw=%d",
                    (long)g_DefaultCam.pos.vx, (long)g_DefaultCam.pos.vy, (long)g_DefaultCam.pos.vz,
                    (long)g_DefaultCam.lookAt.vx, (long)g_DefaultCam.lookAt.vy, (long)g_DefaultCam.lookAt.vz,
                    (int)vcWork.cam_mat_ang.vy);
                SH_DBG("[CAM] nudge   pos=(%ld,%ld,%ld) yaw=%d pitch=%d",
                    (long)g_PcCamNudgePos.vx, (long)g_PcCamNudgePos.vy, (long)g_PcCamNudgePos.vz,
                    (int)g_PcCamNudgeYaw, (int)g_PcCamNudgePitch);
            }
            npslPrev = cur;
        }

        /* Scene-baseline cam corrections (built up from in-game tuning).
         * When Harry is within `radius` of one of these world positions on
         * the matching map, the listed nudge gets applied EVERY FRAME on
         * top of vcMoveAndSetCamera's natural cam — same shape as the
         * numpad runtime nudge, but baked in. The runtime nudge stacks
         * on top, so the user can still fine-tune at runtime.
         *
         * Each entry is one fixed-cam tuning the user logged via the
         * Number-key 4 / 5 BAD/GOOD pair. Adding more entries = adding
         * more rows here. radius is in PSX Q12 world units (~1m=4096). */
        struct CamCorrection {
            int     mapId;          /* mapOverlayId_A4 */
            VECTOR3 harryPos;       /* center of correction zone */
            s32     radius2;        /* squared radius (Q12)^2; uses XZ only */
            VECTOR3 posDelta;       /* pos nudge */
            s32     yawDelta;       /* yaw nudge (Q3.12) */
            s32     pitchDelta;     /* pitch nudge (Y bias) */
        };
        static const struct CamCorrection s_camCorrections[] = {
            /* map0_s01 alley near AS room — user-logged GOOD vs BAD pair
             * (SilentHill.log lines 103870/150358). User confirmed the
             * default cam at this position was bad and the nudged version
             * looks correct. radius ≈ 2m (Q12(2.0)^2). */
            {
                .mapId      = 1,
                .harryPos   = { 18815, 0, 1088743 },
                .radius2    = (s32)((s64)Q12(2.0f) * Q12(2.0f) >> 12),
                .posDelta   = { -1818, 612, -252 },
                .yawDelta   = -72,
                .pitchDelta = 3774,
            },
            /* map2_s00 cafe Bachman corner near KeyOfWoodman pickup — user
             * logged BAD/GOOD pair from SilentHill.log: BAD had pitchN=0
             * (camera tilted up at ceiling), GOOD had pitchN=-26112
             * (camera tilts down to frame Harry). Harry's pos at the
             * freeze: (-781992, 0, 1543104). The camera at this scene
             * has lookAt.vy=+2048 by default — adding pitchDelta=-26112
             * shifts look.vy to -24064 in +Y=up convention so camera
             * pitches downward to look at Harry. radius ≈ 4m (this is a
             * fairly large interior so the same fixed-cam shot covers a
             * wide area in cafe). */
            {
                .mapId      = 10,                       /* map2_s00 */
                .harryPos   = { -781992, 0, 1543104 },
                .radius2    = (s32)((s64)Q12(4.0f) * Q12(4.0f) >> 12),
                .posDelta   = { 0, 0, 0 },
                .yawDelta   = 0,
                .pitchDelta = -26112,
            },
            /* map2_s00 alleyway — user logged BAD vs GOOD pair: camera was
             * "way too far up" through the whole alley. Default cam needs
             * to drop Y by 3927 and pitch DOWN by 7956 (PSX +Y = down, so
             * positive posY pushes cam down; negative pitch pushes lookAt
             * up so cam pitches down to look at Harry). Generous radius
             * because the alleyway is long; same fixed-cam shot covers a
             * decent stretch. If parts of the alley still need different
             * tuning, add more rows. */
            {
                .mapId      = 10,                       /* map2_s00 */
                .harryPos   = { 232058, 0, 402043 },
                .radius2    = (s32)((s64)Q12(6.0f) * Q12(6.0f) >> 12),
                .posDelta   = { 0, 3927, 0 },
                .yawDelta   = 0,
                .pitchDelta = -7956,
            },
            /* map2_s00 streets fixed-cam near (460000,0,886000) — user logged
             * BAD/GOOD pair; default cam misframed Harry. The vy lift is the
             * "z/vertical" value from the user's second GOOD log entry (2907,
             * adjusted down from the first GOOD's 5355). yawN/pitchN are the
             * user's tuned rotation (yaw -60 nudges right, pitch -10251 lifts
             * lookAt up so cam pitches downward). radius ≈ 2m. */
            {
                .mapId      = 10,                       /* map2_s00 */
                .harryPos   = { 459965, 0, 886197 },
                .radius2    = (s32)((s64)Q12(2.0f) * Q12(2.0f) >> 12),
                .posDelta   = { 57, 2907, 78 },
                .yawDelta   = -60,
                .pitchDelta = -10251,
            },
        };
        VECTOR3 sceneNudgePos = {0, 0, 0};
        s32     sceneNudgeYaw   = 0;
        s32     sceneNudgePitch = 0;
        {
            const VECTOR3* hp = &g_SysWork.playerWork.player.position;
            int curMap = (int)g_SavegamePtr->mapOverlayId_A4;
            for (size_t i = 0; i < sizeof(s_camCorrections) / sizeof(s_camCorrections[0]); i++) {
                const struct CamCorrection* cc = &s_camCorrections[i];
                if (cc->mapId != curMap) continue;
                s64 dx = hp->vx - cc->harryPos.vx;
                s64 dz = hp->vz - cc->harryPos.vz;
                /* Q12 distance squared scaled down to fit s32. */
                s32 d2 = (s32)(((dx * dx) + (dz * dz)) >> 12);
                if (d2 > cc->radius2) continue;
                sceneNudgePos   = cc->posDelta;
                sceneNudgeYaw   = cc->yawDelta;
                sceneNudgePitch = cc->pitchDelta;
                break;
            }
        }

        /* Apply nudge: rebuild cam_pos / watch_tgt and rebuild view matrix.
         * lookAt is rotated around cam_pos by the yaw nudge so the camera
         * "swings" rather than parallel-translating. Combine the
         * scene-baseline correction with the runtime numpad nudge. */
        s32 effPosX  = g_PcCamNudgePos.vx + sceneNudgePos.vx;
        s32 effPosY  = g_PcCamNudgePos.vy + sceneNudgePos.vy;
        s32 effPosZ  = g_PcCamNudgePos.vz + sceneNudgePos.vz;
        s32 effYaw   = g_PcCamNudgeYaw    + sceneNudgeYaw;
        s32 effPitch = g_PcCamNudgePitch  + sceneNudgePitch;
        if (effPosX | effPosY | effPosZ | effYaw | effPitch)
        {
            VECTOR3 newCam, newLook;
            VECTOR3 dl;
            s32 syN, cyN, dx, dz;

            newCam.vx = vcWork.cam_pos.vx + effPosX;
            newCam.vy = vcWork.cam_pos.vy + effPosY;
            newCam.vz = vcWork.cam_pos.vz + effPosZ;

            /* Original lookAt relative to cam */
            dl.vx = vcWork.watch_tgt_pos.vx - vcWork.cam_pos.vx;
            dl.vy = vcWork.watch_tgt_pos.vy - vcWork.cam_pos.vy;
            dl.vz = vcWork.watch_tgt_pos.vz - vcWork.cam_pos.vz;

            /* Rotate XZ component of dl by yaw nudge */
            syN = Math_Sin(effYaw);
            cyN = Math_Cos(effYaw);
            dx  = (s32)(((s64)dl.vx * cyN + (s64)dl.vz * syN) >> 12);
            dz  = (s32)((-(s64)dl.vx * syN + (s64)dl.vz * cyN) >> 12);

            newLook.vx = newCam.vx + dx;
            newLook.vy = newCam.vy + dl.vy + effPitch; /* crude pitch as Y bias */
            newLook.vz = newCam.vz + dz;

            Vw_SetLookAtMatrix(&newCam, &newLook);
            vwSetViewInfo();

            /* Periodic trace so log shows nudge cam is live */
            {
                static int tickCounter = 0;
                if ((++tickCounter & 0x3F) == 0) {
                    SH_DBG("[CAM-NUDGE] cam=(%ld,%ld,%ld) look=(%ld,%ld,%ld) yawN=%d (scene=%d,%d,%d / %d,%d)",
                        (long)newCam.vx, (long)newCam.vy, (long)newCam.vz,
                        (long)newLook.vx, (long)newLook.vy, (long)newLook.vz,
                        (int)effYaw,
                        (int)sceneNudgePos.vx, (int)sceneNudgePos.vy, (int)sceneNudgePos.vz,
                        (int)sceneNudgeYaw, (int)sceneNudgePitch);
                }
            }
        }

        #undef PC_NUDGE_MOVE_SPEED
        #undef PC_NUDGE_TURN_SPEED
        #undef PC_NUDGE_VERT_SPEED
    }

    /* If free-fly debug cam is off */
    if (!g_DebugCamEnabled) {
        /* Third-person orbit camera. Mouse moves the camera around Harry
         * (head-look style); Harry's body only rotates when a movement
         * key is pressed and snaps toward the camera-relative direction.
         * Movement input handling lives in player_control.c TPS branch. */
        if (g_DebugThirdPersonCam) {
            #define TP_DIST         Q12(2.5f)    /* orbit radius from Harry */
            #define TP_HEIGHT       Q12(-1.4f)   /* base lift above Harry (Y-up = negative) */
            #define TP_LOOKAT_OFS   Q12(-0.85f)  /* Y offset for look target (Harry's chest) */
            #define TP_MOUSE_SENS   6            /* Q12 units per pixel for yaw */
            #define TP_PITCH_SENS   2            /* Q12 units per pixel for pitch */

            s_SubCharacter* tp_hr = &g_SysWork.playerWork.player;

            /* Mouse: orbit the camera, decoupled from Harry's body */
            {
                int mdx = 0, mdy = 0;
                SDL_GetRelativeMouseState(&mdx, &mdy);
                /* GTA-style orbit camera. Convention:
                 *   yaw=0   → camera south of Harry, looking north
                 *   pitch>0 → camera looks UP (pitches up, dips below)
                 *   pitch<0 → camera looks DOWN (pitches down, rises above)
                 *
                 * Mouse-RIGHT (mdx>0) → += yaw → view rotates right (FPS) ✓
                 * Mouse-UP    (mdy<0) → -=mdy → += pitch → view tilts up   ✓
                 * Mouse-DOWN  (mdy>0) → -=mdy → -= pitch → view tilts down ✓ */
                g_TpsCamYaw   += (s32)(mdx * TP_MOUSE_SENS);
                g_TpsCamYaw    = Q12_ANGLE_NORM_U(g_TpsCamYaw + Q12_ANGLE(360.0f));
                g_TpsCamPitch -= (s32)(mdy * TP_PITCH_SENS);
                /* Tighter clamp on the look-down side so the camera doesn't
                 * rise far over Harry's head; symmetric range was making the
                 * cam pop overhead easily. */
                if (g_TpsCamPitch < -Q12_ANGLE(40.0f)) g_TpsCamPitch = -Q12_ANGLE(40.0f);
                if (g_TpsCamPitch >  Q12_ANGLE(50.0f)) g_TpsCamPitch =  Q12_ANGLE(50.0f);
            }

            /* Compute view direction (forward unit vector) from yaw+pitch.
             * PSX -Y=up convention: pitch>0 (look up) → forward.y negative. */
            s32 sy = Math_Sin(g_TpsCamYaw);
            s32 cy = Math_Cos(g_TpsCamYaw);
            s32 sp = Math_Sin(g_TpsCamPitch);
            s32 cp = Math_Cos(g_TpsCamPitch);

            /* forward = (sin(yaw)*cos(pitch), -sin(pitch), cos(yaw)*cos(pitch))  Q12 */
            s32 fwdX = (s32)((s64)sy * cp >> 12);
            s32 fwdY = -sp;
            s32 fwdZ = (s32)((s64)cy * cp >> 12);

            /* Camera D units BACK along forward, lifted by TP_HEIGHT */
            VECTOR3 tpCamPos, tpLookAt;
            tpCamPos.vx = tp_hr->position.vx - (s32)((s64)TP_DIST * fwdX >> 12);
            tpCamPos.vy = tp_hr->position.vy - (s32)((s64)TP_DIST * fwdY >> 12) + TP_HEIGHT;
            tpCamPos.vz = tp_hr->position.vz - (s32)((s64)TP_DIST * fwdZ >> 12);

            /* lookAt FAR ahead, Y-anchored to Harry's chest. Previous
             * impl projected straight forward from camera origin (vy
             * = camPos.vy) which placed screen-center at the camera's
             * own Y — TP_HEIGHT above Harry's feet, ≈ his mid-back.
             * Hence the user complaint that the cam sits "halfway up
             * Harry's back". TP_LOOKAT_OFS was declared but never
             * applied. Anchor lookAt.vy at harry.y + TP_LOOKAT_OFS so
             * the screen-center crosshair lands on Harry's chest. The
             * X/Z still project forward by TP_LOOKAT_DIST so we keep
             * the Q12→Q8 anti-jitter benefit of a far target.
             *
             * Pitch contribution: bias additionally by sin(pitch) so
             * looking up/down still works — at pitch=0 the cam looks
             * dead-on at chest; at pitch=+50° the look target rides
             * higher; at pitch=-40° lower. */
            #define TP_LOOKAT_DIST Q12(25.0f)
            s32 anchorY = tp_hr->position.vy + TP_LOOKAT_OFS;
            tpLookAt.vx = tpCamPos.vx + (s32)((s64)TP_LOOKAT_DIST * fwdX >> 12);
            tpLookAt.vy = anchorY     + (s32)((s64)TP_LOOKAT_DIST * fwdY >> 12);
            tpLookAt.vz = tpCamPos.vz + (s32)((s64)TP_LOOKAT_DIST * fwdZ >> 12);
            #undef TP_LOOKAT_DIST

            Vw_SetLookAtMatrix(&tpCamPos, &tpLookAt);
            vwSetViewInfo();

            /* Key 6 (top row): snapshot TPS camera state for tuning */
            {
                static int s_tpLogPrev = 0;
                int s_tpLogCur = g_sdlKeyboardState[SDL_SCANCODE_6];
                if (s_tpLogCur && !s_tpLogPrev) {
                    SH_DBG("[TPS-SNAP] harry  pos=(%d,%d,%d)  bodyYaw=%d  camYaw=%d camPitch=%d",
                        tp_hr->position.vx, tp_hr->position.vy, tp_hr->position.vz,
                        (int)tp_hr->rotation.vy, (int)g_TpsCamYaw, (int)g_TpsCamPitch);
                    SH_DBG("[TPS-SNAP] camPos=(%d,%d,%d)  lookAt=(%d,%d,%d)",
                        tpCamPos.vx, tpCamPos.vy, tpCamPos.vz,
                        tpLookAt.vx, tpLookAt.vy, tpLookAt.vz);
                }
                s_tpLogPrev = s_tpLogCur;
            }

            #undef TP_DIST
            #undef TP_HEIGHT
            #undef TP_LOOKAT_OFS
            #undef TP_MOUSE_SENS
            #undef TP_PITCH_SENS
        }
        return;
    }

    int moved = 0;
    s32 sinY = Math_Sin(g_DebugCamAngleY);
    s32 cosY = Math_Cos(g_DebugCamAngleY);

    /* Numpad 8: forward */
    if (g_sdlKeyboardState[SDL_SCANCODE_KP_8]) {
        g_DebugCamPos.vx += (s32)((s64)DBG_CAM_MOVE_SPEED * sinY >> 12);
        g_DebugCamPos.vz += (s32)((s64)DBG_CAM_MOVE_SPEED * cosY >> 12);
        moved = 1;
    }
    /* Numpad 5: backward */
    if (g_sdlKeyboardState[SDL_SCANCODE_KP_5]) {
        g_DebugCamPos.vx -= (s32)((s64)DBG_CAM_MOVE_SPEED * sinY >> 12);
        g_DebugCamPos.vz -= (s32)((s64)DBG_CAM_MOVE_SPEED * cosY >> 12);
        moved = 1;
    }
    /* Numpad 4: strafe left */
    if (g_sdlKeyboardState[SDL_SCANCODE_KP_4]) {
        g_DebugCamPos.vx -= (s32)((s64)DBG_CAM_MOVE_SPEED * cosY >> 12);
        g_DebugCamPos.vz += (s32)((s64)DBG_CAM_MOVE_SPEED * sinY >> 12);
        moved = 1;
    }
    /* Numpad 6: strafe right */
    if (g_sdlKeyboardState[SDL_SCANCODE_KP_6]) {
        g_DebugCamPos.vx += (s32)((s64)DBG_CAM_MOVE_SPEED * cosY >> 12);
        g_DebugCamPos.vz -= (s32)((s64)DBG_CAM_MOVE_SPEED * sinY >> 12);
        moved = 1;
    }
    /* Numpad 7: turn left */
    if (g_sdlKeyboardState[SDL_SCANCODE_KP_7]) {
        g_DebugCamAngleY -= DBG_CAM_TURN_SPEED;
        moved = 1;
    }
    /* Numpad 9: turn right */
    if (g_sdlKeyboardState[SDL_SCANCODE_KP_9]) {
        g_DebugCamAngleY += DBG_CAM_TURN_SPEED;
        moved = 1;
    }
    /* Page Up: move up (Y-, PSX Y is inverted) */
    if (g_sdlKeyboardState[SDL_SCANCODE_PAGEUP]) {
        g_DebugCamPos.vy -= DBG_CAM_VERT_SPEED;
        moved = 1;
    }
    /* Page Down: move down (Y+) */
    if (g_sdlKeyboardState[SDL_SCANCODE_PAGEDOWN]) {
        g_DebugCamPos.vy += DBG_CAM_VERT_SPEED;
        moved = 1;
    }
    /* Numpad +: tilt up (look upward) */
    if (g_sdlKeyboardState[SDL_SCANCODE_KP_PLUS]) {
        g_DebugCamAngleX -= DBG_CAM_TURN_SPEED;
        moved = 1;
    }
    /* Numpad -: tilt down (look downward) */
    if (g_sdlKeyboardState[SDL_SCANCODE_KP_MINUS]) {
        g_DebugCamAngleX += DBG_CAM_TURN_SPEED;
        moved = 1;
    }
    /* Numpad /: print debug camera coordinates to log */
    {
        static int dbg_slash_prev = 0;
        int dbg_slash_cur = g_sdlKeyboardState[SDL_SCANCODE_KP_DIVIDE];
        if (dbg_slash_cur && !dbg_slash_prev) {
            SH_DBG("[DBGCAM] COORDS: pos=(%ld,%ld,%ld) angleY=%d",
                (long)g_DebugCamPos.vx, (long)g_DebugCamPos.vy, (long)g_DebugCamPos.vz,
                g_DebugCamAngleY);
            SH_DBG("[DBGCAM] HARRY:  pos=(%ld,%ld,%ld)",
                (long)g_SysWork.playerWork.player.position.vx,
                (long)g_SysWork.playerWork.player.position.vy,
                (long)g_SysWork.playerWork.player.position.vz);
        }
        dbg_slash_prev = dbg_slash_cur;
    }

    /* Keep Harry at his original position (do NOT teleport-follow the
     * debug cam). This makes it possible to fly the debug cam around
     * Harry and mark camera positions RELATIVE to where he actually is.
     * Texture/IPD chunks near the saved Harry position remain loaded.
     * Side effect: flying far away may show un-textured / un-loaded
     * geometry, but that's a fair trade for being able to mark
     * corrected-camera positions accurately. */
    {
        s_SubCharacter* hp = &g_SysWork.playerWork.player;
        hp->position.vx = g_DebugCamSavedHarryPos.vx;
        hp->position.vz = g_DebugCamSavedHarryPos.vz;
        hp->position.vy = g_DebugCamSavedHarryPos.vy;
    }

    /* Set look-at point ahead of camera, incorporating pitch (AngleX).
     * forward = distance projected onto XZ plane (cos(pitch) * 20480)
     * Y offset = sin(pitch) * 20480 (positive = down in PSX Y-down coords) */
    {
        s32 forward = (s32)((s64)20480 * Math_Cos(g_DebugCamAngleX) >> 12);
        g_DebugCamLookAt.vx = g_DebugCamPos.vx + (s32)((s64)forward * sinY >> 12);
        g_DebugCamLookAt.vy = g_DebugCamPos.vy + (s32)((s64)20480 * Math_Sin(g_DebugCamAngleX) >> 12);
        g_DebugCamLookAt.vz = g_DebugCamPos.vz + (s32)((s64)forward * cosY >> 12);
    }

    /* Override the camera view */
    Vw_SetLookAtMatrix(&g_DebugCamPos, &g_DebugCamLookAt);
    vwSetViewInfo();

    if (moved) {
        static int dbg_print_counter = 0;
        if (++dbg_print_counter % 30 == 0) {
            SH_DBG("[DBGCAM] pos=(%ld,%ld,%ld) angleY=%d harry=(%ld,%ld,%ld)",
                (long)g_DebugCamPos.vx, (long)g_DebugCamPos.vy, (long)g_DebugCamPos.vz,
                g_DebugCamAngleY,
                (long)g_SysWork.playerWork.player.position.vx,
                (long)g_SysWork.playerWork.player.position.vy,
                (long)g_SysWork.playerWork.player.position.vz);
        }
    }

    #undef DBG_CAM_MOVE_SPEED
    #undef DBG_CAM_TURN_SPEED
    #undef DBG_CAM_VERT_SPEED
}
#endif

// ========================================
// MAINLOOP
// ========================================

void GameState_Boot_Update(void) // 0x80032D1C
{
    s32 gameState;
    s32 VabAudioTaskId;

    switch (g_GameWork.gameStateSteps[0])
    {
        case 0:
            g_GameWork.background2dColor.r = 0;
            g_GameWork.background2dColor.g = 0;
            g_GameWork.background2dColor.b = 0;

            Screen_Init(SCREEN_WIDTH, false);
            g_SysWork.counters_1C[1]              = 0;
            g_GameWork.gameStateSteps[1] = 0;
            g_GameWork.gameStateSteps[2] = 0;
            g_GameWork.gameStateSteps[0]++;
            break;

        case 1:
            if (!Sd_AudioStreamingCheck())
            {
                VabAudioTaskId = g_baseVabAudiosTaskId[g_GameWork.gameStateSteps[1]];
                if (VabAudioTaskId != 0)
                {
                    SD_Call(VabAudioTaskId);
                    g_GameWork.gameStateSteps[1]++;
                }
                else
                {
                    g_SysWork.counters_1C[1]              = 0;
                    g_GameWork.gameStateSteps[1] = 0;
                    g_GameWork.gameStateSteps[2] = 0;
                    g_GameWork.gameStateSteps[0]++;
                }
            }
            break;

        case 2:
            Fs_QueueStartReadTim(FILE_1ST_FONT16_TIM, FS_BUFFER_1, &g_Font16AtlasImg);
            Fs_QueueStartReadTim(FILE_1ST_KONAMI_TIM, FS_BUFFER_1, &g_KonamiLogoImg);
#ifdef SH_PC_PORT
            if (g_PcConfig.skipIntros) {
                /* Replicate all loads that b_konami.c/b_kcet.c normally handle during logo display */
                WorldGfx_HarryCharaLoad();
                GameFs_BgItemLoad();
                GameFs_BgEtcGfxLoad(); /* snow/rain/particle textures (BG_ETC.TIM → VRAM tPage 12) */
                Map_EffectTexturesLoad(NO_VALUE);
                Fs_QueueStartRead(FILE_ANIM_HB_BASE_ANM, FS_BUFFER_0);
                GameFs_TitleGfxLoad();
            }
#endif
            ScreenFade_Start(true, false, false);
            g_GameWork.gameStateSteps[0]++;
            break;

        case 3:
            if (ScreenFade_IsFinished())
            {
                Fs_QueueWaitForEmpty();

                gameState = g_GameWork.gameState;

                g_SysWork.counters_1C[0] = 0;
                g_SysWork.counters_1C[1] = 0;

                g_GameWork.gameStateSteps[1] = 0;
                g_GameWork.gameStateSteps[2] = 0;

                SysWork_StateSetNext(SysState_Gameplay);

                g_GameWork.gameStateSteps[0] = gameState;
#ifdef SH_PC_PORT
                if (g_PcConfig.skipIntros) {
                    /* Normally called by b_konami.c; must happen before MainMenu */
                    Settings_RestoreDefaults();
                    g_GameWork.gameState = GameState_MainMenu;
                } else
#endif
                g_GameWork.gameState        = gameState + 1;
                g_GameWork.gameStatePrev    = gameState;
                g_GameWork.gameStateSteps[0] = 0;
            }
            break;
    }

#ifdef SH_PC_PORT
    { extern FILE* g_ShDebugLog; if (g_ShDebugLog) { fprintf(g_ShDebugLog, "[BOOT] step=%d pre func_80033548\n", (int)g_GameWork.gameStateSteps[0]); fflush(g_ShDebugLog); } }
#endif
    func_80033548();
#ifdef SH_PC_PORT
    { extern FILE* g_ShDebugLog; if (g_ShDebugLog) { fprintf(g_ShDebugLog, "[BOOT] post func_80033548 / pre Screen_BackgroundImgDraw\n"); fflush(g_ShDebugLog); } }
#endif
    Screen_BackgroundImgDraw(&g_MainImg0);
#ifdef SH_PC_PORT
    { extern FILE* g_ShDebugLog; if (g_ShDebugLog) { fprintf(g_ShDebugLog, "[BOOT] post Screen_BackgroundImgDraw / pre func_80089090\n"); fflush(g_ShDebugLog); } }
#endif
    func_80089090(1);
#ifdef SH_PC_PORT
    { extern FILE* g_ShDebugLog; if (g_ShDebugLog) { fprintf(g_ShDebugLog, "[BOOT] post func_80089090 — frame done\n"); fflush(g_ShDebugLog); } }
#endif
}

#ifdef SH_PC_PORT
/* Sentinel scan helper — walks an OT chain looking for the corrupt-addr
 * fingerprint that's been plaguing the muzzle flash codepath: a prim's
 * `addr` field points OUTSIDE both the packet buffer and the OT bucket
 * array (and isn't the natural &prim_terminator). The bug class is a
 * 32→64-bit pointer truncation hidden somewhere in the spawn/update
 * dispatch we haven't been able to spot via grep.
 *
 * Strategy: call this at multiple checkpoints across the frame. The
 * FIRST checkpoint that detects corruption brackets the writer to the
 * subsystem that ran since the previous clean checkpoint.
 *
 * Logs at most once per (phase × ot) per session so the log doesn't
 * flood; switch phases by adding a new label string, no de-dup overhead.
 *
 * Safe to call on any frame — the existing OT0/OT2 sanitizer at
 * post-GsSortClear will still terminate the chain so we never crash. */
extern OT_TAG prim_terminator;
/* Provided by libgs_stub.c — gives the bounds of any subroot OT registered
 * via GsSortOt(subroot, root). The pickup-screen pipeline registers
 * g_OrderingTable1 as a subroot of g_OrderingTable0; its bucket nodes
 * live in storage outside our packet-buffer + OT0-array windows and
 * MUST be accepted as valid chain pointers, otherwise the sanitizer
 * truncates the chain mid-walk and the picked-up item disappears. */
extern void GsSortOt_GetSubrootBounds(uintptr_t* lo, uintptr_t* hi);

void Pc_OtSentinelScan(GsOT* ot, const char* phase, const char* otName)
{
    if (!ot || !ot->tag) return;

    uintptr_t pktLo  = (uintptr_t)s_PcPacketBufs[g_ActiveBufferIdx];
    uintptr_t pktHi  = (uintptr_t)s_PcPacketBufEnds[g_ActiveBufferIdx];
    uintptr_t otLo   = (uintptr_t)ot->org;
    int       otLen  = (ot->length > 0 && ot->length <= 16) ? (int)ot->length : 0;
    size_t    otCnt  = (size_t)1 << otLen;
    uintptr_t otHi   = (otLo && otLen) ? (otLo + otCnt * sizeof(GsOT_TAG)) : otLo;
    uintptr_t termA  = (uintptr_t)&prim_terminator;
    uintptr_t subLo  = 0, subHi = 0;
    GsSortOt_GetSubrootBounds(&subLo, &subHi);

    OT_TAG* prev = NULL;
    OT_TAG* cur  = (OT_TAG*)ot->tag;
    int hops = 0;

    while (cur && hops < 16384)
    {
        uintptr_t curA = (uintptr_t)cur;
        int valid = (curA == termA) ||
                    (curA >= pktLo && curA < pktHi) ||
                    (curA >= otLo  && curA < otHi) ||
                    (subLo && curA >= subLo && curA < subHi);
        if (!valid)
        {
            /* Found the corruption boundary. prev is the LAST valid prim;
             * its `addr` field was clobbered to point at `cur` (wild).
             * Log once per phase × OT name pair. */
            static const void* s_seenPhase[16] = {0};
            static const void* s_seenOt[16]    = {0};
            static int         s_seenCount     = 0;
            int seen = 0;
            for (int i = 0; i < s_seenCount; i++)
                if (s_seenPhase[i] == phase && s_seenOt[i] == otName) { seen = 1; break; }
            if (!seen && s_seenCount < 16)
            {
                s_seenPhase[s_seenCount] = phase;
                s_seenOt[s_seenCount]    = otName;
                s_seenCount++;
                SH_DBG("[OT-SCAN] %s/%s: CORRUPT addr field — prev=%p next=%p hops=%d (pkt=[%p..%p) ot=[%p..%p))",
                       phase, otName, (void*)prev, (void*)cur, hops,
                       (void*)pktLo, (void*)pktHi, (void*)otLo, (void*)otHi);
                if (prev != NULL)
                {
                    u32* w = (u32*)prev;
                    SH_DBG("[OT-SCAN]   prev raw bytes: %08x %08x %08x %08x  %08x %08x %08x %08x",
                           w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
                    SH_DBG("[OT-SCAN]   prev raw bytes: %08x %08x %08x %08x  %08x %08x %08x %08x",
                           w[8], w[9], w[10], w[11], w[12], w[13], w[14], w[15]);
                }
            }
            return;
        }
        if (curA == termA) return; /* clean end of chain */
        prev = cur;
        cur  = (OT_TAG*)nextPrim(cur);
        hops++;
    }
}

#define PC_OT_SCAN(phase) do { \
    if (g_GameWork.gameState == GameState_InGame) { \
        Pc_OtSentinelScan(&g_OrderingTable0[g_ActiveBufferIdx], phase, "OT0"); \
        Pc_OtSentinelScan(&g_OrderingTable2[g_ActiveBufferIdx], phase, "OT2"); \
    } \
} while (0)
#else
#define PC_OT_SCAN(phase) ((void)0)
#endif

void MainLoop(void) // 0x80032EE0
{
    #define TICKS_PER_SECOND_MIN (TICKS_PER_SECOND / 4)
    #define H_BLANKS_PER_SECOND  15780
    #define H_BLANKS_PER_TICK    (H_BLANKS_PER_SECOND / TICKS_PER_SECOND) // 263

    #define H_BLANKS_TO_SEC_CONVERSION_FACTOR ((float)Q12(1.0f) / (float)H_BLANKS_PER_SECOND)             // 0.25956907477f
    #define H_BLANKS_PER_FRAME_MIN            (H_BLANKS_PER_SECOND / TICKS_PER_SECOND_MIN)                // 1052
    #define H_BLANKS_Q12_TO_SEC_SCALE         (s32)(H_BLANKS_TO_SEC_CONVERSION_FACTOR * (float)Q12(1.0f)) // 1063
    #define H_BLANKS_GRAVITY_SCALE            Q12(9.8f * H_BLANKS_TO_SEC_CONVERSION_FACTOR)               // 10419
    #define V_BLANKS_MAX                      4

    s32 vBlanks;
    s32 vCount;
    s32 vCountCopy;
    s32 interval;

    // Initialize engine.
    GsInitVcount();
    MemCard_SysInit();
    MemCard_SysInit2();
    MemCard_InitStatus();
    Joy_Init();
    VSyncCallback(&Screen_VSyncCallback);

    // NTSC-J moves these calls into the `HP_SAFE1`/`S__SAFE2` anti-modchip overlays,
    // likely to make sure those overlays wouldn't be patched out by pirates.
#if !VERSION_REGION_IS(NTSCJ)
    InitGeom();
    ItemScreen_TmdGsFCallInit();
#ifdef SH_PC_PORT
    /* Skip vibration init - requires PadInfoMode which may crash without real pad */
#else
    func_800890B8();
#endif
#endif

    SD_Init();

#ifdef SH_PC_PORT
    /* SD_Init -> SdInit -> SpuInit -> ResetCallback clears the VSync callback.
     * Re-register it after SD_Init to ensure the callback stays active. */
    VSyncCallback(&Screen_VSyncCallback);

    /* PC graphic-content warning. Runs after all subsystem inits
     * (GsInitVcount, MemCard, Joy, InitGeom, SD_Init) so it shares
     * the same boot pipeline as Konami/KCET — no perceptible delay
     * between warning and the next state. Hor+ OFF so the PsyCross
     * 2D ortho is (0, disp.w, disp.h, 0) — fb 0..640 maps to the
     * full window with no margin, and our quads at fb 0..640
     * stretch edge-to-edge. With hor+ ON the ortho would expand to
     * [-margin, disp.w+margin], leaving the quads inside the inner
     * 4:3 portion (white margin visible on the sides). Per-frame
     * gate at line 1371 reasserts the InGame-only rule once the
     * main loop starts. */
    {
        extern int g_PcHorPlusEnabled;
        extern void Pc_PlayWarningScreen(void);
        const int prevHor = g_PcHorPlusEnabled;
        g_PcHorPlusEnabled = 0;
        Pc_PlayWarningScreen();
        g_PcHorPlusEnabled = prevHor;
    }
#endif
    // Run game.
    while (true)
    {
        g_TickCount++;

#ifdef SH_PC_PORT
        { extern FILE* g_ShDebugLog; if (g_ShDebugLog) { fprintf(g_ShDebugLog, "[MAIN] === iteration tick=%u start ===\n", (unsigned)g_TickCount); fflush(g_ShDebugLog); } }
        /* PsyCross requires explicit input polling — on PSX this happens
         * via hardware interrupt during VBlank. */
        PsyX_UpdateInput();
        { extern FILE* g_ShDebugLog; if (g_ShDebugLog) { fprintf(g_ShDebugLog, "[MAIN] post PsyX_UpdateInput\n"); fflush(g_ShDebugLog); } }
        DebugConsole_Update();
        { extern FILE* g_ShDebugLog; if (g_ShDebugLog) { fprintf(g_ShDebugLog, "[MAIN] post DebugConsole_Update\n"); fflush(g_ShDebugLog); } }
#endif
        // Update input.
        Joy_ReadP1();
#ifdef SH_PC_PORT
        { extern FILE* g_ShDebugLog; if (g_ShDebugLog) { fprintf(g_ShDebugLog, "[MAIN] post Joy_ReadP1\n"); fflush(g_ShDebugLog); } }
#endif
        Demo_ControllerDataUpdate();
#ifdef SH_PC_PORT
        { extern FILE* g_ShDebugLog; if (g_ShDebugLog) { fprintf(g_ShDebugLog, "[MAIN] post Demo_ControllerDataUpdate\n"); fflush(g_ShDebugLog); } }
#endif
        Joy_ControllerDataUpdate();
#ifdef SH_PC_PORT
        { extern FILE* g_ShDebugLog; if (g_ShDebugLog) { fprintf(g_ShDebugLog, "[MAIN] post Joy_ControllerDataUpdate\n"); fflush(g_ShDebugLog); } }
#endif

        if (MainLoop_ShouldWarmReset() == 2)
        {
            Game_WarmBoot();
            continue;
        }

        g_ActiveBufferIdx = GsGetActiveBuff();

#ifdef SH_PC_PORT
        /* PC primitives are larger than PSX (8-byte pointers, bigger structs).
         * The original 128KB packet buffer overflows when rendering 2+ characters.
         * Allocate 512KB per buffer from heap instead of fixed PSX temp memory.
         * Extra 64 bytes of canary at the end for corruption detection. */
        if (!s_PcPacketBufs[0]) {
            s_PcPacketBufs[0] = (PACKET*)calloc(1, PC_PKTBUF_SIZE + PC_CANARY_SIZE);
            s_PcPacketBufs[1] = (PACKET*)calloc(1, PC_PKTBUF_SIZE + PC_CANARY_SIZE);
            s_PcPacketBufEnds[0] = s_PcPacketBufs[0] + PC_PKTBUF_SIZE;
            s_PcPacketBufEnds[1] = s_PcPacketBufs[1] + PC_PKTBUF_SIZE;
            memset(s_PcPacketBufEnds[0], PC_CANARY_VAL, PC_CANARY_SIZE);
            memset(s_PcPacketBufEnds[1], PC_CANARY_VAL, PC_CANARY_SIZE);
        }
        GsOUT_PACKET_P = s_PcPacketBufs[g_ActiveBufferIdx];
#else
        if (g_GameWork.gameState == GameState_MainLoadScreen ||
            g_GameWork.gameState == GameState_InGame)
        {
            GsOUT_PACKET_P = (PACKET*)(TEMP_MEMORY_ADDR + (g_ActiveBufferIdx << 17));
        }
        else if (g_GameWork.gameState == GameState_InventoryScreen)
        {
            GsOUT_PACKET_P = (PACKET*)(TEMP_MEMORY_ADDR + (g_ActiveBufferIdx * 40000));
        }
        else
        {
            GsOUT_PACKET_P = (PACKET*)(TEMP_MEMORY_ADDR + (g_ActiveBufferIdx << 15));
        }
#endif

#ifdef SH_PC_PORT
        { extern FILE* g_ShDebugLog; if (g_ShDebugLog) { fprintf(g_ShDebugLog, "[MAIN] pre GsClearOt OT0/OT2\n"); fflush(g_ShDebugLog); } }
#endif
        GsClearOt(0, 0, &g_OrderingTable0[g_ActiveBufferIdx]);
        GsClearOt(0, 0, &g_OrderingTable2[g_ActiveBufferIdx]);
#ifdef SH_PC_PORT
        { extern FILE* g_ShDebugLog; if (g_ShDebugLog) { fprintf(g_ShDebugLog, "[MAIN] post GsClearOt\n"); fflush(g_ShDebugLog); } }
#endif

        g_SysWork.bgmStatusFlags = BgmStatusFlag_None;

#ifdef SH_PC_PORT
        { extern FILE* g_ShDebugLog; if (g_ShDebugLog) { fprintf(g_ShDebugLog, "[MAIN] tick=%u gameState=%d pre GameStateUpdate\n", (unsigned)g_TickCount, (int)g_GameWork.gameState); fflush(g_ShDebugLog); } }
#endif
        PC_OT_SCAN("pre-GameStateUpdate");
        // Call update function for current GameState.
        g_GameStateUpdateFuncs[g_GameWork.gameState]();
#ifdef SH_PC_PORT
        { extern FILE* g_ShDebugLog; if (g_ShDebugLog) { fprintf(g_ShDebugLog, "[MAIN] post GameStateUpdate\n"); fflush(g_ShDebugLog); } }
#endif
        PC_OT_SCAN("post-GameStateUpdate");
#ifdef SH_PC_PORT
        if (g_GameWork.gameState == GameState_InGame) {
            /* Canary checks after InGame state update */
            /* --- Canary checks after game state update --- */
            {
                PACKET* pktEnd0 = s_PcPacketBufEnds[0];
                PACKET* pktEnd1 = s_PcPacketBufEnds[1];
                PACKET* pktStart = s_PcPacketBufs[g_ActiveBufferIdx];
                ptrdiff_t pktUsed = GsOUT_PACKET_P - pktStart;
                int canaryOk = 1;
                int i;
                for (i = 0; i < PC_CANARY_SIZE; i++) {
                    if (pktEnd0[i] != PC_CANARY_VAL) { canaryOk = 0; break; }
                }
                if (!canaryOk) {
                    SH_DBG("[CANARY] *** PACKET BUF 0 OVERFLOW! byte %d changed to 0x%02X (used=%td/%d)", i, (unsigned char)pktEnd0[i], pktUsed, PC_PKTBUF_SIZE);
                }
                canaryOk = 1;
                for (i = 0; i < PC_CANARY_SIZE; i++) {
                    if (pktEnd1[i] != PC_CANARY_VAL) { canaryOk = 0; break; }
                }
                if (!canaryOk) {
                    SH_DBG("[CANARY] *** PACKET BUF 1 OVERFLOW! byte %d changed to 0x%02X (used=%td/%d)", i, (unsigned char)pktEnd1[i], pktUsed, PC_PKTBUF_SIZE);
                }
                SH_DBG("[PKTBUF] used=%td/%d (%.1f%%)", pktUsed, PC_PKTBUF_SIZE, (double)pktUsed * 100.0 / PC_PKTBUF_SIZE);
            }
        }
#endif

#ifdef SH_PC_PORT
        { extern FILE* g_ShDebugLog; if (g_ShDebugLog) { fprintf(g_ShDebugLog, "[MAIN] pre Demo_Update\n"); fflush(g_ShDebugLog); } }
#endif
        Demo_Update();
        PC_OT_SCAN("post-Demo_Update");
#ifdef SH_PC_PORT
        { extern FILE* g_ShDebugLog; if (g_ShDebugLog) { fprintf(g_ShDebugLog, "[MAIN] pre Demo_GameRandSeedSet\n"); fflush(g_ShDebugLog); } }
#endif
        Demo_GameRandSeedSet();
#ifdef SH_PC_PORT
        { extern FILE* g_ShDebugLog; if (g_ShDebugLog) { fprintf(g_ShDebugLog, "[MAIN] pre WarmReset check\n"); fflush(g_ShDebugLog); } }
#endif

        if (MainLoop_ShouldWarmReset() == 2)
        {
            Game_WarmBoot();
            continue;
        }

#ifdef SH_PC_PORT
        /* g_SH_PostFireTrace is bumped to N in Player_CombatUpdate when a
         * fire dispatch happens. ML_TRACE prints for those frames AND for
         * any frame where g_SH_AlwaysMlTrace is set (currently always-on
         * to diagnose the post-pistol-equip silent crash AND a boot-time
         * silent crash that dies after frame 1's GameState_Boot_Update
         * completes — so flush per call so the trace survives the crash). */
        extern int g_SH_PostFireTrace;
        extern int g_SH_AlwaysMlTrace;
#define ML_TRACE(tag) do { \
    if (g_SH_PostFireTrace > 0 || g_SH_AlwaysMlTrace) SH_DBG_ECHO("[ML] " tag); \
} while (0)
#else
#define ML_TRACE(tag) ((void)0)
#endif
        PC_OT_SCAN("pre-Screen_FadeUpdate");
        ML_TRACE("Screen_FadeUpdate");
        Screen_FadeUpdate();
        PC_OT_SCAN("post-Screen_FadeUpdate");
        ML_TRACE("MemCard_Update");
        MemCard_Update();
        ML_TRACE("Sd_TaskPoolExecute");
        Sd_TaskPoolExecute();
#ifdef SH_PC_PORT
        /* PC-only: drain queued loader task in one frame instead of one
         * sub-state per frame (kills the 15s warning→Konami pause).
         * Helper lives in sd_call.c because g_Sd_TaskPool /
         * g_Sd_AudioStreamingStates are static there. */
        Sd_TaskPoolDrain();
        XaPlayer_Update();
#endif

#ifdef SH_PC_PORT
        /* PSX gates Fs_QueueUpdate on audio streaming because the disc
         * laser can only do one thing at a time — reading file data and
         * streaming XA from CD share the same hardware. On PC the
         * "disc" is a regular file (and XA streams from separate .XA
         * files via xa_player.c), so reads and audio operations don't
         * compete. The gate is not just unnecessary on PC, it's
         * actively harmful: any audio-task-pool stall (a CD command not
         * yet stubbed in PsyCross, a state-machine bug) blocks the
         * file queue, which in turn blocks inventory opening
         * (Fs_QueueChunksLoad never returns true), room transitions,
         * and DMS chunk loads. Tick the queue every frame on PC. */
        ML_TRACE("Fs_QueueUpdate");
        Fs_QueueUpdate();
#else
        if (!Sd_AudioStreamingCheck())
        {
            ML_TRACE("Fs_QueueUpdate");
            Fs_QueueUpdate();
        }
#endif

        PC_OT_SCAN("post-Fs_QueueUpdate");
        ML_TRACE("func_80089128");
        func_80089128();
        PC_OT_SCAN("post-func_80089128");
        ML_TRACE("func_8008D78C");
        func_8008D78C(); // Camera update?
        PC_OT_SCAN("post-func_8008D78C");
        ML_TRACE("DrawSync");
        DrawSync(SyncMode_Wait);
        ML_TRACE("VSync-begin");
        // Handle V sync.
        if (g_SysWork.flags_22A4 & UnkSysFlag_1)
        {
            ML_TRACE("VSync-flag2_1-branch");
            vBlanks   = VSync(SyncMode_Count);
            g_VBlanks = vBlanks - g_PrevVBlanks;

            Demo_PresentIntervalUpdate();

            interval      = g_Demo_VideoPresentInterval;
            g_PrevVBlanks = vBlanks;

            if (interval < g_IntervalVBlanks)
            {
                interval = g_IntervalVBlanks;
            }

            do
            {
                VSync(SyncMode_Wait);
                g_VBlanks++;
                g_PrevVBlanks++;
            }
            while (g_VBlanks < interval);

            g_UncappedVBlanks = g_VBlanks;
            g_VBlanks         = MIN(g_VBlanks, 4);

            vCount     = g_Demo_VideoPresentInterval * H_BLANKS_PER_TICK;
            vCountCopy = g_UncappedVBlanks * H_BLANKS_PER_TICK;
            g_VBlanks  = g_Demo_VideoPresentInterval;
        }
        else
        {
            if (g_SysWork.sysState != SysState_Gameplay)
            {
                ML_TRACE("VSync-nonGameplay");
                g_VBlanks     = VSync(SyncMode_Count) - g_PrevVBlanks;
                g_PrevVBlanks = VSync(SyncMode_Count);
                ML_TRACE("VSync-Wait");
                VSync(SyncMode_Wait);
                ML_TRACE("VSync-Wait-done");
            }
            else
            {
                ML_TRACE("VSync-gameplay");
                if (!ScreenFade_IsNone())
                {
                    VSync(SyncMode_Wait);
                }

                g_VBlanks     = VSync(SyncMode_Count) - g_PrevVBlanks;
                g_PrevVBlanks = VSync(SyncMode_Count);

#ifdef SH_PC_PORT
                /* Compute effective vblank interval from fps_cap config and debug toggle.
                 * Only override the game's own g_IntervalVBlanks when fully in gameplay —
                 * menu sub-states (map, items, pause text) set g_IntervalVBlanks=1 themselves
                 * and must not be overridden, or they drop to 30fps while overlays are open.
                 *
                 * Priority: vsync+refresh_rate > debug unlock > fps_cap > game default.
                 * VBlank timer ticks at 60Hz; for <= 60fps use vblank waits (effectiveMin).
                 * For > 60fps (120, 240) use SDL high-precision timer since vblank
                 * granularity is 16.67ms and can't express sub-frame intervals. */
                {
                    static Uint64 s_lastFrameTime = 0;
                    int effectiveMin = g_IntervalVBlanks;
                    if (g_GameWork.gameState == GameState_InGame)
                    {
                        int effectiveFps;

                        /* vsync + explicit refresh rate: let the display pacing be the cap */
                        if (g_PcConfig.vsync != 0 && g_PcConfig.refreshRate > 0)
                            effectiveFps = g_PcConfig.refreshRate;
                        else if (g_DebugUnlockFps || g_PcConfig.fpsCap == 0)
                            effectiveFps = 0; /* uncapped */
                        else
                            effectiveFps = g_PcConfig.fpsCap;

                        if (effectiveFps <= 0)
                        {
                            effectiveMin = 0; /* uncapped: don't wait */
                        }
                        else if (effectiveFps > 60)
                        {
                            /* High fps (120, 240): SDL timer — vblank loop can't express <16ms */
                            Uint64 freq = SDL_GetPerformanceFrequency();
                            Uint64 targetTicks = freq / (Uint64)effectiveFps;
                            if (s_lastFrameTime == 0)
                                s_lastFrameTime = SDL_GetPerformanceCounter();
                            Uint64 elapsed = SDL_GetPerformanceCounter() - s_lastFrameTime;
                            if (elapsed < targetTicks)
                            {
                                Uint64 remainMs = ((targetTicks - elapsed) * 1000) / freq;
                                if (remainMs > 2) SDL_Delay((Uint32)(remainMs - 1));
                                while (SDL_GetPerformanceCounter() - s_lastFrameTime < targetTicks) {}
                            }
                            s_lastFrameTime = SDL_GetPerformanceCounter();
                            effectiveMin = 0; /* SDL timing handled it */
                        }
                        else if (effectiveFps >= 60)
                            effectiveMin = 1; /* 60fps: 1 vblank per frame */
                        else
                            effectiveMin = 60 / effectiveFps; /* e.g. 30→2, 20→3 */
                    }

                    if (effectiveMin > 0 || s_lastFrameTime == 0)
                        s_lastFrameTime = 0; /* reset SDL timer when not in use */

                    while (g_VBlanks < effectiveMin)
                    {
                        VSync(SyncMode_Wait);
                        g_VBlanks++;
                        g_PrevVBlanks++;
                    }
                }
#else
                while (g_VBlanks < g_IntervalVBlanks)
                {
                    VSync(SyncMode_Wait);
                    g_VBlanks++;
                    g_PrevVBlanks++;
                }
#endif
            }

            // Update V blanks.
            g_UncappedVBlanks = g_VBlanks;
            g_VBlanks         = MIN(g_VBlanks, V_BLANKS_MAX);

            // Update V count.
            vCount     = MIN(GsGetVcount(), H_BLANKS_PER_FRAME_MIN); // NOTE: Will call `GsGetVcount` twice.
            vCountCopy = vCount;
        }

        ML_TRACE("deltaTime");
        // Update delta time.
        g_DeltaTime    = Q12_MULT(vCount, H_BLANKS_Q12_TO_SEC_SCALE);
        g_DeltaTimeRaw = Q12_MULT(vCountCopy, H_BLANKS_Q12_TO_SEC_SCALE);
        g_GravitySpeed = Q12_MULT(vCount, H_BLANKS_GRAVITY_SCALE);
        GsClearVcount();

        ML_TRACE("GsSwapDispBuff");
        // Draw objects?
        GsSwapDispBuff();
        ML_TRACE("post-GsSwapDispBuff");
#ifdef SH_PC_PORT
        /* Numpad .: toggle fog on/off (only active during debug camera) */
        if (g_sdlKeyboardState && g_GameWork.gameState == 11) {
            int cur = g_sdlKeyboardState[SDL_SCANCODE_KP_PERIOD];
            if (cur && !g_DebugFogTogglePrev && g_DebugCamEnabled) {
                g_DebugFogDisabled = !g_DebugFogDisabled;
                SH_DBG("[DEBUG] Fog: %s", g_DebugFogDisabled ? "OFF" : "ON");
            }
            g_DebugFogTogglePrev = cur;

            /* When debug camera is off, fog is always normal */
            if (!g_DebugCamEnabled) {
                g_DebugFogDisabled = 0;
            }

            if (g_DebugFogDisabled) {
                PC_WorldEnvWork.isFogEnabled_1 = 0;
            }
        }

        /* Enable hor+ widescreen only during 3D world states; 2D UI screens
         * (menus, loading screen, memory card warning, etc.) use 4:3 ortho.
         *
         * Map pickup specifically: the paper-map pickup screen renders 2D
         * full-screen SPRTs that need 4:3 ortho. Item-pickup dialogs
         * (`do you want to pick up X?`) DO NOT need this — they want 16:9
         * to match the surrounding gameplay aspect.
         *
         * Detection: PaperMap_ReuploadTimToVram_PC is the only place that
         * sets g_PsxSkipFramebufferStore, and it runs every tick during
         * the map pickup screen. Use that flag as the "this is a map
         * pickup tick" signal — it's already set by game logic before
         * we get here in MainLoop. (auto-clears in PsyX_EndScene.)
         *
         * Result: item pickups stay 16:9, map pickup goes to 4:3. */
        g_PcHorPlusEnabled = (g_GameWork.gameState == GameState_InGame &&
                              !g_PsxSkipFramebufferStore) ? 1 : 0;

        /* Override background color with fog color during InGame.
         * fog params are set by Gfx_FlashlightUpdate from the previous frame's
         * update, so they're valid by frame 2+. Use the normal GsSortClear path
         * which PsyCross handles via activeDrawEnv.isbg in PsyX_BeginScene. */
        if (g_GameWork.gameState == 11 && PC_WorldEnvWork.isFogEnabled_1) {
            g_GameWork.background2dColor.r = PC_WorldEnvWork.fogColor_1C.r;
            g_GameWork.background2dColor.g = PC_WorldEnvWork.fogColor_1C.g;
            g_GameWork.background2dColor.b = PC_WorldEnvWork.fogColor_1C.b;
            g_PsyX_FogColor[0] = PC_WorldEnvWork.fogColor_1C.r / 255.0f;
            g_PsyX_FogColor[1] = PC_WorldEnvWork.fogColor_1C.g / 255.0f;
            g_PsyX_FogColor[2] = PC_WorldEnvWork.fogColor_1C.b / 255.0f;
        }
#endif
        ML_TRACE("GsSortClear");
#ifdef SH_PC_PORT
        /* Stack canary — detect if anything corrupted our stack frame */
        {
            volatile u32 _stackCanary = 0xDEADBEEF;
#endif
        GsSortClear(g_GameWork.background2dColor.r, g_GameWork.background2dColor.g, g_GameWork.background2dColor.b, &g_OrderingTable0[g_ActiveBufferIdx]);
        ML_TRACE("post-GsSortClear");
#ifdef SH_PC_PORT
        if (g_GameWork.gameState == 11) {
            /* Sanitize InGame OT0 — only allow known-safe rendering primitives.
             * Strip DR_MODE (0xE0) which crashes PsyCross ProcessDrawEnv,
             * lines (0x40/0x50), and any unknown types. Texture page info is
             * embedded in POLY_FT/GT prims so DR_MODE isn't needed for textures.
             *
             * Defensive: validate cur pointer BEFORE dereferencing on each
             * iteration. Particle spawning (handgun muzzle flash, same class
             * as the knife OT corruption family) can write garbage nextPtrs
             * into OT entries, and the post-GsSortClear walker hits one,
             * dereferences cur->code or cur->tag, INVALID_POINTER_READ in
             * MainLoop directly. The earlier next-pointer bounds check
             * caught some cases but not e.g. addresses that are in valid
             * user-space range but not actually prim memory. Validate cur
             * is in our packet buffer OR the OT array before reading it. */
            GsOT* ot0 = &g_OrderingTable0[g_ActiveBufferIdx];
            {
                OT_TAG* cur = (OT_TAG*)ot0->tag;
                int w2 = 0;
                /* Compute valid pointer ranges once outside the loop.
                 * GsOT.length is LOG2 of the entry count (PsyCross
                 * GsClearOt does `n = 1 << ot->length`), so for the
                 * 2048-entry OT, length == 11 and the array spans
                 * 2048 * sizeof(GsOT_TAG) = 24 KB. Earlier I computed
                 * length * sizeof which gave only 132 bytes and made
                 * the sanitizer reject every legitimate entry past the
                 * first few — manifesting as constant [OT-SANIT] spam
                 * during pistol fire (5000+ aborts in a single test
                 * session) without actually catching the corruption. */
                uintptr_t pktLo  = (uintptr_t)s_PcPacketBufs[g_ActiveBufferIdx];
                uintptr_t pktHi  = (uintptr_t)s_PcPacketBufEnds[g_ActiveBufferIdx];
                uintptr_t otLo   = (uintptr_t)ot0->org;
                /* Clamp length to a sane range. Real OT length is 11
                 * (2048 entries); accept up to 16 (65536 entries). If
                 * GsOT itself was clobbered to a huge length the shift
                 * would overflow size_t and the bounds window becomes
                 * "anything", letting wild pointers slip through. */
                int       otLen  = (ot0->length > 0 && ot0->length <= 16) ? (int)ot0->length : 0;
                size_t    otCnt  = (size_t)1 << otLen;
                uintptr_t otHi   = (otLo && otLen) ? (otLo + otCnt * sizeof(GsOT_TAG)) : otLo;
                /* Subroot OT bounds (set by GsSortOt in libgs_stub.c).
                 * Pickup screen registers g_OrderingTable1 as a subroot
                 * of OT0; OT1's bucket nodes need to be accepted as
                 * valid chain pointers, otherwise the truncate-on-bad-
                 * cur path here erases the picked-up item. */
                uintptr_t subLo = 0, subHi = 0;
                GsSortOt_GetSubrootBounds(&subLo, &subHi);
                /* CRITICAL: validate `cur` BEFORE any field access — including
                 * the loop condition's isendprim(cur), which reads cur->addr
                 * (a 64-bit pointer load). If ot0->tag itself was corrupted
                 * to a wild-but-non-NULL pointer, the very first
                 * `!isendprim(cur)` faults with INVALID_POINTER_READ
                 * (`mov rdx,[rax]`) before our in-loop curOk check ever
                 * runs. Pre-validate, and re-check at top of every
                 * iteration. */
                /* Track prev so we can TRUNCATE the chain when corruption is
                 * found. Detection alone isn't enough — DrawOTag walks the
                 * same chain after the sanitizer returns and faults on the
                 * same wild pointer. We need to splice the chain to end at
                 * the last known-good entry. */
                OT_TAG* prev = NULL;
                /* Item-pickup TMD diagnostic: when a pickup is in flight
                 * (g_PsxSkipFramebufferStore is set during pickup states),
                 * dump every prim the OT0 walker visits with code/len/bucket.
                 * Pair with [TMDPRIM] logs in libgs_stub.c — if a TMD prim is
                 * emitted into a bucket but the walker never visits it, we
                 * know GsSortOt isn't anchoring the OT1 subroot back into
                 * the OT0 chain. Capped to a single pickup-pass to keep log
                 * readable. */
                int pmapTrace = 0;
                static int s_pmapTraceUsed = 0;
                extern int g_PsxSkipFramebufferStore;
                if (g_PsxSkipFramebufferStore && !s_pmapTraceUsed) {
                    pmapTrace = 1;
                    s_pmapTraceUsed = 1;
                    SH_DBG("[OT-WALK/PICKUP] starting trace — head=%p pkt=[%p..%p) ot=[%p..%p) sub=[%p..%p)",
                           (void*)cur, (void*)pktLo, (void*)pktHi,
                           (void*)otLo, (void*)otHi,
                           (void*)subLo, (void*)subHi);
                }
                while (cur && w2 < 8192) {
                    uintptr_t curAddr = (uintptr_t)cur;
                    int curOk = ((curAddr >= pktLo && curAddr < pktHi) ||
                                 (curAddr >= otLo  && curAddr < otHi)  ||
                                 (subLo && curAddr >= subLo && curAddr < subHi));
                    if (pmapTrace && w2 < 200) {
                        u8 dbgCode = curOk ? ((P_TAG*)cur)->code : 0xFF;
                        int dbgLen = curOk ? getlen(cur) : -1;
                        SH_DBG("[OT-WALK/PICKUP] w=%d cur=%p ok=%d code=0x%02x len=%d isend=%d",
                               w2, (void*)cur, curOk, dbgCode, dbgLen,
                               curOk ? isendprim(cur) : -1);
                    }
                    if (!curOk) {
                        static int s_dumpedOnce = 0;
                        if (!s_dumpedOnce) {
                            s_dumpedOnce = 1;
                            SH_DBG("[OT-SANIT] FIRST bad cur=%p (w2=%d) prev=%p — pkt=[%p..%p) ot=[%p..%p)",
                                   (void*)cur, w2, (void*)prev,
                                   (void*)pktLo, (void*)pktHi,
                                   (void*)otLo, (void*)otHi);
                        }
                        /* Skip past the corrupt prim by re-linking prev to
                         * ot0->org[0] — the closest-to-camera bucket, last in
                         * draw order. The OT chain walks from far→near
                         * (org[N-1] → ... → org[0] → terminator). When we
                         * detect corruption mid-chain, jumping to org[0]
                         * preserves the nearest-camera geometry (which is
                         * what the user notices missing — items near Harry
                         * during muzzle flash). The middle buckets are still
                         * lost, but a small fraction vs. truncating-at-prev. */
                        if (prev != NULL) {
                            setaddr(prev, &ot0->org[0]);
                        } else {
                            ot0->tag = (u_long*)&ot0->org[0];
                        }
                        break;
                    }
                    if (isendprim(cur)) break;
                    int len = getlen(cur);
                    if (len > 0) {
                        u8 hi = ((P_TAG*)cur)->code & 0xF0;
                        u8 codeFull = ((P_TAG*)cur)->code;
                        /* DR_TPAGE (0xE1) is needed in OT0 for the paper-map
                         * pickup screen and similar background-image draws —
                         * Screen_BackgroundImgDraw aliases the OT0 far bucket
                         * and prepends SPRT + DR_TPAGE pairs. Without DR_TPAGE
                         * passing through, the SPRTs sample whatever tpage was
                         * last set (typically the gameplay framebuffer region)
                         * and the pickup screen renders as tiled gameplay-scene
                         * garbage. Other 0xE_ codes (DR_MODE multi-byte family)
                         * remain stripped. */
                        if (len > 32 || (hi != 0x00 && hi != 0x20 && hi != 0x30 &&
                            hi != 0x60 && hi != 0x70 && hi != 0xA0 &&
                            codeFull != 0xE1)) {
                            setlen(cur, 0);
                        }
                    }
                    OT_TAG* next = (OT_TAG*)nextPrim(cur);
                    /* Guard against wild next pointers. Truncate at cur — make
                     * it the new chain terminator instead of just zeroing its
                     * length (the old behaviour left cur->addr pointing at the
                     * wild target, so DrawOTag would still chase it). */
                    if (next && ((uintptr_t)next < 0x1000 || (uintptr_t)next > (uintptr_t)0x7FFFFFFFFFFF)) {
                        static int s_badNextDumped = 0;
                        if (!s_badNextDumped) {
                            s_badNextDumped = 1;
                            SH_DBG("[OT-SANIT] FIRST wild next=%p at cur=%p (w2=%d) — re-link to org[0]",
                                   (void*)next, (void*)cur, w2);
                        }
                        /* Re-link cur past the corrupt next to ot0->org[0]
                         * (closest-camera bucket) so DrawOTag walks through
                         * the nearest geometry instead of the wild pointer. */
                        setaddr(cur, &ot0->org[0]);
                        break;
                    }
                    prev = cur;
                    cur  = next;
                    w2++;
                }
            }
        }

#endif
        ML_TRACE("OT0-draw");
#ifdef SH_PC_PORT
        /* Pre-draw canary check: detect if corruption happened during OT build */
        if (g_GameWork.gameState == GameState_InGame) {
            int _ci; int _canaryOk = 1;
            for (_ci = 0; _ci < PC_CANARY_SIZE; _ci++) {
                if (s_PcPacketBufEnds[g_ActiveBufferIdx][_ci] != PC_CANARY_VAL) { _canaryOk = 0; break; }
            }
            if (!_canaryOk) {
                SH_DBG("[CANARY] *** PRE-DRAW: buf %d overflow at byte %d (0x%02X)", g_ActiveBufferIdx, _ci, (unsigned char)s_PcPacketBufEnds[g_ActiveBufferIdx][_ci]);
            }
        }
#endif
        GsDrawOt(&g_OrderingTable0[g_ActiveBufferIdx]);
        ML_TRACE("OT0-done");
#ifdef SH_PC_PORT
        /* Sanitize InGame OT2 — extended whitelist for 2D overlays.
         * OT2 holds text, screen fade, cutscene borders via g_OtTags0 layers.
         * Text uses SPRT (0x64) + DR_TPAGE (0xE1) per glyph, so allow 0xE0
         * range here (DR_TPAGE is safe; the DR_MODE crashes are in OT0).
         *
         * Defense in depth: same `cur` bounds-validation as the OT0
         * sanitizer above. The OT2 walk uses the SAME isendprim/nextPrim
         * dereference pattern, so a wild pointer in ot2->tag or chained
         * via nextPrim faults identically (`mov rdx,[rax]`) on the
         * loop-condition read. Pre-check before any field access, and
         * re-check after each nextPrim hop. */
        if (g_GameWork.gameState == 11) {
            GsOT* ot2 = &g_OrderingTable2[g_ActiveBufferIdx];
            OT_TAG* cur2 = (OT_TAG*)ot2->tag;
            int w3 = 0;
            uintptr_t pktLo2 = (uintptr_t)s_PcPacketBufs[g_ActiveBufferIdx];
            uintptr_t pktHi2 = (uintptr_t)s_PcPacketBufEnds[g_ActiveBufferIdx];
            uintptr_t otLo2  = (uintptr_t)ot2->org;
            int       otLen2 = (ot2->length > 0 && ot2->length <= 16) ? (int)ot2->length : 0;
            size_t    otCnt2 = (size_t)1 << otLen2;
            uintptr_t otHi2  = (otLo2 && otLen2) ? (otLo2 + otCnt2 * sizeof(GsOT_TAG)) : otLo2;
            OT_TAG* prev2 = NULL;
            while (cur2 && w3 < 4096) {
                uintptr_t curAddr2 = (uintptr_t)cur2;
                int curOk2 = ((curAddr2 >= pktLo2 && curAddr2 < pktHi2) ||
                              (curAddr2 >= otLo2  && curAddr2 < otHi2));
                if (!curOk2) {
                    static int s_ot2DumpedOnce = 0;
                    if (!s_ot2DumpedOnce) {
                        s_ot2DumpedOnce = 1;
                        SH_DBG("[OT2-SANIT] FIRST bad cur2=%p (w3=%d) prev=%p — pkt=[%p..%p) ot=[%p..%p)",
                               (void*)cur2, w3, (void*)prev2,
                               (void*)pktLo2, (void*)pktHi2,
                               (void*)otLo2, (void*)otHi2);
                    }
                    /* Skip past corrupt prim — see OT0 sanitizer above for
                     * rationale. Jump to ot2->org[0] (closest-to-camera
                     * bucket) to preserve nearest-camera geometry. */
                    if (prev2 != NULL) {
                        setaddr(prev2, &ot2->org[0]);
                    } else {
                        ot2->tag = (u_long*)&ot2->org[0];
                    }
                    break;
                }
                if (isendprim(cur2)) break;
                int len2 = getlen(cur2);
                if (len2 > 0) {
                    u8 hi2 = ((P_TAG*)cur2)->code & 0xF0;
                    if (len2 > 32 || (hi2 != 0x00 && hi2 != 0x20 && hi2 != 0x30 &&
                        hi2 != 0x60 && hi2 != 0x70 && hi2 != 0xA0 && hi2 != 0xE0)) {
                        setlen(cur2, 0);
                    }
                }
                OT_TAG* next2 = (OT_TAG*)nextPrim(cur2);
                if (next2 && ((uintptr_t)next2 < 0x1000 || (uintptr_t)next2 > (uintptr_t)0x7FFFFFFFFFFF)) {
                    static int s_ot2BadNextDumped = 0;
                    if (!s_ot2BadNextDumped) {
                        s_ot2BadNextDumped = 1;
                        SH_DBG("[OT2-SANIT] FIRST wild next=%p at cur2=%p (w3=%d) — re-link to org[0]",
                               (void*)next2, (void*)cur2, w3);
                    }
                    setaddr(cur2, &ot2->org[0]);
                    break;
                }
                prev2 = cur2;
                cur2  = next2;
                w3++;
            }
        }
#endif
        ML_TRACE("OT2-draw");
        GsDrawOt(&g_OrderingTable2[g_ActiveBufferIdx]);
        ML_TRACE("OT2-done");
#ifdef SH_PC_PORT
        ML_TRACE("DebugConsole_Render");
        DebugConsole_Render();
        ML_TRACE("PsyX_EndScene");
        PsyX_EndScene();
        ML_TRACE("frame-done");
        if (g_SH_PostFireTrace > 0) {
            g_SH_PostFireTrace--;
        }
        /* End stack canary check */
        if (_stackCanary != 0xDEADBEEF) {
            SH_DBG("[CANARY] *** STACK CORRUPTION! canary=0x%08X", _stackCanary);
        }
        } /* close _stackCanary scope */
#endif
    }

    #undef TICKS_PER_SECOND_MIN
    #undef H_BLANKS_PER_SECOND
    #undef H_BLANKS_PER_TICK
    #undef H_BLANKS_TO_SEC_CONVERSION_FACTOR
    #undef H_BLANKS_PER_FRAME_MIN
    #undef H_BLANKS_Q12_TO_SEC_SCALE
    #undef H_BLANKS_GRAVITY_SCALE
    #undef V_BLANKS_MAX
}
