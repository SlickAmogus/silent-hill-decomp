#include "game.h"

#ifdef SH_PC_PORT
#include "sh_log.h"
#include "pc_config.h"
#include "xa_player.h"
#include <SDL_timer.h>
#include <math.h>
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
#include "bodyprog/screen/screen_fade.h"
#include "bodyprog/screen/vsync.h"
#include "bodyprog/sys/joy.h"
#include "bodyprog/text/text_draw.h"
#include "bodyprog/math/math.h"
#include "bodyprog/collision/ray.h"
#include "bodyprog/memcard.h"
#include "bodyprog/sound/sound_system.h"
#include "screens/b_konami/b_konami.h"
#include "control_style.h"

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
 * Each chunk uses ~40KB of primitives. 25 * 40 = 1MB. 2MB gives headroom.
 * Whole-town mode submits the whole in-view cone (~60+ chunks) at once, so the
 * arena is enlarged and the whole-map submit budget (bodyprog_80040B74.c) is
 * sized well under it — the old 1.2MB budget was throttling the draw set to ~19
 * chunks. 16MB fits ~400 chunks; normal play still uses <2MB. */
#define PC_PKTBUF_SIZE (16 * 1024 * 1024)
#define PC_CANARY_SIZE 64
#define PC_CANARY_VAL  0xDE
static PACKET* s_PcPacketBufs[2] = { NULL, NULL };
static PACKET* s_PcPacketBufEnds[2] = { NULL, NULL };

/* Exposes the active packet arena's end so the wide-LM drawer (pc_wide_lm_draw.c)
 * can bounds-check every emit — the GsOUT_PACKET_P arena has no emit-time guard,
 * and a dense modded model can emit far more prims than any stock walker. */
PACKET* Pc_PacketBuf_End(void)
{
    return s_PcPacketBufEnds[g_ActiveBufferIdx];
}
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
int g_PcGodMode = 0;             /* 1 = god mode: no combat damage + health held full. ONE shared flag toggled by the `god` console cmd AND debug key 7, so turning it off either way fully disables it. */
int g_DebugNoFloorCollision = 0; /* 0 = floor collision on, always on (toggle removed) */
int g_DebugThirdPersonCam = 0;   /* 0 = game camera, 1 = static third-person follow cam */
int g_DebugNoTarget = 0;         /* 0 = normal AI detection, 1 = enemies ignore Harry */
int g_DebugAnimKfView = 0;       /* 1 = freeze Harry's whole skeleton on g_DebugAnimKf for keyframe inspection */
int g_DebugAnimKf = 588;         /* absolute keyframe index posed while g_DebugAnimKfView is on (588 = gun-forward) */
int g_DebugAnimKfMax = 0;        /* keyframeCount of Harry's active anim header, published by Player_Update for the inspector panel */
int g_DebugAnimPlaying = 0;      /* 1 = loop the selected keyframe range (P) instead of freezing on one frame */
int g_DebugAnimKfStart = 0;      /* selected loop range start (absolute keyframe) */
int g_DebugAnimKfEnd   = 0;      /* selected loop range end (absolute keyframe) */
s32 g_DebugAnimRate    = Q12(15.0f); /* loop playback rate (q19_12 kf/sec @30fps); copied from the source anim when constant */
int g_DebugViewNpcSlot = -1;     /* keyframe viewer target: -1 = Harry, else g_SysWork.npcs[] slot */
int g_DebugAnimPlayGen = 0;      /* bumped on each play (re)start so the loop cursor re-seeds */
s32 g_TpsCamYaw = 0;             /* TPS orbit yaw (Q12), independent from Harry's body */
s32 g_TpsCamPitch = 0;           /* TPS orbit pitch (Q12) */
/* Camera eye + forward (unit Q12) published each frame by Pc_TpsCamera_Apply for
 * free-aim: the aim ray is cast from g_TpsCamPos along g_TpsCamFwd (screen-center
 * reticle == camera forward). Read by Player_CombatUpdate. */
VECTOR3 g_TpsCamPos = { 0, 0, 0 };
VECTOR3 g_TpsCamFwd = { 0, 0, Q12(1.0f) };
/* First-person camera eye offset in Harry's local frame (vx=right, vy=up [Y-up
 * is negative], vz=forward), rotated by his BODY yaw each frame to place the eye
 * between his arms. Captured via the L cam-pos log key or the numpad tuner. */
extern int g_PcFpsCam;
VECTOR3 g_PcFpsOffset = { -29, -6836, 919 }; /* FPS eye BASELINE in Harry's BODY frame (all weapons); vx=right, vy=up(neg), vz=forward. Head-follow sway rides on top. */
VECTOR3 g_PcFpsViewFwd = { 0, 0, 4096 };     /* FPS view-forward, WORLD space Q12; published each FPS frame for the head-mounted flashlight */
VECTOR3 g_PcFpsEyePos  = { 0, 0, 0 };        /* FPS eye WORLD pos (Q19.12); flashlight origin in FPS */
VECTOR3 g_PcFlashlightShadowWorld = { 0, 0, 0 }; /* physical flashlight WORLD pos (Q19.12), captured before the FPS eye-override; drives the shadow-map light so FPS shadows match third person */
/* FPS melee arm-clearance dolly (Pc_TpsCamera_Apply): smoothed pull distance, and
 * the "head visible" publish read by world_draw.c — once the dolly is genuinely
 * behind Harry's head, the head draws again so the pulled-back swing reads as a
 * brief third-person beat instead of a headless body. */
static s32 s_FpsSwingPull   = 0; /* Q12 */
int g_PcFpsSwingHeadShow = 0;
/* Published each FPS frame for the KP_5 tuner log: the running head-sway reference
 * (== idle-mean head-local once settled) and the instantaneous head-local. Logging
 * the settled ref at idle gives the constant needed to bake a FIXED sway reference. */
VECTOR3 g_PcFpsHeadRefDbg   = { 0, 0, 0 };
VECTOR3 g_PcFpsHeadLocalDbg = { 0, 0, 0 };
/* Which device last drove the aim/look: 0 = mouse, 1 = controller. Sticky (holds
 * the last device while look input is momentarily idle). Read by Pc_AimAssistFind
 * to pick a mouse-light vs controller-strong (auto-aim) assist window. */
int g_PcAimDevice = 0;
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


/* FPS eye position actually applied this frame; read by the L-key re-log so its
 * LOCAL OFFSET reflects the live first-person camera. */
static VECTOR3       g_PcCamAppliedPos    = {0, 0, 0};
static int           g_PcCamAppliedValid  = 0;

/* TPS orbit camera application. Promoted out of the debug-only path: runs
 * whenever the TPS control style is active (g_DebugThirdPersonCam, mirrored
 * from g_ControlStyle), with or without allow_debug_controls. Mouse delta
 * orbits the camera around Harry; body-yaw follow + movement live in
 * player_control.c's TPS branch. */
/* True while a scripted scene owns the screen. The alternate cameras (TPS/OTS/FPS)
 * must stand down for these — otherwise the follow/eye camera overrides the scripted
 * shot and the scene plays from the wrong place (Harry's sewer ladder descent being
 * the obvious one). Standing down does NOT freeze the view: vcMoveAndSetCamera has
 * already placed the game's own camera this frame, so skipping our override simply
 * lets that camera through.
 *
 * A letterboxed cinematic is unambiguous. Everything else is a scene only when the
 * script drives BOTH the camera and Harry — neither half is sufficient alone:
 *
 *  - The camera flags alone (VC_USER_CAM_F / VC_USER_WATCH_F, raised by
 *    vcUserCamTarget / vcUserWatchTarget) are also raised by the maps' own boss and
 *    region cameras during ORDINARY gameplay. map7_s03 re-raises them every single
 *    frame of the final boss fight, so keying on them alone would leave the alternate
 *    camera dead for that entire fight.
 *
 *  - g_Player_DisableControl alone (Player_ControlFreeze) is raised by every text box
 *    and every item pickup, which freeze Harry but never touch the camera. Keying on
 *    it alone would pop the view to the classic angle and back on every memo and
 *    every item taken — Event_ItemTake freezes at EventState_Initialize, several
 *    frames before Gfx_PickupItemAnimate pauses the world, so there is a live window.
 *
 * Both together hold exactly for the scripted scenes: the sewer descent, the DMS
 * cutscenes, the scripted camera moves.
 *
 * SysState_ReadMessage is excluded outright — examining a memo is never a scene, not
 * even in the few areas whose region camera happens to hold the camera flags up
 * during gameplay. control_style.c makes the same carve-out. */
extern u8 g_Player_DisableControl; /* bodyprog/player.h */

/* Non-static: the per-vertex flashlight override (bodyprog_80055028.c) gates
 * its FPS-eye aim on this too, so the flashlight follows the FPS eye only when
 * the FPS camera is actually the view (not during scripted scenes/cutscenes). */
int Pc_ScriptOwnsScene(void)
{
    if ((g_SysWork.sysFlags & SysFlag_CutsceneActive) ||
        g_SysWork.cutsceneBorderState != CutsceneBorderState_None)
        return 1;

    if (g_SysWork.sysState == SysState_ReadMessage)
        return 0;

    return (vcWork.flags & (VC_USER_CAM_F | VC_USER_WATCH_F)) && g_Player_DisableControl;
}

/* Alternate-camera FOV (degrees of horizontal FOV on the 4:3 frame): override the
 * GTE projection distance ONLY during interactive alternate-camera gameplay. FPS
 * uses fps_fov, Thirdperson and Over-the-Shoulder use tps_fov. Menus, cutscenes,
 * scripted scenes and the Classic fixed cameras always keep the game's own
 * projection. H = 160 / tan(fov/2).
 *
 * The game's own projection distance in gameplay is g_GameWork.gsScreenHeight, and
 * gameplay runs PROGRESSIVE — Screen_Init(SCREEN_WIDTH, false) — so H is 224, not
 * 240. On the 320-wide frame that is a true horizontal FOV of 2*atan(160/224) =
 * 71.1 deg, which is why both fps_fov and tps_fov default to 71.1: they map back to
 * H = 224, the exact value vcExecCamera already set this frame, so the defaults are
 * a genuine no-op. Do NOT use 67.4 (H = 240) — that is the interlaced height and is
 * narrower than the game's real gameplay FOV.
 *
 * Called on BOTH exits of Pc_TpsCamera_Apply, including the stand-down path: the
 * restore has to run even when the camera body is skipped, or the FOV stays
 * clamped onto the scripted shot for the whole scene. */
static void Pc_CameraFov_Update(void)
{
    static int s_fovApplied = 0;
    float      fov          = 0.0f;

    /* Apply the alt-cam FOV whenever the alt camera BODY is active this frame — i.e.
     * we did not stand down (!Pc_ScriptOwnsScene). Restricting to SysState_Gameplay
     * dropped the FOV back to the game default during examine (SysState_ReadMessage)
     * and item pickup, even though the alt camera keeps rendering — a visible FOV pop.
     * The stand-down exit calls this too, where Pc_ScriptOwnsScene() is true, so the
     * game FOV is correctly restored for scripted scenes/cutscenes. */
    if (g_GameWork.gameState == GameState_InGame &&
        !Pc_ScriptOwnsScene())
    {
        if (g_PcFpsCam)
            fov = g_PcConfig.fpsFov;
        else if (g_ControlStyle == ControlStyle_Tps || g_ControlStyle == ControlStyle_Ots)
            fov = g_PcConfig.tpsFov;
    }

    if (fov > 0.0f)
    {
        /* Float tan + round-to-nearest so a default tps_fov (71.1) lands on EXACTLY
         * H = 224 = gsScreenHeight, i.e. the projection the game already had. */
        float t = tanf(fov * (3.14159265f / 360.0f));
        s32   h = (t > 0.001f) ? (s32)((160.0f / t) + 0.5f) : g_GameWork.gsScreenHeight;
        if (h < 16)  h = 16;
        if (h > 512) h = 512;
        SetGeomScreen(h);
        s_fovApplied = 1;
    }
    else if (s_fovApplied)
    {
        SetGeomScreen(g_GameWork.gsScreenHeight);
        s_fovApplied = 0;
    }
}

static void Pc_TpsCamera_Apply(void)
{
    /* Hand the camera back to the game whenever a script owns the scene. */
    if (Pc_ScriptOwnsScene())
    {
        /* Exception: the post-load fade-in of a room/area transition is not a
         * scripted scene -- Pc_ScriptOwnsScene only trips its third branch there
         * (camera flags + frozen control). In first person, standing down lets the
         * vanilla third-person camera ease in from its stale position, which reads
         * as the eye floating toward Harry's body out of a void. The FPS eye is
         * computed fresh from Harry's already-placed head, so applying it here snaps
         * straight in with no drift. Only during the actual fade-in (masked status
         * FadeInStart/FadeInSteps) and never for a real cutscene, so scripted camera
         * moves and cutscenes still stand down normally. */
        int fpsSnapThroughLoad =
            g_PcFpsCam &&
            (g_Screen_FadeStatus & 0x7) >= ScreenFadeState_FadeInStart &&
            !(g_SysWork.sysFlags & SysFlag_CutsceneActive) &&
            g_SysWork.cutsceneBorderState == CutsceneBorderState_None;

        if (!fpsSnapThroughLoad)
        {
            Pc_CameraFov_Update();
            return;
        }
    }

    #define TP_DIST         Q12(2.5f)    /* orbit radius from Harry */
    #define TP_HEIGHT       Q12(-1.4f)   /* base lift above Harry (Y-up = negative) */
    #define TP_LOOKAT_OFS   Q12(-0.85f)  /* Y offset for look target (Harry's chest) */
    #define TP_MOUSE_SENS     6          /* Q12 units per pixel for yaw */
    #define TP_PITCH_SENS     2          /* Q12 units per pixel for pitch */
    #define TP_STICK_DEADZONE 24         /* right-stick deadzone (of 128) */
    #define TP_STICK_YAW      40         /* per-30fps-frame yaw at full deflection */
    #define TP_STICK_PITCH    28         /* per-30fps-frame pitch at full deflection */
    #define TP_DIST_AIM       Q12(1.3f)  /* aim orbit radius at the 100% (default) zoom */
    /* Full-scale (200%) aim zoom pulls TWICE as far in as TP_DIST_AIM: the slider is
     * linear from TP_DIST (0%) to TP_DIST_AIM_MAX (200%), so 100% (the default) lands
     * exactly on TP_DIST_AIM (the original zoom). = 2*1.3 - 2.5 = Q12(0.1). */
    #define TP_DIST_AIM_MAX   (2 * TP_DIST_AIM - TP_DIST)

    s_SubCharacter* tp_hr = &g_SysWork.playerWork.player;
    int             isAiming;
    /* FPS head-follow: body-local head-bone offset captured on FPS entry, used to
     * isolate the idle-animation SWAY so the eye rides Harry's head, not his root. */
    static VECTOR3  s_fpsHeadRef;
    static int      s_fpsHeadRefValid = 0;
    /* FPS head-LOOK follow: the head bone's animated ROTATION (relative to the
     * body), low-passed to null its rest orientation so only the sway/lean turns
     * the view. Layered on top of the mouse look below. */
    static q3_12    s_fpsHeadYawRef   = 0;
    static q3_12    s_fpsHeadPitchRef = 0;
    static int      s_fpsHeadRotValid = 0;

    /* Zoom + OTS offset follow the AIM state only — NOT firing/attacking. Gating
     * on g_Player_IsAttacking too made the camera jarringly zoom in whenever the
     * player fired a shot, swung a melee weapon, or activated/examined something
     * (Cross) without aiming. The zoom is lerped below, so dropping it doesn't
     * snap during an aimed shot (isAiming stays held).
     * Also OR the raw aim-input flag: combat-state churn during rapid fire can
     * blip isAiming false for a frame, which visibly popped the zoom out even
     * though the player never released the aim button. */
    { extern u16 g_Player_IsAiming;
      isAiming = g_SysWork.playerCombat.isAiming || g_Player_IsAiming; }

    /* Aim zoom: ease the orbit distance in while aiming a gun, so the shot lines
     * up better. tps_aim_zoom config gates it (on by default). */
    static s32 s_tpDist = TP_DIST;
    static s32 s_otsOff = 0;   /* OTS lateral offset; also reset on mode entry */
    {
        extern int g_TpsCamNeedsReset;
        if (g_TpsCamNeedsReset)
        {
            /* First frame after entering TPS/OTS from classic: seed the orbit
             * behind Harry's current facing and clear the eased zoom/shoulder so
             * nothing pops in from the previous third-person session. */
            g_TpsCamNeedsReset = 0;
            g_TpsCamYaw   = tp_hr->rotation.vy;
            g_TpsCamPitch = 0;
            s_tpDist      = TP_DIST;
            s_otsOff      = 0;
        }
        /* tps_aim_zoom_amount scales how far in the dolly goes: 0% leaves the
         * camera at TP_DIST (no zoom), 100% (the default) lands on TP_DIST_AIM (the
         * original zoom), 200% goes all the way to TP_DIST_AIM_MAX (twice as far
         * in). Linear across the whole range. */
        s32 pct = (s32)(g_PcConfig.tpsAimZoom + 0.5f);
        s32 aimDist;
        s32 target;

        if (pct < 0)   pct = 0;
        if (pct > 200) pct = 200;

        aimDist = TP_DIST - (((TP_DIST - TP_DIST_AIM_MAX) * pct) / 200);
        target  = isAiming ? aimDist : TP_DIST;
        s_tpDist += (target - s_tpDist) >> 3;
    }

    /* Mouse + right stick: orbit the camera, decoupled from Harry's body. */
    {
        int mdx = 0, mdy = 0;
        s32 dPitch;
        s32 rx, ry;
        /* Console open: hold the camera perfectly still so the frozen frame shows
         * the exact view you were looking at. Still drain the relative-mouse
         * accumulator (below) so the view doesn't jump when the console closes. */
        extern int g_PcConsoleInputActive;
        int frozen = g_PcConsoleInputActive;

        SDL_GetRelativeMouseState(&mdx, &mdy);
        if (frozen) { mdx = 0; mdy = 0; }
        /* Mouse-RIGHT (mdx>0) → += yaw → view rotates right.
         * Mouse-UP (mdy<0) → pitch up by default; invert_mouse_y flips it. */
        {
            float ms = g_PcConfig.mouseSensitivity; /* 0.1..4.0, default 1.0 */
            g_TpsCamYaw   += (s32)(mdx * TP_MOUSE_SENS * ms);
            dPitch         = (s32)(mdy * TP_PITCH_SENS * ms);
        }
        g_TpsCamPitch += g_PcConfig.invertMouseY ? dPitch : -dPitch;

        /* Right stick (controller look parity). 0..255 centered at 128;
         * deadzone, then accumulate frame-rate-scaled. ry>0 = stick down →
         * look down by default; invert_controller_y flips it. */
        rx = (s32)g_Controller0->analogController.rightX - 128;
        ry = (s32)g_Controller0->analogController.rightY - 128;
        if (frozen) { rx = 0; ry = 0; }
        if (rx > -TP_STICK_DEADZONE && rx < TP_STICK_DEADZONE) rx = 0;
        if (ry > -TP_STICK_DEADZONE && ry < TP_STICK_DEADZONE) ry = 0;
        if (rx != 0 || ry != 0) {
            float cs   = g_PcConfig.controllerSensitivity; /* 0.1..4.0, default 1.0 */
            s32 sYaw   = TIMESTEP_SCALE_30_FPS(g_DeltaTime, (s32)(((rx * TP_STICK_YAW)   >> 7) * cs));
            s32 sPitch = TIMESTEP_SCALE_30_FPS(g_DeltaTime, (s32)(((ry * TP_STICK_PITCH) >> 7) * cs));
            g_TpsCamYaw   += sYaw;
            g_TpsCamPitch += g_PcConfig.invertControllerY ? sPitch : -sPitch;
        }

        /* Sticky aim-device detection for aim-assist: mouse motion -> mouse;
         * else any stick deflection (right = look, left = move) -> controller. */
        {
            s32 lx = (s32)g_Controller0->analogController.leftX - 128;
            s32 ly = (s32)g_Controller0->analogController.leftY - 128;
            if (mdx != 0 || mdy != 0)
                g_PcAimDevice = 0;
            else if (rx != 0 || ry != 0 ||
                     lx > TP_STICK_DEADZONE || lx < -TP_STICK_DEADZONE ||
                     ly > TP_STICK_DEADZONE || ly < -TP_STICK_DEADZONE)
                g_PcAimDevice = 1;
        }

        g_TpsCamYaw = Q12_ANGLE_NORM_U(g_TpsCamYaw + Q12_ANGLE(360.0f));
        /* Tighter clamp on the look-down side so the camera doesn't rise far
         * over Harry's head. */
        if (g_PcFpsCam) {
            /* First-person: allow looking down at the legs / up toward the sky,
             * but stay short of straight up/down so the forward vector + the
             * ratan2 in Vw_SetLookAtMatrix don't degenerate. */
            if (g_TpsCamPitch < -Q12_ANGLE(82.0f)) g_TpsCamPitch = -Q12_ANGLE(82.0f);
            if (g_TpsCamPitch >  Q12_ANGLE(82.0f)) g_TpsCamPitch =  Q12_ANGLE(82.0f);

            /* Yaw limit: you can mouse-look from straight-left to straight-right
             * (±90°) of Harry's BODY yaw, but not past his shoulders. When
             * moving or aiming the body snaps to the camera (player_control), so
             * the ±90° window rides along and turning is free; it only bites
             * while standing still — then the body catches up when you move. */
            {
                s32 yd = Math_AngleNormalizeSigned(g_TpsCamYaw - tp_hr->rotation.vy);
                if (yd >  Q12_ANGLE(90.0f))
                    g_TpsCamYaw = Q12_ANGLE_NORM_U(tp_hr->rotation.vy + Q12_ANGLE(90.0f) + Q12_ANGLE(360.0f));
                else if (yd < -Q12_ANGLE(90.0f))
                    g_TpsCamYaw = Q12_ANGLE_NORM_U(tp_hr->rotation.vy - Q12_ANGLE(90.0f) + Q12_ANGLE(360.0f));
            }
        } else {
            if (g_TpsCamPitch < -Q12_ANGLE(40.0f)) g_TpsCamPitch = -Q12_ANGLE(40.0f);
            if (g_TpsCamPitch >  Q12_ANGLE(50.0f)) g_TpsCamPitch =  Q12_ANGLE(50.0f);
        }
    }

    /* forward = (sin(yaw)*cos(pitch), -sin(pitch), cos(yaw)*cos(pitch))  Q12.
     * PSX -Y=up convention: pitch>0 (look up) → forward.y negative. */
    {
        /* First-person head-LOOK: fold Harry's animated head-bone rotation into
         * the view direction so the camera turns with his idle sway / melee lean
         * even when the mouse is still — the mouse look is layered on top. The
         * head bone's world orientation already contains the body yaw (it's a
         * child of the body), so express its forward relative to the body yaw to
         * get the head's local turn, then low-pass a reference to null the DC
         * rest orientation (same >>8 / tau ~4s as the position-sway ref below) so
         * the view rests neutral and only the sway/lean delta turns it. The
         * head-local +Z column is used as face-forward: for YAW this is exact
         * under any rest axis (a Y-rotation shifts every horizontal vector's yaw
         * equally, and the constant axis offset is removed by the reference). */
        s32 rearOfs;
        s32 viewYaw;
        s32 viewPitch = g_TpsCamPitch;
        /* Rear Look (held bind): swing the orbit 180 so the camera sits in front of
         * Harry and looks back past him. TPS/OTS only; FPS forces 0 (byte-identical). */
        {
            extern int g_PcRearLookActive;
            rearOfs = (g_PcRearLookActive && !g_PcFpsCam) ? Q12_ANGLE(180.0f) : 0;
        }
        viewYaw = g_TpsCamYaw + rearOfs;
        if (g_PcFpsCam && g_PcConfig.immersiveFpsHeadTracking)
        {
            const MATRIX* hm  = &g_SysWork.playerBoneCoords[HarryBone_Head].workm;
            s32   fX   = hm->m[0][2]; /* head-local +Z rotated to world, Q12 */
            s32   fY   = hm->m[1][2];
            s32   fZ   = hm->m[2][2];
            s32   hmag = SquareRoot0(SQUARE(fX) + SQUARE(fZ));
            q3_12 headYawW   = ratan2(fX, fZ);
            q3_12 headPitchW = ratan2(-fY, hmag);
            q3_12 headYawL   = Math_AngleNormalizeSigned(headYawW - tp_hr->rotation.vy);

            if (!s_fpsHeadRotValid)
            {
                if (g_GameWork.gameState == GameState_InGame &&
                    g_SysWork.sysState   == SysState_Gameplay)
                {
                    s_fpsHeadYawRef   = headYawL;
                    s_fpsHeadPitchRef = headPitchW;
                    s_fpsHeadRotValid = 1;
                }
            }
            else
            {
                s32 dYaw, dPitch, gain;

                /* Settle delay: Harry's idle head sway begins the instant he stops,
                 * so locking the view onto it immediately feels aggressive. Ease
                 * the follow in over a few seconds of standing still — the body
                 * animates normally, only the CAMERA's response is delayed. Any
                 * movement resets the timer; the reference keeps low-passing so the
                 * delta stays small when the gain finally rises (no pop). */
                #define FPS_LOOK_DELAY_MS 1500u  /* fully off for this long after stopping */
                #define FPS_LOOK_RAMP_MS  3500u  /* fully on by here (ramp over the gap) */
                {
                    static Uint32 s_lastMoveMs = 0;
                    Uint32 now   = SDL_GetTicks();
                    s32    held2 = g_Controller0->heldBtnFlags;
                    int    moving = (g_sdlKeyboardState[SDL_SCANCODE_W] != 0) ||
                                    (g_sdlKeyboardState[SDL_SCANCODE_A] != 0) ||
                                    (g_sdlKeyboardState[SDL_SCANCODE_S] != 0) ||
                                    (g_sdlKeyboardState[SDL_SCANCODE_D] != 0) ||
                                    (held2 & (ControllerFlag_LStickUp   | ControllerFlag_LStickDown  |
                                              ControllerFlag_LStickLeft | ControllerFlag_LStickRight |
                                              ControllerFlag_DpadUp     | ControllerFlag_DpadDown    |
                                              ControllerFlag_DpadLeft   | ControllerFlag_DpadRight));
                    Uint32 still;
                    if (moving) s_lastMoveMs = now;
                    still = now - s_lastMoveMs;
                    if (still <= FPS_LOOK_DELAY_MS)      gain = 0;
                    else if (still >= FPS_LOOK_RAMP_MS)  gain = Q12(1.0f);
                    else gain = (s32)(((s64)(still - FPS_LOOK_DELAY_MS) << 12) /
                                      (FPS_LOOK_RAMP_MS - FPS_LOOK_DELAY_MS));
                }
                #undef FPS_LOOK_DELAY_MS
                #undef FPS_LOOK_RAMP_MS

                s_fpsHeadYawRef   += Math_AngleNormalizeSigned(headYawL   - s_fpsHeadYawRef)   >> 8;
                s_fpsHeadPitchRef += Math_AngleNormalizeSigned(headPitchW - s_fpsHeadPitchRef) >> 8;

                dYaw   = Math_AngleNormalizeSigned(headYawL   - s_fpsHeadYawRef);
                dPitch = Math_AngleNormalizeSigned(headPitchW - s_fpsHeadPitchRef);
                viewYaw   += (s32)(((s64)dYaw   * gain) >> 12);
                viewPitch += (s32)(((s64)dPitch * gain) >> 12);

                /* Safety: keep the composed pitch short of straight up/down so the
                 * ratan2 in Vw_SetLookAtMatrix doesn't degenerate. */
                if (viewPitch >  Q12_ANGLE(87.0f)) viewPitch =  Q12_ANGLE(87.0f);
                if (viewPitch < -Q12_ANGLE(87.0f)) viewPitch = -Q12_ANGLE(87.0f);
            }
        }
        else
        {
            /* Immersive head-tracking off (or non-FPS): re-seed the sway
             * reference next time it's enabled so the view doesn't jerk from a
             * stale frozen reference. */
            s_fpsHeadRotValid = 0;
        }

        s32 sy = Math_Sin(viewYaw);
        s32 cy = Math_Cos(viewYaw);
        s32 sp = Math_Sin(viewPitch);
        s32 cp = Math_Cos(viewPitch);

        s32 fwdX = (s32)((s64)sy * cp >> 12);
        s32 fwdY = -sp;
        s32 fwdZ = (s32)((s64)cy * cp >> 12);

        VECTOR3 tpCamPos, tpLookAt;
        s32     anchorY;
        #define TP_LOOKAT_DIST Q12(25.0f)

        if (!g_PcFpsCam)
        {
            s_fpsHeadRefValid = 0; /* re-capture the head-sway baseline on next FPS entry */
            s_fpsHeadRotValid = 0; /* re-seed the head-LOOK baseline on next FPS entry */
            s_FpsSwingPull       = 0; /* a stale pulled state would flash the head interior on FPS re-entry */
            g_PcFpsSwingHeadShow = 0;
        }

        if (g_PcFpsCam)
        {
            /* First-person: eye = Harry's root + the local between-the-arms offset,
             * rotated by Harry's BODY yaw (rotation.vy) — the SAME frame the L-key
             * logs the offset in, so a captured value reproduces the spot exactly
             * and each numpad axis is a fixed straight-line nudge (no orbit).
             * lookAt = eye + forward (forward still uses camYaw/pitch below — the
             * view direction is the mouse, the eye POSITION rides the body). */
            s32     eyeYaw = tp_hr->rotation.vy;
            s32     sYaw   = Math_Sin(eyeYaw);
            s32     cYaw   = Math_Cos(eyeYaw);
            VECTOR3 eyeLocal = g_PcFpsOffset;

            /* Head-follow: ride Harry's animated head bone so the view breathes and
             * sways with his idle animation instead of his body sliding out from
             * under a root-anchored eye. Take the head bone's body-local offset (its
             * world pos, Q8->Q12, inverse-rotated by body yaw), subtract the value
             * captured on FPS entry to isolate just the SWAY, and add it to the
             * tuned eye offset — so the baseline stays exactly the tuned spot. */
            {
                s32     hdx = Q8_TO_Q12(g_SysWork.playerBoneCoords[HarryBone_Head].workm.t[0]) - tp_hr->position.vx;
                s32     hdy = Q8_TO_Q12(g_SysWork.playerBoneCoords[HarryBone_Head].workm.t[1]) - tp_hr->position.vy;
                s32     hdz = Q8_TO_Q12(g_SysWork.playerBoneCoords[HarryBone_Head].workm.t[2]) - tp_hr->position.vz;
                VECTOR3 headLocal;
                headLocal.vx = (s32)(((s64)hdx * cYaw - (s64)hdz * sYaw) >> 12);
                headLocal.vz = (s32)(((s64)hdx * sYaw + (s64)hdz * cYaw) >> 12);
                headLocal.vy = hdy;

                /* Seed the sway reference from settled gameplay (not the load/spawn
                 * pose), THEN low-pass it toward the head's running mean every frame.
                 * A FROZEN reference stays biased by whatever single pose it captured
                 * (never the idle mean), so the resting eye is off by a constant that
                 * differs per install — the recurring "FPS camera is off on a fresh
                 * install" bug. A TRACKING reference always re-centres, so the eye's
                 * resting position converges to the tuned baseline deterministically
                 * on every install; fast head motion (walk/turn/idle sway) still shows
                 * through, only the slow DC drift is removed. The time constant must
                 * stay WELL above the ~0.5-0.7s melee aim/swing lean, or the reference
                 * tracks the lean and cancels it — the eye must ride the head fully
                 * through an aim/swing. >>8 (tau ~4s @60fps) passes the lean and idle
                 * breathing while still nulling multi-second drift. */
                if (!s_fpsHeadRefValid)
                {
                    if (g_GameWork.gameState == GameState_InGame &&
                        g_SysWork.sysState   == SysState_Gameplay)
                    {
                        s_fpsHeadRef      = headLocal;
                        s_fpsHeadRefValid = 1;
                    }
                }
                else
                {
                    s_fpsHeadRef.vx += (headLocal.vx - s_fpsHeadRef.vx) >> 8;
                    s_fpsHeadRef.vy += (headLocal.vy - s_fpsHeadRef.vy) >> 8;
                    s_fpsHeadRef.vz += (headLocal.vz - s_fpsHeadRef.vz) >> 8;
                }

                if (s_fpsHeadRefValid)
                {
                    eyeLocal.vx += headLocal.vx - s_fpsHeadRef.vx;
                    eyeLocal.vy += headLocal.vy - s_fpsHeadRef.vy;
                    eyeLocal.vz += headLocal.vz - s_fpsHeadRef.vz;
                }

                {
                    extern VECTOR3 g_PcFpsHeadRefDbg, g_PcFpsHeadLocalDbg;
                    g_PcFpsHeadRefDbg   = s_fpsHeadRef;
                    g_PcFpsHeadLocalDbg = headLocal;
                }
            }

            tpCamPos.vx = tp_hr->position.vx + (s32)((s64)eyeLocal.vz * sYaw >> 12)
                                             + (s32)((s64)eyeLocal.vx * cYaw >> 12);
            tpCamPos.vz = tp_hr->position.vz + (s32)((s64)eyeLocal.vz * cYaw >> 12)
                                             - (s32)((s64)eyeLocal.vx * sYaw >> 12);
            tpCamPos.vy = tp_hr->position.vy + eyeLocal.vy;

            /* Melee arm clearance: with the eye in the head, raise/swing poses put
             * Harry's forearms right across the camera. Dolly the eye straight back
             * along the view axis by how much the nearest forearm/hand bone crowds
             * it — the camera backs off as the arms come up and eases home as the
             * swing carries them away, self-timed for every weapon's animation.
             * Gated to melee/unarmed attack input (guns don't raise into the face);
             * proximity keeps it inert while the arms are down. A ray toward the
             * pulled position keeps the dolly out of level geometry behind the eye. */
            {
                #define SWING_PULL_NEAR Q12(0.55f) /* arm distance where the dolly starts */
                #define SWING_PULL_MAX  Q12(0.50f) /* dolly cap */
                #define SWING_PULL_WALL Q12(0.15f) /* keep-out margin from level geometry */
                s32 target = 0;

                if (g_SysWork.playerCombat.weaponAttack != NO_VALUE &&
                    g_SysWork.playerCombat.weaponAttack < WEAPON_ATTACK(EquippedWeaponId_Handgun, AttackInputType_Tap))
                {
                    static const u8 ARM_BONES[4] = { HarryBone_LeftForearm, HarryBone_LeftHand,
                                                     HarryBone_RightForearm, HarryBone_RightHand };
                    s32 minDist = 0x7FFFFFFF;
                    s32 i;

                    for (i = 0; i < 4; i++)
                    {
                        const MATRIX* bm = &g_SysWork.playerBoneCoords[ARM_BONES[i]].workm;
                        s32 dx = Q8_TO_Q12(bm->t[0]) - tpCamPos.vx;
                        s32 dy = Q8_TO_Q12(bm->t[1]) - tpCamPos.vy;
                        s32 dz = Q8_TO_Q12(bm->t[2]) - tpCamPos.vz;
                        s32 d  = SquareRoot0(SQUARE(dx) + SQUARE(dy) + SQUARE(dz));
                        if (d < minDist) minDist = d;
                    }

                    if (minDist < SWING_PULL_NEAR)
                    {
                        /* 1.5x gain: bone origins (elbow/wrist) sit past the mesh
                         * surface that actually fills the view. */
                        target = (SWING_PULL_NEAR - minDist) + ((SWING_PULL_NEAR - minDist) >> 1);
                        if (target > SWING_PULL_MAX) target = SWING_PULL_MAX;
                    }
                }

                /* Ease: quick to extend (beat the raise), gentler to come home.
                 * Snap the tail so the timestep-scaled integer step can't stall. */
                {
                    s32 d = target - s_FpsSwingPull;
                    s_FpsSwingPull += TIMESTEP_SCALE_30_FPS(g_DeltaTime, (d > 0) ? (d >> 1) : (d >> 2));
                    d = target - s_FpsSwingPull;
                    if (d < 0) d = -d;
                    if (d < 24) s_FpsSwingPull = target;
                }

                /* Head visibility follows the ACTUAL dolly distance, not the swing
                 * state: below the threshold the camera is still inside the head.
                 * Hysteresis so it can't flicker around the edge. */
                if (s_FpsSwingPull > Q12(0.32f))
                    g_PcFpsSwingHeadShow = 1;
                else if (s_FpsSwingPull < Q12(0.22f))
                    g_PcFpsSwingHeadShow = 0;

                if (s_FpsSwingPull > 0)
                {
                    s32 pull = s_FpsSwingPull;
                    s_RayTrace pullTrace;
                    VECTOR3    back;
                    back.vx = tpCamPos.vx - (s32)((s64)(pull + SWING_PULL_WALL) * fwdX >> 12);
                    back.vy = tpCamPos.vy - (s32)((s64)(pull + SWING_PULL_WALL) * fwdY >> 12);
                    back.vz = tpCamPos.vz - (s32)((s64)(pull + SWING_PULL_WALL) * fwdZ >> 12);
                    if (Ray_TraceQuery(&pullTrace, &tpCamPos, &back) && pullTrace.hasHit)
                    {
                        s32 safe = pullTrace.hitDistance - SWING_PULL_WALL;
                        if (safe < 0)    safe = 0;
                        if (safe < pull) pull = safe;
                    }
                    tpCamPos.vx -= (s32)((s64)pull * fwdX >> 12);
                    tpCamPos.vy -= (s32)((s64)pull * fwdY >> 12);
                    tpCamPos.vz -= (s32)((s64)pull * fwdZ >> 12);
                }
                #undef SWING_PULL_NEAR
                #undef SWING_PULL_MAX
                #undef SWING_PULL_WALL
            }

            g_PcCamAppliedPos   = tpCamPos;
            g_PcCamAppliedValid = 1;
            /* Publish the view-forward (world Q12) + eye pos so the flashlight can
             * aim where the player looks, from the eye, in FPS — see func_800554C4. */
            g_PcFpsViewFwd.vx = fwdX;
            g_PcFpsViewFwd.vy = fwdY;
            g_PcFpsViewFwd.vz = fwdZ;
            g_PcFpsEyePos     = tpCamPos;
            tpLookAt.vx = tpCamPos.vx + (s32)((s64)TP_LOOKAT_DIST * fwdX >> 12);
            tpLookAt.vy = tpCamPos.vy + (s32)((s64)TP_LOOKAT_DIST * fwdY >> 12);
            tpLookAt.vz = tpCamPos.vz + (s32)((s64)TP_LOOKAT_DIST * fwdZ >> 12);
        }
        else
        {
            /* Camera D units BACK along forward, lifted by TP_HEIGHT */
            tpCamPos.vx = tp_hr->position.vx - (s32)((s64)s_tpDist * fwdX >> 12);
            tpCamPos.vy = tp_hr->position.vy - (s32)((s64)s_tpDist * fwdY >> 12) + TP_HEIGHT;
            tpCamPos.vz = tp_hr->position.vz - (s32)((s64)s_tpDist * fwdZ >> 12);

            /* lookAt projects FAR ahead (anti-jitter), Y-anchored to Harry's chest
             * so the screen-center crosshair lands on him, biased by pitch. */
            anchorY     = tp_hr->position.vy + TP_LOOKAT_OFS;
            tpLookAt.vx = tpCamPos.vx + (s32)((s64)TP_LOOKAT_DIST * fwdX >> 12);
            tpLookAt.vy = anchorY     + (s32)((s64)TP_LOOKAT_DIST * fwdY >> 12);
            tpLookAt.vz = tpCamPos.vz + (s32)((s64)TP_LOOKAT_DIST * fwdZ >> 12);
        }
        #undef TP_LOOKAT_DIST

        /* Over-the-Shoulder: shift the camera + look target laterally so Harry
         * sits to one side; more while aiming. g_OtsSide (middle-mouse) flips it.
         *
         * tps_ots_aim (on by default) gives plain Thirdperson the same shoulder
         * framing WHILE AIMING ONLY: its resting offset is 0, so the camera eases
         * over Harry's shoulder as he raises the gun and back to centre as he
         * lowers it. The branch is entered every frame in that mode — not only
         * while aiming — precisely so s_otsOff can ease both ways instead of
         * snapping. With the option off, Thirdperson never enters it and the
         * camera stays centred exactly as before. */
        if (g_ControlStyle == ControlStyle_Ots ||
            (g_ControlStyle == ControlStyle_Tps && g_PcConfig.tpsOtsAim))
        {
            #define OTS_OFFSET     Q12(0.55f)
            #define OTS_OFFSET_AIM Q12(0.9f)
            s32 restOff   = (g_ControlStyle == ControlStyle_Ots) ? OTS_OFFSET : 0;
            s32 targetOff = (isAiming ? OTS_OFFSET_AIM : restOff) * g_OtsSide;
            s32 rX = Math_Cos(g_TpsCamYaw + rearOfs);   /* horizontal right vector = (cos yaw, -sin yaw); +rearOfs flips the shoulder with Rear Look */
            s32 rZ = -Math_Sin(g_TpsCamYaw + rearOfs);
            s32 ox, oz;

            s_otsOff += (targetOff - s_otsOff) >> 3;
            ox = (s32)((s64)s_otsOff * rX >> 12);
            oz = (s32)((s64)s_otsOff * rZ >> 12);
            tpCamPos.vx += ox; tpCamPos.vz += oz;
            tpLookAt.vx += ox; tpLookAt.vz += oz;
            #undef OTS_OFFSET
            #undef OTS_OFFSET_AIM
        }

#ifdef SH_PC_PORT
        /* Third-person camera-wall collision: keep the eye from clipping through
         * level geometry. Cast from Harry's head out to the computed eye; on a wall
         * hit, pull the eye in along that line to just short of the wall. TPS/OTS
         * only — the FPS eye already sits at Harry's head.
         *
         * The pivot sits at HEAD height, not the chest look-anchor. Pulling the eye
         * toward the chest dropped it to waist level against a close wall and framed
         * Harry's back/backside — the "ass camera", worst on room entry where the
         * persisted orbit aims the eye straight into the wall behind him. From the
         * head anchor the eye instead stays level with his head as it comes in, and
         * the look target is lifted toward the head in step with the pull (lookRaiseY)
         * so the close shot centers on his head rather than tilting down at his chest.
         * At the full orbit distance the lift is 0, so the normal framing is unchanged.
         *
         * The pull-in is computed whenever we are not in FPS, into aimEye. With
         * tps_camera_collision = 1 (default) it also becomes the render eye. With 0 the
         * render eye keeps its full orbit distance and is allowed through geometry —
         * what that option asks for — and aimEye survives only to keep the free-aim ray
         * out of the wall (see the publish below). */
        VECTOR3 aimEye = tpCamPos;
        s32     lookRaiseY = 0;   /* upward look-target lift toward the head, applied with the pull-in */
        if (!g_PcFpsCam)
        {
            #define CAM_COLL_MARGIN Q12(0.25f)
            #define CAM_COLL_MIN    Q12(0.35f)
            #define TP_HEAD_OFS     Q12(-1.55f)  /* Harry's head height above his root (Y-up = negative) */
            s_RayTrace camTrace;
            VECTOR3    pivot;
            s32        headAnchorY = tp_hr->position.vy + TP_HEAD_OFS;
            pivot.vx = tp_hr->position.vx;
            pivot.vy = headAnchorY;
            pivot.vz = tp_hr->position.vz;

            if (Ray_TraceQuery(&camTrace, &pivot, &tpCamPos) && camTrace.hasHit)
            {
                s32 dx  = tpCamPos.vx - pivot.vx;
                s32 dy  = tpCamPos.vy - pivot.vy;
                s32 dz  = tpCamPos.vz - pivot.vz;
                s32 ax  = dx >> 6, ay = dy >> 6, az = dz >> 6;
                s32 full = SquareRoot0(SQUARE(ax) + SQUARE(ay) + SQUARE(az)) << 6;
                if (full > 0 && camTrace.hitDistance < full)
                {
                    s32 safe = camTrace.hitDistance - CAM_COLL_MARGIN;
                    s32 frac;
                    s32 pullT;
                    if (safe < CAM_COLL_MIN) { safe = CAM_COLL_MIN; }
                    frac = (s32)(((s64)safe << 12) / full);
                    aimEye.vx = pivot.vx + (s32)(((s64)dx * frac) >> 12);
                    aimEye.vy = pivot.vy + (s32)(((s64)dy * frac) >> 12);
                    aimEye.vz = pivot.vz + (s32)(((s64)dz * frac) >> 12);

                    /* pullT: 0 at the full orbit distance, 1 when pulled to the
                     * minimum. Lift the look target from the chest anchor toward the
                     * head by that fraction, so the framing rises with the eye. */
                    pullT = Q12(1.0f) - frac;
                    if (pullT < 0)         pullT = 0;
                    if (pullT > Q12(1.0f)) pullT = Q12(1.0f);
                    lookRaiseY = (s32)(((s64)(headAnchorY - anchorY) * pullT) >> 12);
                }
            }
            #undef CAM_COLL_MARGIN
            #undef CAM_COLL_MIN
            #undef TP_HEAD_OFS

            if (g_PcConfig.tpsCameraCollision)
            {
                tpCamPos     = aimEye;
                tpLookAt.vy += lookRaiseY;
            }
        }

        /* Publish the camera eye + forward for free-aim (set AFTER the OTS lateral
         * offset so the eye matches the rendered view). The aim ray in
         * Player_CombatUpdate is cast from g_TpsCamPos along g_TpsCamFwd.
         *
         * Forward must be the ACTUAL view direction (render eye -> lookAt), NOT the raw
         * orbit forward (fwdX/Y/Z): tpLookAt.vy is anchored to Harry's chest, so the
         * screen-center ray has a different PITCH than the orbit forward. Publishing
         * the orbit forward made the aim ray (and the bullet) miss screen-center
         * vertically. unit(lookAt - eye) passes through the reticle at every depth. */
        g_TpsCamPos = tpCamPos;
        {
            s32 dx  = tpLookAt.vx - tpCamPos.vx;
            s32 dy  = tpLookAt.vy - tpCamPos.vy;
            s32 dz  = tpLookAt.vz - tpCamPos.vz;
            s32 ax  = dx >> 6, ay = dy >> 6, az = dz >> 6; /* avoid SQUARE overflow */
            s32 mag = SquareRoot0(SQUARE(ax) + SQUARE(ay) + SQUARE(az));
            if (mag > 0)
            {
                g_TpsCamFwd.vx = (s32)(((s64)ax << 12) / mag);
                g_TpsCamFwd.vy = (s32)(((s64)ay << 12) / mag);
                g_TpsCamFwd.vz = (s32)(((s64)az << 12) / mag);
            }
            else
            {
                g_TpsCamFwd.vx = fwdX;
                g_TpsCamFwd.vy = fwdY;
                g_TpsCamFwd.vz = fwdZ;
            }
        }

        /* tps_camera_collision = 0: the render eye is allowed through walls, but the
         * shot must not be. Player_CombatUpdate traces this ray against level geometry
         * with a DOUBLE-SIDED surface test, so an origin sitting behind a wall hits
         * that wall first and flips the shot ~180 degrees back into it — Harry would
         * fire backwards whenever he backed into a corner.
         *
         * Slide the origin forward ALONG THE VIEW LINE by however far the pull-in would
         * have moved the eye. Using the pulled-in point itself as the origin is wrong:
         * the pull runs along pivot->eye, which is ~11 degrees off the view direction
         * (tpLookAt.vy is anchored to Harry's chest, not to the eye) and is measured
         * from an un-shifted pivot, so it also cancels part of the OTS shoulder offset.
         * That point is off the reticle line, and a ray from it is PARALLEL to the line
         * the player is aiming along — a constant miss at every range. Projecting the
         * displacement onto g_TpsCamFwd keeps origin and reticle collinear.
         *
         * No-op when collision is on (aimEye == tpCamPos) and in FPS (block skipped). */
        if (!g_PcFpsCam && !g_PcConfig.tpsCameraCollision)
        {
            s32 dx = aimEye.vx - tpCamPos.vx;
            s32 dy = aimEye.vy - tpCamPos.vy;
            s32 dz = aimEye.vz - tpCamPos.vz;
            s64 t  = ((s64)dx * g_TpsCamFwd.vx +
                      (s64)dy * g_TpsCamFwd.vy +
                      (s64)dz * g_TpsCamFwd.vz) >> 12; /* Q12 distance along the view line */
            if (t > 0)
            {
                g_TpsCamPos.vx = tpCamPos.vx + (s32)((t * g_TpsCamFwd.vx) >> 12);
                g_TpsCamPos.vy = tpCamPos.vy + (s32)((t * g_TpsCamFwd.vy) >> 12);
                g_TpsCamPos.vz = tpCamPos.vz + (s32)((t * g_TpsCamFwd.vz) >> 12);
            }
        }
#endif
        Vw_SetLookAtMatrix(&tpCamPos, &tpLookAt);
        vwSetViewInfo();
    }

    Pc_CameraFov_Update();

    #undef TP_DIST
    #undef TP_DIST_AIM
    #undef TP_DIST_AIM_MAX
    #undef TP_HEIGHT
    #undef TP_LOOKAT_OFS
    #undef TP_MOUSE_SENS
    #undef TP_PITCH_SENS
    #undef TP_STICK_DEADZONE
    #undef TP_STICK_YAW
    #undef TP_STICK_PITCH
}

/* Auto-repeat with acceleration for the keyframe-inspector , / . keys: steps
 * once on the press edge, then after a short delay repeats at an accelerating
 * rate, ramping from ~4/s up to a 10/s top speed the longer the key is held.
 * pressMs/lastMs are per-key static timers. Returns 1 on the frames it fires. */
static int Kf_HoldRepeat(int cur, int prev, Uint32* pressMs, Uint32* lastMs)
{
    Uint32 now = SDL_GetTicks();

    if (cur && !prev) { /* fresh press: fire immediately */
        *pressMs = now;
        *lastMs  = now;
        return 1;
    }
    if (cur && prev) { /* held */
        Uint32 held = now - *pressMs;
        if (held >= 350) { /* initial delay before auto-repeat kicks in */
            int interval = 250 - (int)((held - 350) / 10); /* 250ms -> 100ms over ~1.5s */
            if (interval < 100) interval = 100;            /* 100ms = 10/s top speed */
            if ((now - *lastMs) >= (Uint32)interval) {
                *lastMs = now;
                return 1;
            }
        }
    }
    return 0;
}

void DebugCamera_Update(void)
{
    #define DBG_CAM_MOVE_SPEED 128   /* Q12(0.03125) — slow enough to dial in the FPS-cam spot */
    #define DBG_CAM_TURN_SPEED 6
    #define DBG_CAM_VERT_SPEED 64

    if (!g_sdlKeyboardState) return;
#ifdef SH_PC_PORT
    /* Master gate for all dev/cheat keys (numpad cam, top-row digits, give-
     * weapon cheats, kill-Harry, noclip, etc.). Off unless allow_debug_controls
     * is set in config. */
    {
        extern int g_PcAllowDebugControls;
        if (!g_PcAllowDebugControls) {
            /* TPS is a normal (non-debug) camera now: apply it even with dev
             * keys off, then skip the dev-key handlers below. Classic just
             * lets the game camera stand. */
            if (g_GameWork.gameState == GameState_InGame && !g_DebugCamEnabled && g_DebugThirdPersonCam)
                Pc_TpsCamera_Apply();
            return;
        }
    }
    /* Console input mode: typed characters land on the same top-row keys the
     * debug binds use (0-9, -, =), and the controller suppression doesn't
     * cover these direct SDL reads — block them all while typing. */
    {
        extern int g_PcConsoleInputActive;
        if (g_PcConsoleInputActive) {
            /* Keep the alternate (TPS/OTS/FPS) camera applied while the console
             * is open, so the frozen frame shows the exact view you were looking
             * at instead of snapping back to the default game camera. The look
             * input is held still inside Pc_TpsCamera_Apply (frozen), so the
             * angle doesn't drift while you type. */
            if (g_GameWork.gameState == GameState_InGame && !g_DebugCamEnabled && g_DebugThirdPersonCam)
                Pc_TpsCamera_Apply();
            return;
        }
    }
#endif
    if (g_GameWork.gameState != GameState_InGame) return;

    /* (Esc warm-reboot / title quit moved to DbgOverlay_Update so it works without
     * debug controls and in every game state, including the title menu.) */

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
    /* (Third-person camera toggle moved out of debug controls: it's now the
     * rebindable Change-Camera action / control_style config, handled every
     * frame by Pc_ControlStyleUpdate.) */

    /* Kill Harry moved to the `kill` console command (was number key 1). */
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

    /* Number key 6: kill every active enemy near Harry (debug). Each enemy's own
     * update applies damage.amount to its health (health = MAX(health - amount, 0))
     * and then runs its normal death path, so forcing a huge damage.amount routes
     * the kill through each enemy's real death/cleanup — works for every type, and
     * the value clears all per-enemy damage thresholds (e.g. Creeper needs >=200).
     * Replaces the old (non-working) Grey Child spawn. */
    {
        static int prevKey6 = 0;
        int cur6 = g_sdlKeyboardState[SDL_SCANCODE_6];
        if (cur6 && !prevKey6) {
            s_SubCharacter* hr   = &g_SysWork.playerWork.player;
            s32             killed = 0;
            s32             i;
            for (i = 0; i < NPC_COUNT_MAX; i++) {
                s_SubCharacter* npc = &g_SysWork.npcs[i];
                if (npc->model.charaId == Chara_None || npc->model.charaId == Chara_Harry ||
                    npc->health <= Q12(0.0f)) {
                    continue;
                }
                if (ABS(npc->position.vx - hr->position.vx) > Q12(50.0f) ||
                    ABS(npc->position.vz - hr->position.vz) > Q12(50.0f)) {
                    continue;
                }
                npc->damage.amount = Q12(99999.0f);
                killed++;
            }
            SH_DBG_ECHO("[DEBUG] Key 6: killed %d nearby enemies", (int)killed);
        }
        prevKey6 = cur6;
    }

    /* Top-row 7: toggle god mode (same shared g_PcGodMode flag as the `god` console cmd) */
    {
        static int prevKey = 0;
        int cur = g_sdlKeyboardState[SDL_SCANCODE_7];
        if (cur && !prevKey) {
            g_PcGodMode = !g_PcGodMode;
            Sd_PlaySfx(g_PcGodMode ? Sfx_MenuConfirm : Sfx_MenuCancel, 0, 64);
            SH_DBG_ECHO("[DEBUG] Key 7: Invincibility: %s", g_PcGodMode ? "ON" : "OFF");
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

    /* Keyframe inspector: K toggles freezing Harry's whole skeleton on one
     * absolute keyframe; , / . step the keyframe down / up. Used to find the
     * exact authored pose index for the aim shim (e.g. the gun-forward frame).
     * The actual pose override + clamp to the anim header's keyframe count live
     * in Player_Update (player_control.c); here we just drive the index. */
    {
        static int    prevK = 0, prevComma = 0, prevPeriod = 0;
        static Uint32 commaPress = 0, commaLast = 0, periodPress = 0, periodLast = 0;
        int curK      = g_sdlKeyboardState[SDL_SCANCODE_K];
        int curComma  = g_sdlKeyboardState[SDL_SCANCODE_COMMA];
        int curPeriod = g_sdlKeyboardState[SDL_SCANCODE_PERIOD];
        if (curK && !prevK) {
            g_DebugAnimKfView = !g_DebugAnimKfView;
            if (!g_DebugAnimKfView) g_DebugAnimPlaying = 0;
            Sd_PlaySfx(g_DebugAnimKfView ? Sfx_MenuConfirm : Sfx_MenuCancel, 0, 64);
            SH_DBG_ECHO("[DEBUG] K: Keyframe view: %s (KF %d)",
                        g_DebugAnimKfView ? "ON" : "OFF", g_DebugAnimKf);
        }
        if (g_DebugAnimKfView) {
            /* Hold , / . to scroll, accelerating up to 10/s the longer it's held.
             * Any manual scrub also stops loop playback. */
            if (Kf_HoldRepeat(curComma, prevComma, &commaPress, &commaLast)) {
                if (g_DebugAnimKf > 0) g_DebugAnimKf--;
                g_DebugAnimPlaying = 0;
            }
            if (Kf_HoldRepeat(curPeriod, prevPeriod, &periodPress, &periodLast)) {
                g_DebugAnimKf++;
                g_DebugAnimPlaying = 0;
            }
            /* Echo on a fresh tap or on release (the landed frame) only — the amber
             * panel is the live readout, so a held fast-scroll doesn't spam the log. */
            if ((curComma && !prevComma) || (curPeriod && !prevPeriod) ||
                (!curComma && prevComma) || (!curPeriod && prevPeriod)) {
                SH_DBG_ECHO("[DEBUG] KF %d", g_DebugAnimKf);
            }
        }
        prevK      = curK;
        prevComma  = curComma;
        prevPeriod = curPeriod;
    }

    /* `/` while the inspector is on: cycle the equipped weapon's UPPER-BODY anims
     * (HARRY_BASE_ANIM_INFOS entries 56..75 = anim indices 28..37: aim / fire /
     * recoil / reload — these are overwritten per equipped weapon by
     * GameFs_WeaponInfoUpdate, so EQUIP THE WEAPON FIRST). Jumps straight to the
     * next weapon-anim start keyframe so you land on the gun/aim poses instead of
     * stepping through every base movement anim. The base movement anims sit at
     * lower keyframes — reach them by scrubbing , / . . If no weapon anims are
     * loaded (unarmed), `/` falls back to cycling ALL anim starts. */
    {
        static int prevSlash = 0;
        int curSlash = g_sdlKeyboardState[SDL_SCANCODE_SLASH];
        if (curSlash && !prevSlash && g_DebugAnimKfView) {
            int i, lo, hi, haveWeaponAnims = 0;
            int best    = -1;   /* smallest start keyframe strictly above current */
            int wrapMin = -1;   /* smallest start keyframe overall (for wrap)      */
            for (i = 56; i < 76; i++) {
                if (HARRY_BASE_ANIM_INFOS[i].startKeyframeIdx >= 0) { haveWeaponAnims = 1; break; }
            }
            lo = haveWeaponAnims ? 56 : 0;
            hi = haveWeaponAnims ? 76 : 256;
            for (i = lo; i < hi; i++) {
                int sk = HARRY_BASE_ANIM_INFOS[i].startKeyframeIdx;
                if (sk < 0) continue; /* NO_VALUE blend entries */
                if (wrapMin < 0 || sk < wrapMin) wrapMin = sk;
                if (sk > g_DebugAnimKf && (best < 0 || sk < best)) best = sk;
            }
            if (best < 0) best = wrapMin;
            if (best >= 0) {
                g_DebugAnimKf = best;
                g_DebugAnimPlaying = 0;
                SH_DBG_ECHO("[DEBUG] / %s anim start: KF %d",
                            haveWeaponAnims ? "weapon" : "base", g_DebugAnimKf);
            }
        }
        prevSlash = curSlash;
    }

    /* N while the inspector is on: cycle the viewed character among Harry (-1)
     * and the loaded NPCs (g_SysWork.npcs slots with a real charaId). Resets the
     * keyframe + stops playback so scrubbing restarts in the new target's space. */
    {
        static int prevN = 0;
        int curN = g_sdlKeyboardState[SDL_SCANCODE_N];
        if (curN && !prevN && g_DebugAnimKfView) {
            int i, next = -1;
            for (i = g_DebugViewNpcSlot + 1; i < NPC_COUNT_MAX; i++) {
                if (g_SysWork.npcs[i].model.charaId != Chara_None &&
                    g_SysWork.npcs[i].model.charaId != Chara_Padlock) { next = i; break; }
            }
            g_DebugViewNpcSlot = next;
            g_DebugAnimPlaying = 0;
            if (next < 0) {
                g_DebugAnimKf = 588;
                SH_DBG_ECHO("[DEBUG] N: view Harry (KF %d)", g_DebugAnimKf);
            } else {
                g_DebugAnimKf = 0;
                SH_DBG_ECHO("[DEBUG] N: view NPC slot %d (chara %d)", next,
                            (int)g_SysWork.npcs[next].model.charaId);
            }
        }
        prevN = curN;
    }

    /* P while the inspector is on: PLAY — loop the anim that contains the current
     * keyframe, from the active target's table: Harry's HARRY_BASE_ANIM_INFOS, or
     * a viewed NPC's own baseAnimInfos. Press again to stop; , / . or / resume
     * manual scrubbing. The looping pose is driven in Player_Update / Game_NpcUpdate. */
    {
        static int prevP = 0;
        int curP = g_sdlKeyboardState[SDL_SCANCODE_P];
        if (curP && !prevP && g_DebugAnimKfView) {
            if (!g_DebugAnimPlaying) {
                s_AnimInfo* tbl = HARRY_BASE_ANIM_INFOS;
                int count = 76, i, lo = -1, hi = -1;
                s32 rate = Q12(15.0f);
                if (g_DebugViewNpcSlot >= 0 && g_DebugViewNpcSlot < NPC_COUNT_MAX) {
                    s_SubCharacter* npc = &g_SysWork.npcs[g_DebugViewNpcSlot];
                    tbl   = (npc->model.charaId != Chara_None) ? npc->model.anim.baseAnimInfos : NULL;
                    count = 64; /* NPC tables carry no runtime length: cap + validate */
                }
                for (i = 0; tbl != NULL && i < count; i++) {
                    s32 sk = tbl[i].startKeyframeIdx;
                    s32 ek = tbl[i].endKeyframeIdx;
                    if (tbl[i].playbackFunc == NULL) continue;
                    if (sk < 0 || ek <= sk || ek >= 2048) continue;
                    if (g_DebugAnimKf >= sk && g_DebugAnimKf <= ek) {
                        lo = sk; hi = ek;
                        rate = tbl[i].hasVariableDuration ? Q12(15.0f) : tbl[i].duration.constant;
                        break;
                    }
                }
                if (lo >= 0) {
                    g_DebugAnimKfStart = lo;
                    g_DebugAnimKfEnd   = hi;
                    g_DebugAnimRate    = rate;
                    g_DebugAnimPlayGen++;
                    g_DebugAnimPlaying = 1;
                    SH_DBG_ECHO("[DEBUG] P: play loop [%d..%d] rate=%d", lo, hi, (int)rate);
                } else {
                    SH_DBG_ECHO("[DEBUG] P: no loopable anim contains KF %d", g_DebugAnimKf);
                }
            } else {
                g_DebugAnimPlaying = 0;
                SH_DBG_ECHO("[DEBUG] P: play stopped (KF %d)", g_DebugAnimKf);
            }
        }
        prevP = curP;
    }

    /* L: log the FPS eye offset to bake into g_PcFpsOffset.
     * In FPS mode the eye = g_PcFpsOffset (baseline) + live head-sway, so logging
     * the swaying EYE would bake the sway in and it re-adds every session (the
     * "always off by the same amount" bug). Log the stable BASELINE the numpad
     * edits instead — pasting it converges. With the debug cam, there is no
     * baseline, so fall back to the flown-to eye position in Harry's body frame. */
    {
        static int prevL = 0;
        int curL = g_sdlKeyboardState[SDL_SCANCODE_L];
        if (curL && !prevL) {
            if (g_PcFpsCam && !g_DebugCamEnabled) {
                SH_DBG_ECHO("[FPSCAM] g_PcFpsOffset = { %d, %d, %d }  (baseline; paste to bake)",
                            (int)g_PcFpsOffset.vx, (int)g_PcFpsOffset.vy, (int)g_PcFpsOffset.vz);
            } else {
                s_SubCharacter* hr = &g_SysWork.playerWork.player;
                VECTOR3 cam = g_DebugCamEnabled ? g_DebugCamPos : g_PcCamAppliedPos;
                s32 dx = cam.vx - hr->position.vx;
                s32 dy = cam.vy - hr->position.vy;
                s32 dz = cam.vz - hr->position.vz;
                s32 sy = Math_Sin(hr->rotation.vy);
                s32 cy = Math_Cos(hr->rotation.vy);
                s32 localX = (s32)(((s64)dx * cy - (s64)dz * sy) >> 12);
                s32 localZ = (s32)(((s64)dx * sy + (s64)dz * cy) >> 12);
                SH_DBG_ECHO("[FPSCAM] cam=(%d,%d,%d) harry=(%d,%d,%d) bodyYaw=%d  -> LOCAL OFFSET { %d, %d, %d }",
                            cam.vx, cam.vy, cam.vz, hr->position.vx, hr->position.vy, hr->position.vz,
                            (int)hr->rotation.vy, localX, dy, localZ);
            }
        }
        prevL = curL;
    }

    /* Per-frame cheat enforcement: god mode (health catch-all) + no-target */
    if (g_GameWork.gameState == GameState_InGame) {
        if (g_PcGodMode)
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
            /* FPS-cam eye tuner owns KP_3 (yaw fine-turn); don't also fire the
             * fall-recovery teleport there. */
            if (curKp3 && !prevKp3 && _haveSafeY && !g_PcFpsCam) {
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

    /* ==== First-person eye tuning (numpad) ====
     * Active only in FPS mode (not debug-cam). The eye sits at Harry's root +
     * g_PcFpsOffset, a LOCAL offset in his BODY frame. Every key below is a
     * straight-line nudge along one body axis — no rotation, no orbit — so the
     * eye moves exactly where you'd expect. Press KP_5 to print values to bake:
     *   KP_8/KP_2  move eye forward / back    (offset vz, held)
     *   KP_6/KP_4  move eye right / left       (offset vx, held)
     *   KP_9/KP_7  move eye up / down          (offset vy, held, coarse)
     *   KP_+/KP_-  move eye up / down          (offset vy, held, fine)
     *   KP_5       log g_PcFpsOffset (paste to bake) */
    if (g_PcFpsCam && !g_DebugCamEnabled &&
        g_GameWork.gameState == GameState_InGame)
    {
        #define FPS_MOVE_STEP 64
        #define FPS_VFINE     12   /* fine vertical step for KP_- / KP_+ */

        if (g_sdlKeyboardState[SDL_SCANCODE_KP_8]) g_PcFpsOffset.vz += FPS_MOVE_STEP; /* forward */
        if (g_sdlKeyboardState[SDL_SCANCODE_KP_2]) g_PcFpsOffset.vz -= FPS_MOVE_STEP; /* back */
        if (g_sdlKeyboardState[SDL_SCANCODE_KP_6]) g_PcFpsOffset.vx += FPS_MOVE_STEP; /* right */
        if (g_sdlKeyboardState[SDL_SCANCODE_KP_4]) g_PcFpsOffset.vx -= FPS_MOVE_STEP; /* left */
        if (g_sdlKeyboardState[SDL_SCANCODE_KP_9]) g_PcFpsOffset.vy -= FPS_MOVE_STEP; /* up (PSX +Y is down) */
        if (g_sdlKeyboardState[SDL_SCANCODE_KP_7]) g_PcFpsOffset.vy += FPS_MOVE_STEP; /* down */
        if (g_sdlKeyboardState[SDL_SCANCODE_KP_PLUS])  g_PcFpsOffset.vy -= FPS_VFINE; /* fine up */
        if (g_sdlKeyboardState[SDL_SCANCODE_KP_MINUS]) g_PcFpsOffset.vy += FPS_VFINE; /* fine down */

        {
            static int prev5 = 0;
            int cur5 = g_sdlKeyboardState[SDL_SCANCODE_KP_5];
            if (cur5 && !prev5) {
                extern VECTOR3 g_PcFpsHeadRefDbg, g_PcFpsHeadLocalDbg;
                SH_DBG_ECHO("[FPSCAM-TUNE] g_PcFpsOffset = { %d, %d, %d }",
                            (int)g_PcFpsOffset.vx, (int)g_PcFpsOffset.vy, (int)g_PcFpsOffset.vz);
                SH_DBG_ECHO("[FPSCAM-HEADREF] s_fpsHeadRef = { %d, %d, %d }  headLocal = { %d, %d, %d }",
                            (int)g_PcFpsHeadRefDbg.vx, (int)g_PcFpsHeadRefDbg.vy, (int)g_PcFpsHeadRefDbg.vz,
                            (int)g_PcFpsHeadLocalDbg.vx, (int)g_PcFpsHeadLocalDbg.vy, (int)g_PcFpsHeadLocalDbg.vz);
            }
            prev5 = cur5;
        }

        #undef FPS_MOVE_STEP
        #undef FPS_VFINE
    }


    /* If free-fly debug cam is off */
    if (!g_DebugCamEnabled) {
        /* TPS orbit camera (when the TPS control style is active). Body-yaw
         * follow + movement live in player_control.c's TPS branch. */
        if (g_DebugThirdPersonCam) {
            Pc_TpsCamera_Apply();
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

    /* Hold Left-Ctrl for ultra-fine placement (quarter speed) when nudging the
     * debug cam onto the exact between-the-arms FPS spot before pressing L. */
    if (g_sdlKeyboardState[SDL_SCANCODE_LCTRL]) {
        dbgMoveSpeed >>= 2;
        dbgVertSpeed >>= 2;
        dbgTurnSpeed >>= 2;
        if (dbgTurnSpeed < 1) dbgTurnSpeed = 1;
    }

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
#ifndef SH_PC_PORT
    /* g_MainImg0 is the 2ZANKO_E "violent images" warning (descriptor identical to
     * warning_screen.c's s_WarnImg, same VRAM). On PSX the boot state IS the warning
     * screen; on PC the warning is a separate pre-loop pass (Pc_PlayWarningScreen), so
     * re-drawing the same VRAM image here just flashed the warning a SECOND time
     * (snaps in 4:3-pillarboxed, then fades out). Boot clears to black (background2dColor=0)
     * without it, so the hand-off to the Konami logo stays clean. */
    Screen_BackgroundImgDraw(&g_MainImg0);
#endif
    func_80089090(1);
}

#ifdef SH_PC_PORT
/* Provided by libgs_stub.c — gives the bounds of any subroot OT registered
 * via GsSortOt(subroot, root). The pickup-screen pipeline registers
 * g_OrderingTable1 as a subroot of g_OrderingTable0; its bucket nodes
 * live in storage outside our packet-buffer + OT0-array windows and
 * MUST be accepted as valid chain pointers by the OT0 sanitizer below,
 * otherwise the chain is truncated mid-walk and the picked-up item
 * disappears. */
extern void GsSortOt_GetSubrootBounds(uintptr_t* lo, uintptr_t* hi);
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

        /* Randomizer: per-area monster placement, entry-door relock timer.
         * No-op unless a run is live. */
        {
            extern void Pc_Rando_Update(void);
            Pc_Rando_Update();
        }
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

        /* Change Camera (F9 / R3): toggle control style; manages TPS mouse
         * capture. Always on, not debug-gated. */
        {
            extern void Pc_ControlStyleUpdate(void);
            Pc_ControlStyleUpdate();
        }

        /* Bound PC actions: Cycle Weapons + Quick Heal + Quick Turn request (reload
         * is pulled by the combat FSM). Keyboard + controller, edge-detected. */
        {
            extern void Pc_ExtraActionsUpdate(void);
            Pc_ExtraActionsUpdate();
        }

        /* Rear Look (held): set g_PcRearLookActive for the TPS/OTS camera + head. */
        {
            extern void Pc_RearLookUpdate(void);
            Pc_RearLookUpdate();
        }

        /* "Disable D-pad for movement" applies ONLY during gameplay, so the D-pad
         * still navigates menus / inventory / the map. Re-evaluated every frame. */
        {
            extern int g_cfg_disableDpadMovement;
            g_cfg_disableDpadMovement =
                (g_PcConfig.disableDpadMovement &&
                 g_GameWork.gameState == GameState_InGame &&
                 g_SysWork.sysState   == SysState_Gameplay) ? 1 : 0;
        }

        /* Mouse cursor: drive free-cursor puzzles + the main menu from the mouse.
         * Runs after the controller is built and before the state update reads
         * it, so puzzle-cursor injection lands this frame. */
        {
            extern void Pc_MouseCursor_FrameUpdate(void);
            Pc_MouseCursor_FrameUpdate();
        }

        /* Console `fmv`: once the fade-out it started lands, this blocks in
         * FMV_Play and fades back in afterwards. */
        {
            extern void Pc_ConsoleFmvUpdate(void);
            Pc_ConsoleFmvUpdate();
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

#ifdef SH_PC_PORT
        /* Cleared each frame; Gfx_PickupItemAnimate re-arms it while a world
         * item-pickup model is on screen. See the OT0 force-item-depth bracket
         * and the freeze-frame release below. */
        { extern int g_PcPickupItemActive; g_PcPickupItemActive = 0; }
#endif

        // Call update function for current GameState.
        g_GameStateUpdateFuncs[g_GameWork.gameState]();
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

#ifdef SH_PC_PORT
        /* Flashlight shadow master gate: the light-POV depth pre-pass is a
         * live-gameplay-only effect. Running it on menu / room-load-fade /
         * transition frames corrupts unrelated rendering (white flash between
         * rooms, on inventory/map open-close, and on first load; Harry's face
         * dropping out on the options screen). Arm it only in settled gameplay:
         * the in-game state, the plain gameplay sub-state (not paused/menu/map),
         * and no screen fade in progress. Recomputed every frame here, before the
         * OT is drawn; reset paths (menus, fades) simply leave it 0. */
        {
            extern int g_PsyX_ShadowsAllowed;
            g_PsyX_ShadowsAllowed = (g_GameWork.gameState == GameState_InGame &&
                                     g_SysWork.sysState == SysState_Gameplay &&
                                     ScreenFade_IsNone()) ? 1 : 0;
        }

        /* Release the freeze-frame the instant the world item-pickup ends
         * (Gfx_PickupItemAnimate stops re-arming g_PcPickupItemActive), so live
         * gameplay rendering resumes. The item pass held g_PsxPresentLastFrame
         * to show the frozen room behind the isolated model. */
        {
            extern int g_PcPickupItemActive;
            extern int g_PsxPresentLastFrame;
            static int s_pcPickupWas = 0;
            if (s_pcPickupWas && !g_PcPickupItemActive)
                g_PsxPresentLastFrame = 0;
            s_pcPickupWas = g_PcPickupItemActive;
        }
#endif

        Demo_Update();
        Demo_GameRandSeedSet();

        if (MainLoop_ShouldWarmReset() == 2)
        {
            Game_WarmBoot();
            continue;
        }

#define ML_TRACE(tag) ((void)0)
        ML_TRACE("Screen_FadeUpdate");
        Screen_FadeUpdate();
#ifdef SH_PC_PORT
        /* Quick Heal green pulse — full-screen additive tile into OT2, same window as
         * the fade. Self-gated on its timer (no-op when not healing). */
        {
            extern void Pc_HealFlashUpdate(void);
            Pc_HealFlashUpdate();

            /* Minimap: load the area's paper-map TIM here (game side) when it
             * changes — the Fs queue must not be touched from the GL hook that
             * draws it. Self-gated on g_PcConfig.minimap. */
            { extern void Pc_MinimapUpdate(void); Pc_MinimapUpdate(); }

            { extern void Sh_LogPeriodicFlush(void); Sh_LogPeriodicFlush(); }
        }
#endif
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

        ML_TRACE("func_80089128");
        func_80089128();
        ML_TRACE("func_8008D78C");
        func_8008D78C(); // Camera update?
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
                        else if (effectiveFps > 60 || (60 % effectiveFps) != 0)
                        {
                            /* High fps (120, 240) or a cap that doesn't divide 60
                             * (e.g. 45, 40): SDL timer — the vblank loop can only
                             * express whole-vblank multiples, so 60/fps integer
                             * division silently turned 31..59 into a 60fps cap. */
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

#ifdef SH_PC_PORT
            /* [PERF] wall-clock frame telemetry, one line per ~256 frames.
             * Remote perf reports need numbers: avg/worst frame time separates
             * "GPU genuinely slow" from a pacing/stall pathology, and
             * vblanks-per-frame exposes vsync quantization (10fps = 6/frame). */
            {
                static Uint32 s_perfLastMs = 0, s_perfAccumMs = 0, s_perfWorstMs = 0;
                static u32    s_perfFrames = 0, s_perfVbAccum = 0;
                Uint32        perfNowMs = SDL_GetTicks();
                if (s_perfLastMs != 0)
                {
                    Uint32 dt = perfNowMs - s_perfLastMs;
                    s_perfAccumMs += dt;
                    if (dt > s_perfWorstMs)
                        s_perfWorstMs = dt;
                    s_perfVbAccum += (u32)g_UncappedVBlanks;
                    if (++s_perfFrames >= 256)
                    {
                        SH_DBG("[PERF] avg=%.1fms (%.1f fps) worst=%lums vblanks/frame=%.2f over %lu frames",
                               (double)s_perfAccumMs / s_perfFrames,
                               1000.0 * s_perfFrames / (double)s_perfAccumMs,
                               (unsigned long)s_perfWorstMs,
                               (double)s_perfVbAccum / s_perfFrames,
                               (unsigned long)s_perfFrames);
                        s_perfFrames = 0; s_perfAccumMs = 0; s_perfWorstMs = 0; s_perfVbAccum = 0;
                    }
                }
                s_perfLastMs = perfNowMs;
            }
#endif

            // Update V count.
            vCount     = MIN(GsGetVcount(), H_BLANKS_PER_FRAME_MIN); // NOTE: Will call `GsGetVcount` twice.
            vCountCopy = vCount;

#ifdef SH_PC_PORT
            /* Invisible-wall fix (#42): the original 15fps floor lets a single
             * frame's timestep reach 2x the PSX 30fps step during a streaming
             * hitch (chunk loads while running through open areas). Movement is
             * moveSpeed*g_DeltaTime, and Collision_WallDetect SWEEPS that full
             * per-frame distance against wall segments (it tests the movement
             * line, not the body radius), so an inflated step over-reaches and
             * snags walls the body never touches — Harry bumps invisible walls
             * at full run. Cap the GAMEPLAY step (vCount feeds g_DeltaTime and
             * g_GravitySpeed) at the PSX 30fps move so the worst-case sweep can
             * never exceed what PSX itself swept. Frames faster than 30fps are
             * unaffected; a real hitch just slows time slightly instead of
             * teleporting Harry forward. g_DeltaTimeRaw (vCountCopy) is left raw
             * for the wall-clock accumulators / cutscene timers.
             *
             * NOTE: a previous cutscene-desync fix skipped this cap during
             * cutscenes (so the visual clock tracked the uncapped audio). That
             * left cutscene timing frame-rate dependent — DMS/animation steps
             * advanced on a per-frame dt that could grow to the 15fps floor on
             * heavy scenes, skipping keyframes/steps, so cutscenes only played
             * correctly near a single frame rate (~50fps). Reverted per user
             * request: cap the cutscene step like normal gameplay so cutscenes
             * behave the same across frame rates as they did before. */
            vCount = MIN(vCount, H_BLANKS_PER_SECOND / 30);
#endif
        }

        ML_TRACE("deltaTime");
        // Update delta time.
        g_DeltaTime    = Q12_MULT(vCount, H_BLANKS_Q12_TO_SEC_SCALE);
        g_DeltaTimeRaw = Q12_MULT(vCountCopy, H_BLANKS_Q12_TO_SEC_SCALE);
        g_GravitySpeed = Q12_MULT(vCount, H_BLANKS_GRAVITY_SCALE);
        GsClearVcount();

#ifdef SH_PC_PORT
        /* Lossless game clock + cutscene audio catch-up. The per-frame
         * vCount pipeline above loses real time at THREE truncation sites
         * (GsGetVcount's int cast, GsClearVcount's epoch reset discarding the
         * fraction, Q12_MULT's floor). Each loss is < 1 unit, but it repeats
         * every frame, so the whole game clock runs slow vs wall time in
         * proportion to fps: ~0.4% at 60fps, ~3.3% at 120, ~6.25% at 240,
         * ~27% uncapped — while XA voices play at true wall clock (OpenAL).
         * Over a 3-minute cutscene that is 6-11+ seconds of scene/subtitle
         * lag behind the dialog. Recompute dt from ONE cumulative Q12 clock
         * (fixed epoch, floored only at the read), so the summed dt equals
         * true elapsed time at any fps forever.
         *
         * A prior fix (edfe66887) carried the per-frame remainder and was
         * reverted on a theory the desync was map6_s04-specific; widespread
         * subtitle/scene desync reports at high fps since then match the
         * uncorrected drift exactly. This version differs from the reverted
         * one: single clock source (no double GsGetVcount sample feeding two
         * different values), and the cutscene catch-up below never lets one
         * frame exceed the PSX 30fps step, the regression mode that revert
         * blamed (comment above at the 30fps cap).
         *
         * Cutscene catch-up: the 30fps cap (invisible-wall fix) and 15fps
         * floor discard wall time on every slow frame; during a cutscene the
         * discarded time goes into a debt that later fast frames repay — but
         * each frame's dt stays <= the PSX 30fps step (136), so DMS/anim
         * stepping never sees a larger step than original hardware. After a
         * load hitch the scene briefly runs fast and relocks to the voices
         * instead of staying permanently behind. Debt is bounded, reset
         * outside cutscenes, and not accrued while the console freeze has
         * dt zeroed (frozen time stays lost, matching today's behavior).
         * During cutscenes g_DeltaTimeRaw = g_DeltaTime so subtitles, message
         * timers and event waits share one clock with DMS/anims (on PSX the
         * two were the same variable; the raw/capped split is PC-only and
         * made subtitles advance up to 2x faster than the scene below
         * 30fps). */
        if (!(g_SysWork.sysFlags & SysFlag_DemoActive))
        {
            extern long long GsGetCumulativeQ12(void);
            extern int g_PcConsoleInputActive;

            #define PC_DT_STEP_30FPS     136 /* (526*1063)>>12: PSX 30fps step in Q12 seconds */
            #define PC_DT_STEP_15FPS     273 /* (1052*1063)>>12: PSX 15fps floor step */
            #define PC_CUTSCENE_DEBT_MAX Q12(2.0f)

            static long long s_prevCumQ12   = -1;
            static s32       s_cutsceneDebt = 0;

            long long cumQ12 = GsGetCumulativeQ12();
            s32       dtTrue;
            s32       dtRaw;
            s32       dtCapped;
            int       pcInCutscene = (g_SysWork.sysFlags & SysFlag_CutsceneActive) ||
                                     g_SysWork.cutsceneBorderState != CutsceneBorderState_None;
            int       pcConsoleFrozen = g_PcConsoleInputActive &&
                                        !(g_SysWork.bgmStatusFlags & BgmStatusFlag_Pause) &&
                                        !g_SysWork.isMgsStringSet;

            /* Voice audio must freeze with the game clock, or the spoken line
             * runs ahead of the frozen scene/subtitle for the whole console
             * session. */
            XaPlayer_SetPauseHold(pcConsoleFrozen);

            if (s_prevCumQ12 < 0)
            {
                s_prevCumQ12 = cumQ12;
            }
            dtTrue       = (s32)(cumQ12 - s_prevCumQ12);
            s_prevCumQ12 = cumQ12;
            if (dtTrue < 0)
            {
                dtTrue = 0;
            }

            dtRaw    = MIN(dtTrue, PC_DT_STEP_15FPS);
            dtCapped = MIN(dtRaw, PC_DT_STEP_30FPS);

            if (pcInCutscene && !pcConsoleFrozen)
            {
                s_cutsceneDebt += dtTrue - dtCapped;
                s_cutsceneDebt  = MIN(s_cutsceneDebt, PC_CUTSCENE_DEBT_MAX);
                if (s_cutsceneDebt > 0 && dtCapped < PC_DT_STEP_30FPS)
                {
                    s32 pay = MIN(s_cutsceneDebt, PC_DT_STEP_30FPS - dtCapped);

                    dtCapped       += pay;
                    s_cutsceneDebt -= pay;
                }
                dtRaw = dtCapped;
            }
            else if (!pcInCutscene)
            {
                s_cutsceneDebt = 0;
            }

            g_DeltaTime    = dtCapped;
            g_DeltaTimeRaw = dtRaw;
            g_GravitySpeed = Q12_MULT(dtCapped, Q12(9.8f));

            #undef PC_DT_STEP_30FPS
            #undef PC_DT_STEP_15FPS
            #undef PC_CUTSCENE_DEBT_MAX
        }
#endif

#ifdef SH_PC_PORT
        /* Interactive console input mode (hold `~`): freeze the game like the
         * pause screen — zero game time so the world stops simulating. Must
         * happen here, after the dt recompute above, so next frame's game
         * update sees 0. Controller suppression lives next to the pad parse
         * at the top of the loop.
         *
         * BUT only when the game isn't ALREADY paused. The pause screen /
         * inventory / status / map / any state that sets BgmStatusFlag_Pause
         * already froze the world its own way; zeroing dt on top makes the two
         * pauses fight — the menu's own blink/animation freezes, and the world
         * stays stuck after you unpause until the console is closed. When
         * already paused, leave dt alone and let that pause own the freeze.
         *
         * Same applies while a map-message is on screen (e.g. "I don't have a
         * map"): it runs on the live mapMsgTimer (-= g_DeltaTimeRaw), so zeroing
         * dt freezes the message's own timing and the console fights it. isMgsStringSet
         * is true while a map-message is being displayed — leave dt alone then too. */
        {
            extern int g_PcConsoleInputActive;
            if (g_PcConsoleInputActive &&
                !(g_SysWork.bgmStatusFlags & BgmStatusFlag_Pause) &&
                !g_SysWork.isMgsStringSet) {
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
        /* Hysteresis on the wide->narrow edge: state transitions (white
         * fades, menu opens) pass through non-InGame for a frame or two,
         * which flashed the 4:3 pillarbox mid-fade ("4:3 swap" reports).
         * Only drop to 4:3 after the narrow condition holds for several
         * consecutive frames; returning to Hor+ stays instant. */
        /* "Fullscreen 2D background screen drawn within the last 300ms" signal,
         * maintained UNCONDITIONALLY every frame so it always decays. The
         * per-frame decrement must NOT live behind the fog/menu branches below:
         * in non-foggy rooms that branch never ran, so g_Pc2dBackgroundActive
         * stuck >0 and Hor+ stayed permanently pillarboxed after examining an
         * object. Screen_BackgroundImgDraw* re-sets the counter to 2 each frame
         * it draws; the 300ms hold bridges the few-frame TIM-swap gaps during
         * puzzle interactions (and avoids a stretched-frame flash on return). */
        int bg2dHeld;
        {
            extern s32 g_Pc2dBackgroundActive;
            static Uint32 s_bg2dHoldUntilMs;
            if (g_Pc2dBackgroundActive > 0) {
                g_Pc2dBackgroundActive--;
                s_bg2dHoldUntilMs = SDL_GetTicks() + 300;
            }
            bg2dHeld = (SDL_GetTicks() < s_bg2dHoldUntilMs) ? 1 : 0;
        }
        {
            static int s_narrowFrames = 0;
            /* Fullscreen 2D background screens that run inside GameState_InGame
             * (keypad/dial/plate puzzles, the eclipse door, item-inspection and
             * death-tip images) draw via Screen_BackgroundImgDraw* and must use
             * 4:3 ortho like the menus — Hor+ widening stretches them off-screen.
             * Honour bg2dHeld immediately (these are stable screens, not a fade
             * transient to ride out — the countdown would flash a stretched
             * frame before snapping). */
            int wantHorPlus = (g_GameWork.gameState == GameState_InGame &&
                               !g_PsxSkipFramebufferStore &&
                               !g_PcMapScreenActive &&
                               !bg2dHeld) ? 1 : 0;
            if (wantHorPlus)
            {
                s_narrowFrames     = 0;
                g_PcHorPlusEnabled = 1;
            }
            else if (bg2dHeld)
            {
                s_narrowFrames     = 6;
                g_PcHorPlusEnabled = 0;
            }
            else if (++s_narrowFrames >= 6)
            {
                g_PcHorPlusEnabled = 0;
            }
        }

        /* Cutscenes get their vertical framing from the letterbox bars, so the gameplay
         * vfov crop (g_PsxWorldVScale) must NOT apply during them — it scaled/clipped the
         * bars and subtitles off the bottom. Hand the cutscene state to PsyCross so
         * GR_SetOffscreenState skips the vertical crop while a cutscene is active. */
        {
            extern int g_PsxCutsceneActive;
            g_PsxCutsceneActive = ((g_SysWork.sysFlags & SysFlag_CutsceneActive) ||
                                   g_SysWork.cutsceneBorderState != CutsceneBorderState_None) ? 1 : 0;
        }

        /* Fixed-angle camera shots frame the top of the scene clipped vs PSX (e.g. a
         * medkit off the top of frame). Correct it by shifting the GTE projection
         * center down (world content moves down INSIDE the normal 224-line frame)
         * rather than shifting the PsyCross ortho window up: the ortho shift revealed
         * rows above the frame that screen-space overlay prims (darkness/light quads,
         * authored 0..224) never cover — the "faded letterbox" band at the top that
         * toggled with FIX_ANG camera zones, in every camera style. Screen-space prims
         * don't go through the GTE, so full-frame overlay coverage is preserved; the
         * visible world window is identical to the old ortho shift (same cull margins).
         * Asserted every frame in every state so item screens / menus (GTE consumers
         * outside gameplay) always run at the clean baseline offset (0,0). Gameplay
         * classic camera only: alt cameras (FPS/TPS/OTS) replace the game camera, and
         * cutscenes frame via letterbox bars. g_PsxWorldVShift (PsyCross, console
         * `vshift`) stays the live-tunable amount in PSX units. */
        {
            extern int   g_PsxFixedCamActive;
            extern int   g_PsxCutsceneActive;
            extern float g_PsxWorldVShift;
            extern int   g_PcPickupItemActive;
            static s32   s_heldWorldOfy = 0;
            s32 ofy = 0;

            if (g_GameWork.gameState == GameState_InGame &&
                g_SysWork.sysState == SysState_Gameplay &&
                !g_PcPickupItemActive)
            {
                if (g_PsxFixedCamActive && !g_PsxCutsceneActive && !g_DebugThirdPersonCam)
                {
                    ofy = (s32)g_PsxWorldVShift;
                }
                s_heldWorldOfy = ofy;
            }
            else if (g_GameWork.gameState == GameState_InGame &&
                     (g_SysWork.sysState == SysState_ReadMessage || g_PcPickupItemActive))
            {
                /* Frozen interactions — examine/read-message text and the item-pickup
                 * animation — keep re-rendering the SAME fixed-cam frame (vcMoveAndSetCamera
                 * still runs every frame) and snapshot it for the frozen backdrop. The
                 * original has no per-state offset, so it looks identical before and during
                 * the interaction; hold the gameplay offset here instead of recomputing it,
                 * so the shift can't jump the instant text/pickup appears (any display
                 * camera, since the shift is at the GTE projection center). */
                ofy = s_heldWorldOfy;
            }

            SetGeomOffset(0, ofy);
        }

        /* Suppress dither on 2D-only states (logos, menus, map screen,
         * inventory, options, save/load). Dither makes flat-shaded UI
         * art look chewed-up at high resolution. Keep it for 3D gameplay
         * + cutscenes (InGame state covers both). Read by PsyCross via
         * extern int g_PsxDitherSuppressed in PsyX_render.cpp. */
        extern int g_PsxDitherSuppressed;
        g_PsxDitherSuppressed = (g_GameWork.gameState == GameState_InGame) ? 0 : 1;

        /* Set by the game (game_sys_states.c) while a state freezes the world and presents
         * the captured FOGGY gameplay frame behind a message: the literal PAUSED screen
         * and the map-screen messages ("I don't have a map" / "too dark for map"). Those
         * want the foggy frozen scene behind the text, NOT a black 2D-menu clear. */
        extern int g_PsxPresentLastFrame;

        /* Override background color with fog color during InGame.
         * fog params are set by Gfx_FlashlightUpdate from the previous frame's
         * update, so they're valid by frame 2+. Use the normal GsSortClear path
         * which PsyCross handles via activeDrawEnv.isbg in PsyX_BeginScene. */
        if (g_PcMapScreenActive && !g_PsxPresentLastFrame) {
            /* Paper-map / map-pickup screens (Screen_BackgroundImgDraw) run under
             * GameState_MapEvent (12) for the pickup, so the gameState==11 forcing below
             * is skipped and the pillarbox bars kept the stale gameplay fog color (read
             * as garbage/old scenery). Force black so the map sits on black bars like the
             * inventory. (A map-screen MESSAGE state freezes + presents the foggy frame
             * instead — excluded via !g_PsxPresentLastFrame so it keeps the foggy sky.) */
            g_GameWork.background2dColor.r = 0;
            g_GameWork.background2dColor.g = 0;
            g_GameWork.background2dColor.b = 0;
        }
        else if (g_GameWork.gameState == 11 && g_SysWork.sysState == SysState_GameOver) {
            /* GAME OVER renders its own death scene; the sky is meant to be black there.
             *
             * This branch used to ALSO force black on any gameState==11 frame with
             * SysFlag_MenuActive — that was the one-frame BLACK-SKY FLASH on opening the
             * status menu / map. MenuActive is set on the SAME frame the button is pressed
             * (SysState_Gameplay_Update, sysState set immediately) while gameState is still
             * InGame(11) and Gfx_InGameDraw still renders the full world, so forcing the
             * clear black painted the uncovered fog/sky region black under a lit world for
             * one frame (map: for the whole ScreenFade). The genuine 2D menus re-black
             * background2dColor themselves once they are actually up (inventory/options at
             * their own gameState; paper map via g_PcMapScreenActive above), so the
             * transition frame must keep the FOG clear to match the world still being drawn.
             * Fixes the flash on both status and map WITHOUT re-arming g_PsxPresentLastFrame
             * (the reverted hold, c2751cf15/2a2260a57, which poisoned the VRAM framebuffer
             * feedback -> post-close sky/pillarbox ghost). */
            g_GameWork.background2dColor.r = 0;
            g_GameWork.background2dColor.g = 0;
            g_GameWork.background2dColor.b = 0;
        }
        else if (g_GameWork.gameState == 11 && PC_WorldEnvWork.isFogEnabled) {
            /* Fullscreen 2D background screens (eclipse/plates doors, item
             * inspection) must clear to the game's own color (black) — on PSX
             * the fog void isn't the clear color, so these screens were never
             * fog-tinted. bg2dHeld (maintained unconditionally above) is the
             * time-based "2D background drawn within the last 300ms" signal that
             * bridges the few-frame TIM-swap gaps during puzzle interactions. */
            if (bg2dHeld) {
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
#ifdef SH_PC_PORT
        /* Inventory item see-through fix: force real per-pixel depth (test+write,
         * with a fresh depth clear) around the item OT0 draw. Scoped to
         * GameState_InventoryScreen only, where OT0 holds the rotating item ALONE
         * (the world is not sorted), so the model's own front faces occlude its
         * back faces (radio antenna through the body) without touching gameplay or
         * in-world pickups. Pairs with g_PcItemPreciseDepth feeding true per-prim
         * SZ during the sort (item_screens_cam.c). */
        /* Also the world item-pickup: Gfx_PickupItemAnimate pauses the world so
         * OT0 again holds only the item model (g_PcPickupItemActive), so the same
         * per-pixel depth pass fixes its see-through without touching the world. */
        {
            extern int g_PcPickupItemActive;
            if (g_GameWork.gameState == GameState_InventoryScreen || g_PcPickupItemActive) {
                extern void PsyX_ForceItemDepthBegin(void);
                PsyX_ForceItemDepthBegin();
            }
        }
#endif
        GsDrawOt(&g_OrderingTable0[g_ActiveBufferIdx]);
#ifdef SH_PC_PORT
        {
            extern int g_PcPickupItemActive;
            if (g_GameWork.gameState == GameState_InventoryScreen || g_PcPickupItemActive) {
                extern void PsyX_ForceItemDepthEnd(void);
                PsyX_ForceItemDepthEnd();
            }
        }
#endif
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
#ifdef SH_PC_PORT
        /* OT2 = 2D UI (text/subtitles, screen fade, cutscene letterbox bars).
         * Flag its splits so the renderer draws them with the full vertical
         * ortho instead of the Hor+ world crop (g_PsxWorldVScale), which would
         * otherwise clip bottom-anchored subtitles and the bottom letterbox
         * bar off-screen. The world (OT0, drawn above) keeps the crop. */
        {
            extern int g_PsxUIOrthoPass;
            g_PsxUIOrthoPass = 1;
            GsDrawOt(&g_OrderingTable2[g_ActiveBufferIdx]);
            g_PsxUIOrthoPass = 0;
        }
#else
        GsDrawOt(&g_OrderingTable2[g_ActiveBufferIdx]);
#endif
        ML_TRACE("OT2-done");
#ifdef SH_PC_PORT
        /* The dev console is now drawn via g_PsyX_PostCaptureHook (registered in main_pc.c)
         * INSIDE PsyX_EndScene, AFTER the freeze-frame capture — so it's never baked into a
         * frozen pause / "no map" image (which used to ghost the live console against the
         * frozen copy when the console was already open before pausing). Not drawn here. */
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
