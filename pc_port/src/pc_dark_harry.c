/* SPDX-License-Identifier: GPL-3.0-or-later */
/* "Dark Harry". See pc_dark_harry.h for what it is and how to remove it.
 * Nothing in here is original-game behaviour. */
#include "pc_dark_harry.h"

#include "game.h"

#include <psyq/libgte.h>

#include "bodyprog/bodyprog.h"
#include "bodyprog/anim.h"
#include "bodyprog/chara/chara.h"
#include "bodyprog/collision/collision.h"
#include "bodyprog/items.h"
#include "bodyprog/map/map.h"
#include "bodyprog/math/math.h"
#include "bodyprog/player.h"
#include "bodyprog/screen/screen_data.h" /* g_DeltaTime */
#include "bodyprog/sound/sound_system.h"
#include "bodyprog/view/vc_util.h"
#include "maps/characters/harry.h"
#include "pc_config.h"
#include "sh_log.h"

extern void func_8003DA9C(e_CharaId charaId, GsCOORDINATE2* boneCoords, s32 arg2, q3_12 timer, s32 arg4);
extern int  g_PcCharaGhostDraw;

/* Speeds in world units per second. The walk is far below Harry's own (1.4):
 * the effect is noticing the shape is nearer than it was, not watching
 * something approach. The run is a real run, short of Harry's 4.0 so the
 * player can open a gap, but he never stops. */
#define DARK_HARRY_WALK Q12(0.5f)
#define DARK_HARRY_RUN  Q12(3.0f)

/* Keyframes per second for each loop. The walk is slowed in step with the
 * speed above so his feet do not slide. */
#define DARK_HARRY_WALK_RATE Q12(11.0f)
#define DARK_HARRY_RUN_RATE  Q12(28.0f)

/* How fast he turns to face the player while he stands and listens. */
#define DARK_HARRY_TURN_RATE Q12_ANGLE(120.0f)

/* Close enough to touch. Harry's own collision radius is 0.3. */
#define DARK_HARRY_REACH Q12(0.9f)

/* Radio attenuation the moment he appears. The scale is backwards: 0 is full
 * volume and 255 is silence, so this is "barely there", and it walks down to 0
 * as he closes. */
#define DARK_HARRY_RADIO_FAINT 232

/* Brightness multiplier when he is on top of the player. Not zero -- the fade
 * is meant to read as the light going out of the scene, not as a black screen
 * that looks like the game died. */
#define DARK_HARRY_DARKEN_MIN 0.12f

/* He is NOT an NPC.
 *
 * The obvious route -- take an NPC slot like the console SPAWN command -- does
 * not work for Harry: g_CharaAnimDataIdxs[Chara_Harry] is never set, because
 * the player's animation loads through the player's own path (FS_BUFFER_0 and
 * g_SysWork.playerBoneCoords) rather than into one of the NPC anim slots.
 * npc_main sees animLoaded == false and skips the slot with `continue`, so the
 * model is never drawn. The first build did exactly that: he spawned, walked
 * over and killed the player while being completely invisible.
 *
 * So he carries his own model state and bone coords and is drawn directly.
 * That also keeps him out of npc_main, collision and the spawn bookkeeping
 * entirely, which suits something that is not part of the game. */
#define DARK_HARRY_BONE_MAX 32

/* His three poses, as loops out of Harry's own animation file. The keyframe
 * ranges are the ones HARRY_BASE_ANIM_INFOS uses for the player's still, walk
 * and run; the durations are constants here because the player's rows take
 * theirs from the player's own move speed, which is not his. */
enum { DH_POSE_STILL = 0, DH_POSE_WALK, DH_POSE_RUN, DH_POSE_N };

static s_AnimInfo s_poses[DH_POSE_N] = {
    { NULL, 0, false, 0, { Q12(30.0f) },           0,  1  },
    { NULL, 0, false, 0, { DARK_HARRY_WALK_RATE }, 1,  23 },
    { NULL, 0, false, 0, { DARK_HARRY_RUN_RATE },  26, 45 },
};

static s_Model       s_model;
static GsCOORDINATE2 s_bones[DARK_HARRY_BONE_MAX];
static VECTOR3       s_pos;
static SVECTOR       s_rot;
static int           s_present; /* 0 = not here */
static int           s_posed;   /* bones initialised for this appearance */
static int           s_pose;    /* DH_POSE_* the skeleton is playing */
static int           s_hold;    /* standing and listening */
static int           s_run;     /* running from now on */
static int           s_taken;   /* he reached the player; read once by PollTaken */

static q19_12 s_spawnDist; /* how far out he started, for the approach curve */
static q19_12 s_dist;      /* current distance to the player */

/* Effects he owns while present, each restored on the way out. */
static int   s_radioVoice;  /* 1 = we started the static loop */
static int   s_brightHeld;  /* 1 = s_brightBase holds the player's own setting */
static float s_brightBase;

int Pc_DarkHarry_Active(void)
{
    return s_present;
}

void Pc_DarkHarry_Pos(int* x, int* y, int* z)
{
    *x = s_present ? s_pos.vx : 0;
    *y = s_present ? s_pos.vy : 0;
    *z = s_present ? s_pos.vz : 0;
}

float Pc_DarkHarry_Dist(void)
{
    return s_present ? (float)s_dist / 4096.0f : 0.0f;
}

int Pc_DarkHarry_PollTaken(void)
{
    int t  = s_taken;
    s_taken = 0;
    return t;
}

void Pc_DarkHarry_Hold(int hold)
{
    s_hold = hold ? 1 : 0;
}

void Pc_DarkHarry_Run(int run)
{
    s_run = run ? 1 : 0;
}

/* Hand back the radio and the brightness. Called from every exit so he cannot
 * leave the screen dark or the static running. */
static void DarkHarry_EffectsClear(void)
{
    extern float g_cfg_brightness;

    if (s_radioVoice)
    {
        Sd_SfxStop(Sfx_RadioStaticLoop);
        s_radioVoice = 0;
    }

    if (s_brightHeld)
    {
        g_cfg_brightness = s_brightBase;
        s_brightHeld     = 0;
    }
}

static void DarkHarry_Despawn(void)
{
    DarkHarry_EffectsClear();
    s_present = 0;
    s_posed   = 0;
    s_hold    = 0;
    s_run     = 0;
}

void Pc_DarkHarry_Dismiss(const char* why)
{
    if (s_present)
        SH_DBG("[DARKHARRY] gone: %s", why ? why : "dismissed");
    DarkHarry_Despawn();
}

/* NULL when he is allowed to be here: normal gameplay, player alive and
 * actually driving. Anything scripted (cutscene, message box, inventory, a
 * door, a death) names itself instead; the reason is what AFKEGG reports and
 * what the log records when he vanishes. */
static const char* DarkHarry_Ineligible(void)
{
    u32 state;

    if (!g_PcConfig.darkHarry)
        return "dark_harry is off in config.cfg";
    if (g_GameWork.gameState != GameState_InGame)
        return "not in gameplay";
    if (g_SysWork.sysState != SysState_Gameplay)
        return "a menu or a script owns the scene";
    if (g_SavegamePtr == NULL)
        return "no savegame";
    if (g_SysWork.playerWork.player.health <= Q12(0.0f))
        return "the player is already dead";

    /* PlayerState_Idle is the game's own look-around after ten seconds of
     * standing still, not a scripted takeover. The death still works from
     * it: the health<=0 branch at the end of Player_ReceiveDamage excludes
     * only the death and grab states. */
    state = (u32)g_SysWork.playerWork.extra.state;
    if (state != PlayerState_None && state != PlayerState_Idle)
        return "the player is not in normal control";

    return NULL;
}

static void DarkHarry_PoseSet(int pose)
{
    if (s_pose == pose && s_posed)
        return;
    s_pose                  = pose;
    s_model.anim.time       = Q12(s_poses[pose].startKeyframeIdx);
    s_model.anim.keyframeIdx = s_poses[pose].startKeyframeIdx;
    s_model.anim.alpha      = Q12(0.0f);
}

const char* Pc_DarkHarry_Spawn(int fromBehind)
{
    const s_SubCharacter* pl = &g_SysWork.playerWork.player;
    s_CollisionSurface    surf;
    q19_12                posX, posZ;
    s32                   dx, dz;
    const char*           why;

    if (s_present)
        return "he is already here";
    why = DarkHarry_Ineligible();
    if (why != NULL)
        return why;

    if (!fromBehind && (g_PcConfig.darkHarryX != 0 || g_PcConfig.darkHarryZ != 0))
    {
        /* A fixed landmark: he belongs to a place in the map rather than to
         * wherever the player happens to be facing. Config is in hundredths
         * of a world unit. */
        posX = (q19_12)(((s64)g_PcConfig.darkHarryX * Q12(1.0f)) / 100);
        posZ = (q19_12)(((s64)g_PcConfig.darkHarryZ * Q12(1.0f)) / 100);
    }
    else
    {
        /* At the fog wall, ahead of the player, or behind them when something
         * called him: he arrives from where they are not looking. */
        q19_12 dist = Q12(1.0f) * g_PcConfig.darkHarryDistance;
        s32    yaw  = fromBehind ? ((pl->rotation.vy + 0x800) & 0xFFF) : pl->rotation.vy;

        posX = pl->position.vx + (s32)(((s64)dist * Math_Sin(yaw)) >> 12);
        posZ = pl->position.vz + (s32)(((s64)dist * Math_Cos(yaw)) >> 12);
    }

    memset(&s_model, 0, sizeof(s_model));
    s_model.charaId      = Chara_Harry;
    s_model.controlState = 0;
    s_model.stateStep    = 0;
    s_model.anim.status  = ANIM_STATUS(HarryAnim_Still, false);
    s_model.anim.flags  |= AnimFlag_Visible | AnimFlag_Unlocked;
    s_model.anim.time    = Q12(0.0f);

    s_pos.vx = posX;
    s_pos.vz = posZ;
    Collision_SurfaceGet(&surf, posX, posZ);
    s_pos.vy = surf.groundHeight;

    s_rot.vx = 0;
    s_rot.vz = 0;
    s_rot.vy = (s16)((pl->rotation.vy + 0x800) & 0xFFF); /* facing the player */

    dx = pl->position.vx - s_pos.vx;
    dz = pl->position.vz - s_pos.vz;

    s_present   = 1;
    s_posed     = 0;
    s_pose      = DH_POSE_STILL;
    s_hold      = 0;
    s_run       = 0;
    s_taken     = 0;
    s_dist      = SquareRoot12((s32)((((s64)dx * dx) + ((s64)dz * dz)) >> 12));
    s_spawnDist = s_dist;
    DarkHarry_PoseSet(DH_POSE_WALK);

    /* A continue, taken the moment he appears, so the death he is walking
     * toward costs the player nothing but the walk back. */
    g_GameWork.autosave = g_GameWork.savegame;

    SH_DBG("[DARKHARRY] appeared at (%d,%d,%d), player at (%d,%d,%d), %d units out%s",
           FP_FROM(s_pos.vx, Q12_SHIFT), FP_FROM(s_pos.vy, Q12_SHIFT), FP_FROM(s_pos.vz, Q12_SHIFT),
           FP_FROM(pl->position.vx, Q12_SHIFT), FP_FROM(pl->position.vy, Q12_SHIFT),
           FP_FROM(pl->position.vz, Q12_SHIFT), FP_FROM(s_spawnDist, Q12_SHIFT),
           fromBehind ? ", from behind" : "");

    return NULL;
}

/* He has reached the player.
 *
 * NOT the Split Head instant death: that state animates out of the boss map's
 * own anim table (field_38[D_800AF220]) and ends on one exact keyframe in it,
 * which a map carrying no row for it can never reach -- and in map0_s00 it
 * routes to the scripted wake-up in the cafe instead of a game over. He walked
 * up, the state was set, and nothing happened. Zeroing health hands over to
 * the death every enemy in the game uses, which works on every map: the scream,
 * the collapse and the Game Over screen are the real ones. */
static void DarkHarry_Take(void)
{
    s_SubCharacter* pl = &g_SysWork.playerWork.player;

    SH_DBG("[DARKHARRY] reached the player - death");
    DarkHarry_Despawn();
    s_taken = 1;

    /* 66 is the attack that routes to the drowning variant; anything stale in
     * here would pick the wrong death. */
    pl->attackReceived = NO_VALUE;
    pl->health         = Q12(0.0f);

    /* The scream. The normal death has no voice of its own -- the cry a dying
     * player hears belongs to the hit that killed them, and nothing hit them
     * here -- so he died silently. This is the clip the Split Head death plays
     * (player_control.c, PlayerState_InstantDeath). */
    SD_Call(Sfx_XaAudio635);
}

/* 0 at the distance he appeared from, Q12(1.0) at arm's length. */
static q19_12 DarkHarry_Closeness(void)
{
    q19_12 span = s_spawnDist - DARK_HARRY_REACH;
    q19_12 gone;

    if (span <= Q12(0.5f))
        return Q12(1.0f);

    gone = s_spawnDist - s_dist;
    if (gone <= Q12(0.0f))
        return Q12(0.0f);

    return CLAMP((s32)(((s64)gone * Q12(1.0f)) / span), Q12(0.0f), Q12(1.0f));
}

/* The radio hears him and the light goes out of the scene, both tied to the
 * same closeness so they arrive together. */
static void DarkHarry_EffectsUpdate(void)
{
    extern float g_cfg_brightness;

    q19_12 closeness = DarkHarry_Closeness();

    /* Nothing to pick him up if the radio is off or not found yet. */
    if (g_SavegamePtr->itemToggleFlags & ItemToggleFlag_RadioOn)
    {
        s32 att = DARK_HARRY_RADIO_FAINT - (s32)(((s64)DARK_HARRY_RADIO_FAINT * closeness) >> 12);

        /* The static loop rather than the interference loop: npc_main fills its
         * two radio slots in order, so the second one is free unless two real
         * enemies are already in range. Sd_SfxAttributesUpdate restarts a
         * stopped radio voice by itself, so this both starts and drives it. */
        Sd_SfxAttributesUpdate(Sfx_RadioStaticLoop, Vc_StereoBalanceGet(&s_pos),
                               (u8)CLAMP(att, 0, 255), 0);
        s_radioVoice = 1;
    }

    if (!s_brightHeld)
    {
        s_brightBase = g_cfg_brightness;
        s_brightHeld = 1;
    }
    g_cfg_brightness = s_brightBase *
                       (1.0f - ((float)closeness / 4096.0f) * (1.0f - DARK_HARRY_DARKEN_MIN));
}

static void DarkHarry_FacePlayer(s32 yaw)
{
    q3_12 delta = Math_AngleNormalizeSigned((q3_12)yaw - s_rot.vy);
    q3_12 step  = Q12_MULT_PRECISE(g_DeltaTime, DARK_HARRY_TURN_RATE);

    if (ABS(delta) <= step)
        s_rot.vy = (s16)(yaw & 0xFFF);
    else
        s_rot.vy = (s16)((s_rot.vy + (delta > 0 ? step : -step)) & 0xFFF);
}

static void DarkHarry_Move(void)
{
    const s_SubCharacter* pl = &g_SysWork.playerWork.player;
    s_CollisionSurface    surf;
    s32 dx    = pl->position.vx - s_pos.vx;
    s32 dz    = pl->position.vz - s_pos.vz;
    s32 yaw   = ratan2(dx, dz);
    s32 speed = s_run ? DARK_HARRY_RUN : DARK_HARRY_WALK;
    s32 step  = (s32)(((s64)speed * g_DeltaTime) >> 12);

    s_dist = SquareRoot12((s32)((((s64)dx * dx) + ((s64)dz * dz)) >> 12));

    if (s_hold)
    {
        DarkHarry_PoseSet(DH_POSE_STILL);
        DarkHarry_FacePlayer(yaw);
        DarkHarry_EffectsUpdate();
        return;
    }

    if (s_dist <= DARK_HARRY_REACH)
    {
        DarkHarry_Take();
        return;
    }

    DarkHarry_PoseSet(s_run ? DH_POSE_RUN : DH_POSE_WALK);
    s_rot.vy  = (s16)(yaw & 0xFFF);
    s_pos.vx += (s32)(((s64)step * Math_Sin(yaw)) >> 12);
    s_pos.vz += (s32)(((s64)step * Math_Cos(yaw)) >> 12);

    /* Follow the ground rather than the collision system: he is not solid, and
     * a floating or sunken figure breaks the effect instantly. */
    Collision_SurfaceGet(&surf, s_pos.vx, s_pos.vz);
    s_pos.vy = surf.groundHeight;

    DarkHarry_EffectsUpdate();
}

void Pc_DarkHarry_Draw(void)
{
    s_AnmHeader* hdr = (s_AnmHeader*)FS_BUFFER_0;
    if (!s_present || hdr == NULL)
        return;

    if (!s_posed)
    {
        /* Harry's skeleton laid out in OUR coords, not the player's, so posing
         * him cannot disturb the player's own pose. */
        if ((int)hdr->boneCount > DARK_HARRY_BONE_MAX)
        {
            SH_DBG("[DARKHARRY] skeleton needs %d bones, room for %d - not drawing",
                   (int)hdr->boneCount, DARK_HARRY_BONE_MAX);
            DarkHarry_Despawn();
            return;
        }
        Anim_BoneInit(hdr, s_bones);
        s_posed = 1;
    }

    /* Playback writes bones 1+, so the root placement below is still ours
     * to do. */
    Anim_PlaybackLoop(&s_model, hdr, s_bones, &s_poses[s_pose]);

    Math_RotMatrixZxyNegGte(&s_rot, &s_bones->coord);
    s_bones->coord.t[0] = Q12_TO_Q8(s_pos.vx);
    s_bones->coord.t[1] = Q12_TO_Q8(s_pos.vy);
    s_bones->coord.t[2] = Q12_TO_Q8(s_pos.vz);
    s_bones->flg        = 0;

    /* See-through, for this draw only. */
    g_PcCharaGhostDraw = 1;
    func_8003DA9C(Chara_Harry, s_bones, 1, 0, 0);
    g_PcCharaGhostDraw = 0;
}

void Pc_DarkHarry_Update(void)
{
    const char* why;

    if (!s_present)
        return;

    why = DarkHarry_Ineligible();
    if (why != NULL)
    {
        /* Leaving gameplay at all takes him with it, so he can never be left
         * standing in a cutscene or survive a map change. */
        Pc_DarkHarry_Dismiss(why);
        return;
    }

    DarkHarry_Move();
}
