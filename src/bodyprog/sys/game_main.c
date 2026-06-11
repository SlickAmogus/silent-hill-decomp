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
extern int g_PcAllowDebugControls;
int g_PcMapScreenActive = 0; /* set while paper-map overlay is displayed */
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
#include "dbg_overlay.h"
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
#include "bodyprog/sound/sound_system.h"
#include "screens/b_konami/b_konami.h"

#include "bodyprog/memcard.h"
#include "bodyprog/sys/game_main.h"
#include "screens/stream/stream.h"
#include "screens/options.h"
#include "screens/credits/credits.h"
#include "screens/saveload.h"
#include "screens/b_konami/b_konami.h"
#include "bodyprog/game_boot/game_load.h"
#include "bodyprog/item_screens.h"
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
    GameState_LoadMapScreen_Update,
    GameState_InGame_Update,
    GameState_MapEvent_Update,
    GameState_ExitMovie_Update,
    GameState_ItemScreens_Update,
    GameState_PaperMapScreen_Update, // idx 15 = GameState_PaperMapScreen; merge mis-set this to LoadMapScreen_Update -> black map + spawn-reset teleport to (0,0,0)
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
int g_DebugInvincible = 0;       /* 0 = normal health, 1 = health locked to max each frame */
int g_DebugNoTarget = 0;         /* 0 = normal AI detection, 1 = enemies ignore Harry */
s32 g_TpsCamYaw = 0;             /* TPS orbit yaw (Q12), independent from Harry's body */
s32 g_TpsCamPitch = 0;           /* TPS orbit pitch (Q12) */
int g_SH_PostFireTrace = 0;      /* Frames remaining of verbose post-fire main-loop tracing */
int g_SH_AlwaysMlTrace = 0;      /* 1 = unconditional ML_TRACE every frame; flip to 0 once silent crashes are diagnosed */
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
static s32           g_PcCamNudgePitch   = 0; /* Q3.12 added to cam pitch (TRUE rotation) */
static s_DefaultCamera g_DefaultCam      = {{0,0,0}, {0,0,0}, 0};

/* Post-nudge cam state — written by the apply site after computing newCam
 * and newLook (the values actually passed to Vw_SetLookAtMatrix this frame).
 * Read by the BAD/GOOD log so [CAM-GOOD-FINAL] shows the exact on-screen
 * camera the user is looking at. g_PcCamAppliedValid is 0 when no nudge
 * was applied this frame (fall back to vcWork for the log). */
static VECTOR3       g_PcCamAppliedPos    = {0, 0, 0};
static VECTOR3       g_PcCamAppliedLookAt = {0, 0, 0};
static int           g_PcCamAppliedValid  = 0;

/* KP_0: "raw cam mode" — zeroes the manual numpad nudge so the engine's
 * unmodified camera output is visible. Lets the user take an accurate BAD
 * snapshot before adjusting. Toggle on/off with KP_0. */
static int           g_DebugRawCamMode    = 0;

void DebugCamera_Update(void)
{
    #define DBG_CAM_MOVE_SPEED 512   /* Q12(0.125) */
    #define DBG_CAM_TURN_SPEED 16
    #define DBG_CAM_VERT_SPEED 256

    if (!g_sdlKeyboardState) return;
#ifdef SH_PC_PORT
    /* Master gate for all dev/cheat keys (numpad cam, top-row digits, give-
     * weapon cheats, kill-Harry, noclip, etc.). Off unless allow_debug_controls
     * is set in config. */
    {
        extern int g_PcAllowDebugControls;
        if (!g_PcAllowDebugControls) return;
    }
    /* Console input mode: typed characters land on the same top-row keys the
     * debug binds use (0-9, -, =), and the controller suppression doesn't
     * cover these direct SDL reads — block them all while typing. */
    {
        extern int g_PcConsoleInputActive;
        if (g_PcConsoleInputActive) return;
    }
#endif
    if (g_GameWork.gameState != GameState_InGame) return;

    /* Esc: warm-reboot to the title screen (edge-triggered) — same path as the
     * controller Select+Start reset and the death warm-boot. Works in gameplay
     * and cutscenes (both are GameState_InGame), so it also escapes a stuck
     * scene. SysFlag_DoWarmReset is consumed by MainLoop_ShouldWarmReset(). */
    {
        static int s_escPrev = 0;
        int cur = g_sdlKeyboardState[SDL_SCANCODE_ESCAPE];
        if (cur && !s_escPrev) {
            g_SysWork.sysFlags |= SysFlag_DoWarmReset;
            SH_DBG_ECHO("[DEBUG] Esc: warm reboot to title");
        }
        s_escPrev = cur;
    }

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
                g_DebugCamSavedHarryPosY = g_SysWork.playerWork.player.properties.player.groundHeight;
                /* Keep Harry visible at his original position so we can
                 * see him while flying the debug cam — useful for
                 * marking corrected camera positions relative to him.
                 * (Was hidden + teleport-followed in the prior design.) */
            } else {
                /* Restore Harry's original position + visibility */
                g_SysWork.playerWork.player.position = g_DebugCamSavedHarryPos;
                g_SysWork.playerWork.player.properties.player.groundHeight = g_DebugCamSavedHarryPosY;
                g_SysWork.playerWork.player.model.anim.flags |= AnimFlag_Visible;
                g_SysWork.playerWork.extra.model.anim.flags |= AnimFlag_Visible;
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
            int curId = (int)g_SavegamePtr->mapIdx;
            int nextId = (curId + 1) % (MapOverlayId_MAPX_S00 + 1);
            g_SavegamePtr->mapIdx = nextId;
            MapRegistry_Load((e_MapOverlayId)nextId);
            extern void GameBoot_MapLoad(s32 mapIdx);
            GameBoot_MapLoad(nextId);
            SH_DBG("[DEBUG] Switched to map %s (overlay %d)",
                MapRegistry_GetName(nextId), nextId);
        }
        prevKey = cur;
    }
#endif

    /* Numpad 1: (unbound — collision toggle moved to top-row 0) */

    /* Top-row 0: toggle wall collision (noclip) */
    {
        static int prevKey = 0;
        int cur = g_sdlKeyboardState[SDL_SCANCODE_0];
        if (cur && !prevKey) {
            g_DebugNoWallCollision = !g_DebugNoWallCollision;
            Sd_PlaySfx(g_DebugNoWallCollision ? Sfx_MenuConfirm : Sfx_MenuCancel, 0, 64);
            SH_DBG_ECHO("[DEBUG] Key 0: Wall collision: %s", g_DebugNoWallCollision ? "OFF (noclip)" : "ON");
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
            SH_DBG_ECHO("[DEBUG] Numpad 2: Third-person camera: %s", g_DebugThirdPersonCam ? "ON (mouse captured)" : "OFF (mouse released)");
        }
        prevKey = cur;
    }

    /* Number key 1: kill Harry (triggers death animation) */
    {
        static int prevKey = 0;
        int cur = g_sdlKeyboardState[SDL_SCANCODE_1];
        if (cur && !prevKey) {
            g_SysWork.playerWork.player.health = -Q12(1.0f);
            SH_DBG_ECHO("[DEBUG] Key 1: KILL HARRY (health set to -1.0)");
        }
        prevKey = cur;
    }
    /* Number keys 4/5: cycle the `map` config value (4 = previous, 5 = next,
     * wrapping). Prints the new map + description and saves it to config.cfg so
     * a warm-reset (Esc) + New Game loads the chosen map. */
    {
        static int prevKey4 = 0, prevKey5 = 0;
        int cur4 = g_sdlKeyboardState[SDL_SCANCODE_4];
        int cur5 = g_sdlKeyboardState[SDL_SCANCODE_5];
        int dir  = 0;
        if (cur5 && !prevKey5)      dir = 1;
        else if (cur4 && !prevKey4) dir = -1;
        if (dir != 0) {
            int count = MapRegistry_Count();
            int id    = MapRegistry_FindByName(g_PcConfig.mapName);
            const char* name;
            if (id < 0) id = 0;
            id = (id + dir + count) % count;
            name = MapRegistry_GetName(id);
            strncpy(g_PcConfig.mapName, name, sizeof(g_PcConfig.mapName) - 1);
            g_PcConfig.mapName[sizeof(g_PcConfig.mapName) - 1] = '\0';
            PcConfig_SaveMapName(name);
            SH_DBG_ECHO("[DEBUG] Map config value changed to %s - %s",
                        name, MapRegistry_GetDescription(id));
        }
        prevKey4 = cur4;
        prevKey5 = cur5;
    }

    /* Number key 6: spawn a Grey Child ~2.5 units in front of Harry, facing him
     * (debug enemy spawn). Chara_Spawn refuses once the active NPC count reaches
     * the per-area cap (g_SysWork.npcFlagsId) — which is already met in rooms
     * that have Grey Children, and tiny during the Cheryl chase — so raise the
     * cap just for this one spawn. Also needs the Grey Child model loaded
     * (registeredCharaModels[Chara_GreyChild]); skip with a note if it isn't. */
    {
        static int prevKey6 = 0;
        int cur6 = g_sdlKeyboardState[SDL_SCANCODE_6];
        if (cur6 && !prevKey6) {
            s_SubCharacter* hr  = &g_SysWork.playerWork.player;
            q3_12           yaw = hr->rotation.vy;
            q19_12          sx  = hr->position.vx + Q12_MULT_PRECISE(Q12(2.5f), Math_Sin(yaw));
            q19_12          sz  = hr->position.vz + Q12_MULT_PRECISE(Q12(2.5f), Math_Cos(yaw));
            if (g_WorldGfxWork.registeredCharaModels[Chara_GreyChild] == NULL) {
                SH_DBG_ECHO("[DEBUG] Key 6: Grey Child model not loaded in this area — can't spawn visibly");
            } else {
                s32 savedCap = g_SysWork.npcFlagsId;
                g_SysWork.npcFlagsId = ARRAY_SIZE(g_SysWork.npcs);
                s32 idx = Chara_Spawn(Chara_GreyChild, 0, sx, sz, yaw ^ Q12_ANGLE(180.0f), 5);
                g_SysWork.npcFlagsId = savedCap;
                SH_DBG_ECHO("[DEBUG] Key 6: spawn Grey Child -> slot %d at (%ld,%ld) modelOK",
                    (int)idx, (long)sx, (long)sz);
            }
        }
        prevKey6 = cur6;
    }

    /* Top-row 7: toggle invincibility (health locked to max each frame) */
    {
        static int prevKey = 0;
        int cur = g_sdlKeyboardState[SDL_SCANCODE_7];
        if (cur && !prevKey) {
            g_DebugInvincible = !g_DebugInvincible;
            Sd_PlaySfx(g_DebugInvincible ? Sfx_MenuConfirm : Sfx_MenuCancel, 0, 64);
            SH_DBG_ECHO("[DEBUG] Key 7: Invincibility: %s", g_DebugInvincible ? "ON" : "OFF");
        }
        prevKey = cur;
    }
    /* Top-row 8: give 15 handgun bullets */
    {
        static int prevKey = 0;
        int cur = g_sdlKeyboardState[SDL_SCANCODE_8];
        if (cur && !prevKey) {
            Inventory_AddSpecialItem(0xC0, 15);
            Sd_PlaySfx(Sfx_MenuConfirm, 0, 64);
            SH_DBG_ECHO("[DEBUG] Key 8: Added 15 handgun bullets");
        }
        prevKey = cur;
    }
    /* Top-row 9: toggle no-target (enemies ignore Harry via CharaFlag_Unk4) */
    {
        static int prevKey = 0;
        int cur = g_sdlKeyboardState[SDL_SCANCODE_9];
        if (cur && !prevKey) {
            g_DebugNoTarget = !g_DebugNoTarget;
            Sd_PlaySfx(g_DebugNoTarget ? Sfx_MenuConfirm : Sfx_MenuCancel, 0, 64);
            SH_DBG_ECHO("[DEBUG] Key 9: No-target: %s", g_DebugNoTarget ? "ON (enemies ignore Harry)" : "OFF");
        }
        prevKey = cur;
    }
    /* Top-row -: give Hunting Rifle (skip if owned) + a stack of rifle shells */
    {
        static int prevKey = 0;
        int cur = g_sdlKeyboardState[SDL_SCANCODE_MINUS];
        if (cur && !prevKey) {
            bool hasRifle = false;
            for (int i = 0; i < INV_ITEM_COUNT_MAX; i++) {
                if (g_SavegamePtr->items[i].id_0 == InvItemId_HuntingRifle) {
                    hasRifle = true;
                    break;
                }
            }
            if (!hasRifle) Inventory_AddSpecialItem(InvItemId_HuntingRifle, 1);
            Inventory_AddSpecialItem(InvItemId_RifleShells, 30);
            Sd_PlaySfx(Sfx_MenuConfirm, 0, 64);
            SH_DBG_ECHO("[DEBUG] Key -: Added%s Rifle Shells x30", hasRifle ? "" : " Hunting Rifle +");
        }
        prevKey = cur;
    }
    /* Top-row =: give Shotgun (skip if owned) + a stack of shotgun shells */
    {
        static int prevKey = 0;
        int cur = g_sdlKeyboardState[SDL_SCANCODE_EQUALS];
        if (cur && !prevKey) {
            bool hasShotgun = false;
            for (int i = 0; i < INV_ITEM_COUNT_MAX; i++) {
                if (g_SavegamePtr->items[i].id_0 == InvItemId_Shotgun) {
                    hasShotgun = true;
                    break;
                }
            }
            if (!hasShotgun) Inventory_AddSpecialItem(InvItemId_Shotgun, 1);
            Inventory_AddSpecialItem(InvItemId_ShotgunShells, 30);
            Sd_PlaySfx(Sfx_MenuConfirm, 0, 64);
            SH_DBG_ECHO("[DEBUG] Key =: Added%s Shotgun Shells x30", hasShotgun ? "" : " Shotgun +");
        }
        prevKey = cur;
    }

    /* Per-frame cheat enforcement: invincibility + no-target */
    if (g_GameWork.gameState == GameState_InGame) {
        if (g_DebugInvincible)
            g_SysWork.playerWork.player.health = Q12(100.0f);
        if (g_DebugNoTarget)
            g_SysWork.playerWork.player.flags |= CharaFlag_Unk4;
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
            s8 curRoom    = g_SavegamePtr->mapRoomIdx;
            s8 curMap     = g_SavegamePtr->mapIdx;

            /* Snapshot rescue Y on map or room change. */
            if (curMap != _prevMapId || curRoom != _prevRoomIdx) {
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
                /* Log the FALL position before teleporting — user uses this
                 * to capture where Harry landed (= broken floor spot). Pair
                 * with HARRY POSITION LOGGED (Numpad .) which captures the
                 * spot where the fall STARTED. Use a unique searchable tag.
                 *
                 * Also probe collision at the current XZ to expose the slope
                 * angles (field_4=X-slope, field_6=Z-slope) and the IPD
                 * ground-validity count (field_8). groundH=Q12(8) means the
                 * IPD cell returned the void-ground sentinel — Harry is over
                 * a hole/missing collision data. */
                {
                    s_CollisionSurface _fallColl;
                    Collision_SurfaceGet(&_fallColl, oldX, oldZ);
                    SH_DBG("HARRY FALL POSITION mapId=%d roomIdx=%d pos=(%ld,%ld,%ld) yaw=%d groundH=%ld slopeX=%d slopeZ=%d validPts=%d voidCell=%d",
                        (int)g_SavegamePtr->mapIdx,
                        (int)g_SavegamePtr->mapRoomIdx,
                        (long)oldX, (long)oldY, (long)oldZ,
                        (int)facing,
                        (long)_fallColl.groundHeight,
                        (int)_fallColl.tiltAngleX, (int)_fallColl.tiltAngleZ,
                        (int)_fallColl.groundType,
                        (int)(_fallColl.groundHeight == Q12(8.0f)));
                }
                p->vy = _lastSafeY;
                pl->fallSpeed = 0;
                /* Push backward — sin/cos give forward direction, subtract. */
                p->vx -= (s32)((s64)sinV * pushDist >> 12);
                p->vz -= (s32)((s64)cosV * pushDist >> 12);
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
        #define PC_NUDGE_MOVE_SPEED  48    /* Q12(~0.012) — ~0.7m/s at 60fps, fine for tuning */
        #define PC_NUDGE_TURN_SPEED  3     /* ~5x slower than debug 16 */
        #define PC_NUDGE_VERT_SPEED  51    /* ~5x slower than debug 256 */

        /* Snapshot the pristine default cam BEFORE nudge application.
         * vcMoveAndSetCamera ran earlier this frame and put its result in
         * vcWork — that's our default. */
        g_DefaultCam.pos    = vcWork.cam_pos;
        g_DefaultCam.lookAt = vcWork.watch_tgt_pos;
        g_DefaultCam.valid  = 1;

        /* Auto-clear accumulated nudges on map transition so a follow-cam
         * entry or yaw/pitch from one map doesn't bleed into the next. */
        {
            static int s_prevMapForNudgeReset = -1;
            int curMapNow = (int)g_SavegamePtr->mapIdx;
            if (curMapNow != s_prevMapForNudgeReset) {
                if (s_prevMapForNudgeReset != -1) {
                    g_PcCamNudgePos.vx = 0;
                    g_PcCamNudgePos.vy = 0;
                    g_PcCamNudgePos.vz = 0;
                    g_PcCamNudgeYaw    = 0;
                    g_PcCamNudgePitch  = 0;
                }
                s_prevMapForNudgeReset = curMapNow;
            }
        }

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
            }
            prevKey3 = cur3;
        }

        /* Numpad 0: toggle "raw cam mode" — zeros the manual nudge so the
         * unmodified engine camera is visible. Use to get a clean BAD
         * snapshot before adjusting: press KP_0 (camera snaps to raw
         * default), log BAD (top-row 4), adjust with numpad, log GOOD
         * (top-row 5). Logs current g_DefaultCam on activation so you can
         * see the engine baseline in the log. */
        {
            static int prevKp0 = 0;
            int curKp0 = g_sdlKeyboardState[SDL_SCANCODE_KP_0];
            if (curKp0 && !prevKp0) {
                g_DebugRawCamMode = !g_DebugRawCamMode;
                g_PcCamNudgePos.vx = 0;
                g_PcCamNudgePos.vy = 0;
                g_PcCamNudgePos.vz = 0;
                g_PcCamNudgeYaw    = 0;
                g_PcCamNudgePitch  = 0;
                if (g_DebugRawCamMode) {
                } else {
                }
            }
            prevKp0 = curKp0;
        }

        /* Read numpad nudge keys (held = continuous). Camera-relative
         * forward/strafe uses the cam's current yaw so 8 always pushes
         * "into the screen". Deltas are NOT scaled by g_DeltaTime — the
         * base constants (esp. PC_NUDGE_TURN_SPEED=3) are small enough
         * that integer division by TIMESTEP_60_FPS at high fps rounds
         * them to 0, silently killing KP_7/9. Reverted to direct
         * constants so the keys always do something. */
        {
            /* Cam-relative movement: KP_8 always pushes into the screen,
             * KP_4/6 strafe along the cam's left/right, regardless of
             * which way the cam is facing in world space. Uses the
             * cam's CURRENT yaw (baseline + accumulated yaw nudge) so
             * direction follows the live view. */
            s32 camYaw = (s32)vcWork.cam_mat_ang.vy + g_PcCamNudgeYaw;
            s32 sinY   = Math_Sin(camYaw);
            s32 cosY   = Math_Cos(camYaw);
            if (g_sdlKeyboardState[SDL_SCANCODE_KP_8]) {
                g_PcCamNudgePos.vx += (s32)((s64)PC_NUDGE_MOVE_SPEED * sinY >> 12);
                g_PcCamNudgePos.vz += (s32)((s64)PC_NUDGE_MOVE_SPEED * cosY >> 12);
            }
            if (g_sdlKeyboardState[SDL_SCANCODE_KP_5]) {
                g_PcCamNudgePos.vx -= (s32)((s64)PC_NUDGE_MOVE_SPEED * sinY >> 12);
                g_PcCamNudgePos.vz -= (s32)((s64)PC_NUDGE_MOVE_SPEED * cosY >> 12);
            }
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
            /* Numpad +/-: tilt cam pitch. NOW a true rotation around the
             * cam-local X axis (matching the yaw rotation), units are
             * Q3.12 angle deltas. Empirically: KP_+ → pitchN negative
             * → cam tilts DOWN (lookAt drops toward ground); KP_- →
             * pitchN positive → cam tilts UP (lookAt rises toward sky).
             * Speed matches yaw (PC_NUDGE_TURN_SPEED) so both rotation
             * axes feel equally responsive. */
            if (g_sdlKeyboardState[SDL_SCANCODE_KP_PLUS]) {
                g_PcCamNudgePitch -= PC_NUDGE_TURN_SPEED;
            }
            if (g_sdlKeyboardState[SDL_SCANCODE_KP_MINUS]) {
                g_PcCamNudgePitch += PC_NUDGE_TURN_SPEED;
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
            }
            npslPrev = cur;
        }


        /* Apply the manual numpad nudge (debug camera tuning tool): rebuild
         * cam_pos / watch_tgt and the view matrix from the engine baseline
         * plus the runtime nudge. Yaw/pitch are applied as a TRUE rotation
         * around the cam axes so dragging feels intuitive. The scene-baseline
         * correction table was removed once the road/chase/settle cameras were
         * fixed at the source (Math_RotMatrixZxyNeg + TransposeMatrix); this
         * remains only as a live-tuning aid (BAD/GOOD logging, numpad). */
        s32 effPosX  = g_PcCamNudgePos.vx;
        s32 effPosY  = g_PcCamNudgePos.vy;
        s32 effPosZ  = g_PcCamNudgePos.vz;
        s32 effYaw   = g_PcCamNudgeYaw;
        s32 effPitch = g_PcCamNudgePitch;
        if (effPosX | effPosY | effPosZ | effYaw | effPitch)
        {
            VECTOR3 newCam, newLook;
            VECTOR3 dl;
            VECTOR3 baseLook;

            newCam.vx = vcWork.cam_pos.vx + effPosX;
            newCam.vy = vcWork.cam_pos.vy + effPosY;
            newCam.vz = vcWork.cam_pos.vz + effPosZ;

            baseLook = vcWork.watch_tgt_pos;

            if (effYaw | effPitch) {
                /* Rotate (baseLook - newCam) around newCam by yaw+pitch. */
                dl.vx = baseLook.vx - newCam.vx;
                dl.vy = baseLook.vy - newCam.vy;
                dl.vz = baseLook.vz - newCam.vz;

                s32 horizDist = SquareRoot0(dl.vx * dl.vx + dl.vz * dl.vz);
                s32 baseYaw   = ratan2(dl.vx, dl.vz);
                s32 basePitch = ratan2(-dl.vy, horizDist);

                s32 newYaw   = baseYaw   + effYaw;
                s32 newPitch = basePitch + effPitch;

                s32 dist3D = SquareRoot0(horizDist * horizDist + dl.vy * dl.vy);

                s32 cp = Math_Cos(newPitch);
                s32 sp = Math_Sin(newPitch);
                s32 cy = Math_Cos(newYaw);
                s32 sy = Math_Sin(newYaw);

                s32 newHorizDist = (s32)(((s64)dist3D * cp) >> 12);
                s32 newDlY = -(s32)(((s64)dist3D * sp) >> 12);
                s32 newDlX = (s32)(((s64)newHorizDist * sy) >> 12);
                s32 newDlZ = (s32)(((s64)newHorizDist * cy) >> 12);

                newLook.vx = newCam.vx + newDlX;
                newLook.vy = newCam.vy + newDlY;
                newLook.vz = newCam.vz + newDlZ;
            } else {
                /* No rotation — pure translation. */
                newLook = baseLook;
            }

            /* Stash applied state for the BAD/GOOD log so it can report
             * the actual on-screen cam (not just the pre-nudge baseline). */
            g_PcCamAppliedPos    = newCam;
            g_PcCamAppliedLookAt = newLook;
            g_PcCamAppliedValid  = 1;

            Vw_SetLookAtMatrix(&newCam, &newLook);
            vwSetViewInfo();

            /* Periodic trace so log shows nudge cam is live */
            {
                static int tickCounter = 0;
                if ((++tickCounter & 0x3F) == 0) {
                }
            }
        }
        else
        {
            /* No nudge / no scene correction — log fallback uses vcWork. */
            g_PcCamAppliedValid = 0;
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

    /* Frame-rate independence: TIMESTEP_SCALE_60_FPS preserves the 60fps
     * base value (1×) and doubles at 30fps so wall-time speed is uniform.
     * Without this, holding e.g. KP_8 at 30fps would move the debug cam
     * half as fast as at 60fps. */
    s32 dbgMoveSpeed = TIMESTEP_SCALE_60_FPS(g_DeltaTime, DBG_CAM_MOVE_SPEED);
    s32 dbgTurnSpeed = TIMESTEP_SCALE_60_FPS(g_DeltaTime, DBG_CAM_TURN_SPEED);
    s32 dbgVertSpeed = TIMESTEP_SCALE_60_FPS(g_DeltaTime, DBG_CAM_VERT_SPEED);

    /* Numpad 8: forward */
    if (g_sdlKeyboardState[SDL_SCANCODE_KP_8]) {
        g_DebugCamPos.vx += (s32)((s64)dbgMoveSpeed * sinY >> 12);
        g_DebugCamPos.vz += (s32)((s64)dbgMoveSpeed * cosY >> 12);
        moved = 1;
    }
    /* Numpad 5: backward */
    if (g_sdlKeyboardState[SDL_SCANCODE_KP_5]) {
        g_DebugCamPos.vx -= (s32)((s64)dbgMoveSpeed * sinY >> 12);
        g_DebugCamPos.vz -= (s32)((s64)dbgMoveSpeed * cosY >> 12);
        moved = 1;
    }
    /* Numpad 4: strafe left */
    if (g_sdlKeyboardState[SDL_SCANCODE_KP_4]) {
        g_DebugCamPos.vx -= (s32)((s64)dbgMoveSpeed * cosY >> 12);
        g_DebugCamPos.vz += (s32)((s64)dbgMoveSpeed * sinY >> 12);
        moved = 1;
    }
    /* Numpad 6: strafe right */
    if (g_sdlKeyboardState[SDL_SCANCODE_KP_6]) {
        g_DebugCamPos.vx += (s32)((s64)dbgMoveSpeed * cosY >> 12);
        g_DebugCamPos.vz -= (s32)((s64)dbgMoveSpeed * sinY >> 12);
        moved = 1;
    }
    /* Numpad 7: turn left */
    if (g_sdlKeyboardState[SDL_SCANCODE_KP_7]) {
        g_DebugCamAngleY -= dbgTurnSpeed;
        moved = 1;
    }
    /* Numpad 9: turn right */
    if (g_sdlKeyboardState[SDL_SCANCODE_KP_9]) {
        g_DebugCamAngleY += dbgTurnSpeed;
        moved = 1;
    }
    /* Page Up: move up (Y-, PSX Y is inverted) */
    if (g_sdlKeyboardState[SDL_SCANCODE_PAGEUP]) {
        g_DebugCamPos.vy -= dbgVertSpeed;
        moved = 1;
    }
    /* Page Down: move down (Y+) */
    if (g_sdlKeyboardState[SDL_SCANCODE_PAGEDOWN]) {
        g_DebugCamPos.vy += dbgVertSpeed;
        moved = 1;
    }
    /* Numpad +: tilt up (look upward) */
    if (g_sdlKeyboardState[SDL_SCANCODE_KP_PLUS]) {
        g_DebugCamAngleX -= dbgTurnSpeed;
        moved = 1;
    }
    /* Numpad -: tilt down (look downward) */
    if (g_sdlKeyboardState[SDL_SCANCODE_KP_MINUS]) {
        g_DebugCamAngleX += dbgTurnSpeed;
        moved = 1;
    }
    /* Numpad /: print debug camera coordinates to log */
    {
        static int dbg_slash_prev = 0;
        int dbg_slash_cur = g_sdlKeyboardState[SDL_SCANCODE_KP_DIVIDE];
        if (dbg_slash_cur && !dbg_slash_prev) {
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

    func_80033548();
    Screen_BackgroundImgDraw(&g_MainImg0);
    func_80089090(1);
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
        /* PsyCross requires explicit input polling — on PSX this happens
         * via hardware interrupt during VBlank. */
        PsyX_UpdateInput();
        DbgOverlay_Update();
#endif
        // Update input.
        Joy_ReadP1();
        Demo_ControllerDataUpdate();
        Joy_ControllerDataUpdate();

#ifdef SH_PC_PORT
        /* Console input mode: suppress controller input HERE, after the pad
         * parse refills g_Controller0 and before game logic reads it — zeroing
         * later in the loop gets overwritten by the next frame's parse before
         * any consumer sees it. Swallow extends past input mode until the
         * submit/exit keys release, so Enter can't leak into the game as
         * Start. */
        {
            extern int g_PcConsoleInputActive;
            extern int g_PcConsoleSwallowInput;
            if (g_PcConsoleInputActive || g_PcConsoleSwallowInput) {
                /* Release the swallow only when BOTH the raw SDL keys and the
                 * parsed pad are clear of the submit/exit keystroke. The
                 * keyboard→pad emulation lags the SDL array by a frame, so an
                 * SDL-only check lets the final Enter through: with prev-held
                 * zeroed, the stale Start parses as a fresh click edge and
                 * fires a menu action. */
                int swallowRelease =
                    !g_PcConsoleInputActive &&
                    g_sdlKeyboardState != NULL &&
                    !g_sdlKeyboardState[SDL_SCANCODE_RETURN] &&
                    !g_sdlKeyboardState[SDL_SCANCODE_GRAVE] &&
                    !(g_Controller0->heldBtnFlags & ControllerFlag_Start);
                if (swallowRelease) {
                    g_PcConsoleSwallowInput = 0;
                } else {
                    g_Controller0->heldBtnFlags      = 0;
                    g_Controller0->clickedBtnFlags   = 0;
                    g_Controller0->releasedBtnFlags  = 0;
                    g_Controller0->pulsedBtnFlags    = 0;
                    g_Controller0->pulsedGuiBtnFlags = 0;
                    g_Controller0->sticks_20.rawData_0 = 0;
                    g_Controller0->sticks_24.rawData_0 = 0;
                }
            }
        }

        /* Quick Save (F6) / Quick Load (F8) — always on, not debug-gated. */
        {
            extern void Pc_QuickSaveLoadUpdate(void);
            Pc_QuickSaveLoadUpdate();
        }

        /* Console `fmv`: once the fade-out it started lands, this blocks in
         * FMV_Play and fades back in afterwards. */
        {
            extern void Pc_ConsoleFmvUpdate(void);
            Pc_ConsoleFmvUpdate();
        }

        /* [SPEED] probe: wall-clock ground speed averaged over 1s while
         * moving, with fps/zone/state — to compare run speed across fps
         * caps and areas ("runs too fast in some scenarios"). Gated on
         * logging only (not debug controls) so any user log captures it;
         * SH_DBG is a no-op without enable_debug_log anyway. */
        if (g_PcConfig.enableDebugLog && g_GameWork.gameState == GameState_InGame &&
            g_SysWork.sysState == SysState_Gameplay)
        {
            static Uint64  s_spdT0;
            static VECTOR3 s_spdPrev;
            static s64     s_spdAccumSq; /* sum of per-frame |dXZ| in Q12 */
            static int     s_spdFrames, s_spdInit;
            s_SubCharacter* pl  = &g_SysWork.playerWork.player;
            Uint64          now = SDL_GetPerformanceCounter();

            if (!s_spdInit) {
                s_spdInit = 1;
                s_spdT0   = now;
                s_spdPrev = pl->position;
            }
            {
                s32 dx = pl->position.vx - s_spdPrev.vx;
                s32 dz = pl->position.vz - s_spdPrev.vz;
                /* Ignore teleport spikes (room transitions, debug warps) —
                 * they'd pollute the average and can overflow SQUARE(). */
                if (ABS(dx) < Q12(2.0f) && ABS(dz) < Q12(2.0f))
                    s_spdAccumSq += SquareRoot0(SQUARE(dx) + SQUARE(dz));
                s_spdPrev     = pl->position;
                s_spdFrames++;

                {
                    double el = (double)(now - s_spdT0) / (double)SDL_GetPerformanceFrequency();
                    if (el >= 1.0) {
                        if (s_spdAccumSq > Q12(0.2f)) {
                            extern s32 Map_SpeedZoneTypeGet(q19_12 posX, q19_12 posZ);
                            SH_DBG("[SPEED] %.2f u/s fps=%.0f moveSpeed=%.2f zone=%d state=%d",
                                   ((double)s_spdAccumSq / 4096.0) / el,
                                   (double)s_spdFrames / el,
                                   (double)pl->moveSpeed / 4096.0,
                                   (int)Map_SpeedZoneTypeGet(pl->position.vx, pl->position.vz),
                                   (int)g_SysWork.playerWork.extra.lowerBodyState);
                        }
                        s_spdT0      = now;
                        s_spdAccumSq = 0;
                        s_spdFrames  = 0;
                    }
                }
            }
        }
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

        GsClearOt(0, 0, &g_OrderingTable0[g_ActiveBufferIdx]);
        GsClearOt(0, 0, &g_OrderingTable2[g_ActiveBufferIdx]);

        g_SysWork.bgmStatusFlags = BgmStatusFlag_None;

        PC_OT_SCAN("pre-GameStateUpdate");
        // Call update function for current GameState.
        g_GameStateUpdateFuncs[g_GameWork.gameState]();
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
                }
                canaryOk = 1;
                for (i = 0; i < PC_CANARY_SIZE; i++) {
                    if (pktEnd1[i] != PC_CANARY_VAL) { canaryOk = 0; break; }
                }
                if (!canaryOk) {
                }
            }
        }
#endif

        Demo_Update();
        PC_OT_SCAN("post-Demo_Update");
        Demo_GameRandSeedSet();

        if (MainLoop_ShouldWarmReset() == 2)
        {
            Game_WarmBoot();
            continue;
        }

#define ML_TRACE(tag) ((void)0)
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
        if (g_SysWork.sysFlags & SysFlag_DemoActive)
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

#ifdef SH_PC_PORT
        /* Interactive console input mode (hold `~`): freeze the game like the
         * pause screen — zero game time so the world stops simulating. Must
         * happen here, after the dt recompute above, so next frame's game
         * update sees 0. Controller suppression lives next to the pad parse
         * at the top of the loop. */
        {
            extern int g_PcConsoleInputActive;
            if (g_PcConsoleInputActive) {
                g_DeltaTime    = 0;
                g_DeltaTimeRaw = 0;
                g_GravitySpeed = 0;
            }
        }
#endif

        ML_TRACE("GsSwapDispBuff");
        // Draw objects?
        GsSwapDispBuff();
        ML_TRACE("post-GsSwapDispBuff");
#ifdef SH_PC_PORT
        /* Numpad .: (1) always logs Harry's detailed position with a unique
         * searchable "HARRY POSITION LOGGED" tag — used to mark spots where
         * Harry falls through the floor. Pair with Numpad 3's HARRY FALL
         * POSITION which records where he LANDS. (2) During debug camera
         * mode, also toggles fog on/off. */
        if (g_PcAllowDebugControls && g_sdlKeyboardState && g_GameWork.gameState == 11) {
            int cur = g_sdlKeyboardState[SDL_SCANCODE_KP_PERIOD];
            if (cur && !g_DebugFogTogglePrev) {
                VECTOR3 camPos;
                s_CollisionSurface _hereColl;
                if (g_DebugCamEnabled)              camPos = g_DebugCamPos;
                else if (g_DebugThirdPersonCam)     vcGetNowCamPos(&camPos);
                else                                camPos = vcWork.cam_pos;
                Collision_SurfaceGet(&_hereColl,
                    g_SysWork.playerWork.player.position.vx,
                    g_SysWork.playerWork.player.position.vz);
                SH_DBG("HARRY POSITION LOGGED mapId=%d roomIdx=%d pos=(%ld,%ld,%ld) yaw=%d pitch=%d moveSpeed=%ld camPos=(%ld,%ld,%ld) camYaw=%d camPitch=%d groundH=%ld slopeX=%d slopeZ=%d validPts=%d voidCell=%d",
                    (int)g_SavegamePtr->mapIdx,
                    (int)g_SavegamePtr->mapRoomIdx,
                    (long)g_SysWork.playerWork.player.position.vx,
                    (long)g_SysWork.playerWork.player.position.vy,
                    (long)g_SysWork.playerWork.player.position.vz,
                    (int)g_SysWork.playerWork.player.rotation.vy,
                    (int)g_SysWork.playerWork.player.rotation.vx,
                    (long)g_SysWork.playerWork.player.moveSpeed,
                    (long)camPos.vx, (long)camPos.vy, (long)camPos.vz,
                    (int)vcWork.cam_mat_ang.vy,
                    (int)vcWork.cam_mat_ang.vx,
                    (long)_hereColl.groundHeight,
                    (int)_hereColl.tiltAngleX, (int)_hereColl.tiltAngleZ,
                    (int)_hereColl.groundType,
                    (int)(_hereColl.groundHeight == Q12(8.0f)));
                if (g_DebugCamEnabled) {
                    g_DebugFogDisabled = !g_DebugFogDisabled;
                    SH_DBG_ECHO("[DEBUG] Numpad . (debug cam) Fog: %s", g_DebugFogDisabled ? "OFF" : "ON");
                }
            }
            g_DebugFogTogglePrev = cur;

            /* When debug camera is off, fog is always normal */
            if (!g_DebugCamEnabled) {
                g_DebugFogDisabled = 0;
            }

            if (g_DebugFogDisabled) {
                PC_WorldEnvWork.isFogEnabled = 0;
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
                              !g_PsxSkipFramebufferStore &&
                              !g_PcMapScreenActive) ? 1 : 0;

        /* Suppress dither on 2D-only states (logos, menus, map screen,
         * inventory, options, save/load). Dither makes flat-shaded UI
         * art look chewed-up at high resolution. Keep it for 3D gameplay
         * + cutscenes (InGame state covers both). Read by PsyCross via
         * extern int g_PsxDitherSuppressed in PsyX_render.cpp. */
        extern int g_PsxDitherSuppressed;
        g_PsxDitherSuppressed = (g_GameWork.gameState == GameState_InGame) ? 0 : 1;

        /* Override background color with fog color during InGame.
         * fog params are set by Gfx_FlashlightUpdate from the previous frame's
         * update, so they're valid by frame 2+. Use the normal GsSortClear path
         * which PsyCross handles via activeDrawEnv.isbg in PsyX_BeginScene. */
        if (g_GameWork.gameState == 11 &&
            ((g_SysWork.sysFlags & SysFlag_MenuActive) || g_SysWork.sysState == SysState_GameOver)) {
            /* A 2D menu (inventory/status/options/paper map) is open or opening
             * within InGame. These transition through gameState==InGame for a
             * few frames before the menu draws, so the fog-color background
             * clear below would flash gray (the void fog color) on open. Force
             * black for the 2D background instead; fog params are left untouched
             * so 3D gameplay fog is unaffected once the menu closes.
             * Same for the GAME OVER screen: it's a SysState within InGame that
             * draws text/tips over what must be the game's own black clear. */
            g_GameWork.background2dColor.r = 0;
            g_GameWork.background2dColor.g = 0;
            g_GameWork.background2dColor.b = 0;
        }
        else if (g_GameWork.gameState == 11 && PC_WorldEnvWork.isFogEnabled) {
            /* Fullscreen 2D background screens (eclipse/plates doors, item
             * inspection) must clear to the game's own color (black) — on
             * PSX the fog void isn't the clear color, so these screens were
             * never fog-tinted. Counter set by Screen_BackgroundImgDraw*. */
            extern s32 g_Pc2dBackgroundActive;
            if (g_Pc2dBackgroundActive > 0) {
                g_Pc2dBackgroundActive--;
                g_GameWork.background2dColor.r = 0;
                g_GameWork.background2dColor.g = 0;
                g_GameWork.background2dColor.b = 0;
            } else {
                g_GameWork.background2dColor.r = PC_WorldEnvWork.fog.color.r;
                g_GameWork.background2dColor.g = PC_WorldEnvWork.fog.color.g;
                g_GameWork.background2dColor.b = PC_WorldEnvWork.fog.color.b;
            }
            g_PsyX_FogColor[0] = PC_WorldEnvWork.fog.color.r / 255.0f;
            g_PsyX_FogColor[1] = PC_WorldEnvWork.fog.color.g / 255.0f;
            g_PsyX_FogColor[2] = PC_WorldEnvWork.fog.color.b / 255.0f;
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
                }
                while (cur && w2 < 8192) {
                    uintptr_t curAddr = (uintptr_t)cur;
                    int curOk = ((curAddr >= pktLo && curAddr < pktHi) ||
                                 (curAddr >= otLo  && curAddr < otHi)  ||
                                 (subLo && curAddr >= subLo && curAddr < subHi));
                    if (pmapTrace && w2 < 200) {
                        u8 dbgCode = curOk ? ((P_TAG*)cur)->code : 0xFF;
                        int dbgLen = curOk ? getlen(cur) : -1;
                    }
                    if (!curOk) {
                        static int s_dumpedOnce = 0;
                        if (!s_dumpedOnce) {
                            s_dumpedOnce = 1;
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
                            /* LINE_F2 (0x42) and LINE_G2 (0x52) used by inventory
                             * selection-box borders in item_screens_3.c */
                            hi != 0x40 && hi != 0x50 &&
                            hi != 0x60 && hi != 0x70 && hi != 0xA0 &&
                            codeFull != 0xE1 &&
                            /* textured/quad poly types emitted by NTG3/NTG4/TG3/TG4 */
                            hi != 0x24 && hi != 0x28 && hi != 0x2C &&
                            hi != 0x34 && hi != 0x38 && hi != 0x3C)) {
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
                /* Accept heap packet buffer, OT array, OR any static/BSS primitive
                 * (e.g. screen fade DR_MODE/TILE in D_800A8E5C/D_800A8E74).
                 * Only reject null, very-low, or kernel-space addresses. */
                int curOk2 = (curAddr2 >= 0x1000 && curAddr2 <= (uintptr_t)0x7FFFFFFFFFFF);
                if (!curOk2) {
                    static int s_ot2DumpedOnce = 0;
                    if (!s_ot2DumpedOnce) {
                        s_ot2DumpedOnce = 1;
                    }
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
                        hi2 != 0x40 && hi2 != 0x50 &&
                        hi2 != 0x60 && hi2 != 0x70 && hi2 != 0xA0 && hi2 != 0xE0)) {
                        setlen(cur2, 0);
                    }
                }
                OT_TAG* next2 = (OT_TAG*)nextPrim(cur2);
                if (next2 && ((uintptr_t)next2 < 0x1000 || (uintptr_t)next2 > (uintptr_t)0x7FFFFFFFFFFF)) {
                    static int s_ot2BadNextDumped = 0;
                    if (!s_ot2BadNextDumped) {
                        s_ot2BadNextDumped = 1;
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
        DbgOverlay_Render();
        ML_TRACE("PsyX_EndScene");
        PsyX_EndScene();
        ML_TRACE("frame-done");
        if (g_SH_PostFireTrace > 0) {
            g_SH_PostFireTrace--;
        }
        /* End stack canary check */
        if (_stackCanary != 0xDEADBEEF) {
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
