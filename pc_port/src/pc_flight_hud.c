/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_flight_hud.c - fighter-jet style HUD (config key: flight_hud).
 *
 *  - A persistent flight-style overlay: heading tape, speed and altitude tapes,
 *    boresight, target containers on enemies, radar, damage and weapon panels,
 *    time and score.
 *  - Lock warning: an enemy that keeps Harry inside its facing cone, in range,
 *    for AH_LOCK_TIME seconds has a lock. While any lock is held "MISSILE ALERT"
 *    flashes in the middle of the screen and every HUD element turns red (the
 *    crosshair and the touch overlay read Pc_FlightHud_AlertActive too).
 *  - Flares: L3+R3 together (or the touch FLARE button) pops a salvo that breaks
 *    every lock and jams new ones for AH_JAM_TIME. Stock recharges over time.
 *  - A lock from closer than AH_DANGER_RANGE is the third alarm level: faster
 *    tones, "EVADE", and a red pulse on the screen edges.
 *  - Events: "DESTROYED" and a "+1000" on each kill, edge arrows for enemies
 *    out of the picture, text-only radio calls, a "MISSION UPDATE" banner on
 *    the first visit to a zone, and a debrief with a rank after a boss or on
 *    reaching a new chapter.
 *
 * Logic runs from the gameplay state (Pc_FlightHud_Update, game time, so a pause
 * freezes it); drawing runs from the post-capture hook with its own GL program
 * and a full state save/restore, the same arrangement as pc_ra_toast.c. Text is
 * a built-in stroke font, so nothing is read from disk and every platform gets
 * the same HUD.
 */
#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/sys/joy.h"
#include "bodyprog/sound/sfx_id_enum.h"
#include "bodyprog/sound/sound_system.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

#include <SDL.h>
#include <PsyX/common/glad.h>
#include <PsyX/PsyX_backend.h>
#include <libgs.h>
#include <libgpu.h>
#include <libspu.h>

#include "bodyprog/view/vw_calc.h"
#include "bodyprog/view/vw_system.h"
#include "bodyprog/game_boot/fs_chara_anim.h"
#include "bodyprog/screen/screen_data.h"
#include "bodyprog/libsd.h"

#include "sh_log.h"
#include "pc_config.h"
#if defined(__ANDROID__) || defined(SH_IOS)
#include "pc_touch.h"
#define AH_TOUCH_QUICKSAVE() (Pc_Touch_IsDrivingInput() && g_PcConfig.touchQuickSaveLoad)
#else
#define Pc_Touch_IsDrivingInput() 0
#define AH_TOUCH_QUICKSAVE()      0
#endif
#include "pc_quick_options.h"
#include "pc_flight_hud.h"
#include "pc_flight_arcade.h"

extern long   ReadGeomScreen(void);
extern int    PsyX_RawControllerBindHeld(int buttonOrAxis);
extern int    g_PcConsoleInputActive;
extern GLuint GR_ScreenReadFBO(void);

#define AH_PI 3.14159265f

/* ------------------------------------------------------------------ */
/* Tuning                                                              */
/* ------------------------------------------------------------------ */

#define AH_LOCK_RANGE     12.0f  /* m */
#define AH_LOCK_CONE      30.0f  /* deg, half-angle of the enemy's facing cone */
#define AH_LOCK_TIME       0.8f  /* s of continuous tracking before the lock */
#define AH_TRACK_DECAY     2.0f  /* tracking time lost per second out of the cone */
#define AH_JAM_TIME        3.0f  /* s a salvo keeps every seeker blind */
#define AH_FLARE_MAX       4
#define AH_FLARE_RECHARGE 10.0f  /* s per flare */
#define AH_SALVO_PAIRS     4
#define AH_SALVO_STEP      0.08f /* s between pairs */
#define AH_TARGET_RANGE   40.0f  /* m, containers beyond this are not drawn */
#define AH_RADAR_RANGE    25.0f  /* m at the radar rim */

#define AH_DANGER_RANGE    2.5f  /* m, a lock this close is the third alarm level */
#define AH_KILL_MAX        4
#define AH_KILL_TIME       1.2f  /* s "DESTROYED" stays over the wreck */
#define AH_SCORE_FLY       0.9f  /* s the "+1000" takes to reach the score */
#define AH_RADIO_TIME      3.6f
#define AH_RADIO_QUEUE     3
#define AH_RADIO_COOLDOWN 25.0f  /* s before the same kind of call can come again */
#define AH_BANNER_TIME     3.5f
#define AH_BANNER_EDGE     0.35f /* s the banner rules take to open / close */
#define AH_DEBRIEF_TIME    7.0f
#define AH_SEEK_TIME       0.6f  /* s the seeker takes to close onto the auto-aim target */
#define AH_HITFX_MAX       6
#define AH_HITFX_TIME      0.6f  /* s "HIT" / "MISS" stay up */
#define AH_MISS_WAIT       0.4f  /* s after a shot with no damage dealt before it is a miss */
#define AH_HURT_TIME       0.7f  /* s the HUD breaks up after Harry is hit */
#define AH_LOW_HEALTH     25.0f
#define AH_DEAD_FAIL       0.6f  /* s of static before MISSION FAILED */

#define AH_GRAVITY         5.5f  /* m/s^2, +Y is down */
#define AH_FLARE_DRAG      0.6f
#define AH_TRAIL_N         8
#define AH_TRAIL_STEP      0.035f
#define AH_PARTICLES_MAX   (AH_SALVO_PAIRS * 2 * 3)

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

typedef struct
{
    int   alive;
    float x, y, z;       /* m, game axes (Y down) */
    float vx, vy, vz;
    float floorY;
    float life, lifeMax;
    float trailT;
    int   trailN;
    float tx[AH_TRAIL_N], ty[AH_TRAIL_N], tz[AH_TRAIL_N];
} s_AhFlare;

static float s_lockT[NPC_COUNT_MAX];
static int   s_lockChara[NPC_COUNT_MAX];
static int   s_lockState[NPC_COUNT_MAX]; /* 0 none, 1 tracking, 2 locked */
static int   s_anyTrack, s_anyLock;
static int   s_alert;

static int   s_flareStock = AH_FLARE_MAX;
static float s_rechargeT;
static float s_jamT;
static int   s_salvoLeft;
static float s_salvoT;
static float s_flareMsgT, s_emptyMsgT;
static int   s_flareReq;

static float s_beepT;
static int   s_danger;

static float s_lockDist[NPC_COUNT_MAX];
static int   s_wasAlive[NPC_COUNT_MAX];
static float s_hpMax[NPC_COUNT_MAX];
static float s_npcHpPrev[NPC_COUNT_MAX];

static int   s_seekSlot = -1;
static float s_seekT;

typedef struct
{
    float x, y, z;
    float life;
} s_AhHitFx;

static s_AhHitFx s_hitFx[AH_HITFX_MAX];
static int       s_hitFxNext;
static float     s_missT;
static float     s_shotWait;
static int       s_prevFired = -1;
static int       s_hitSinceShot;
static float     s_hurtT, s_hurtAmt;
static float     s_prevHp = -1.0f;
static int       s_dead;
static float     s_deadT;
static float     s_jx, s_jy; /* HUD shake while it breaks up */

typedef struct
{
    float x, y, z;
    float age;
} s_AhKill;

static s_AhKill s_kills[AH_KILL_MAX] = {
    { 0, 0, 0, AH_KILL_TIME }, { 0, 0, 0, AH_KILL_TIME }, { 0, 0, 0, AH_KILL_TIME }, { 0, 0, 0, AH_KILL_TIME }
};
static int      s_killNext;

/* Radio chatter */
enum
{
    AH_RC_LOCK,
    AH_RC_KILL,
    AH_RC_HURT,
    AH_RC_DRY,
    AH_RC_FLARE,
    AH_RC_ZONE,
    AH_RC_BOSS,
    AH_RC_TAUNT, /* the monster that just locked on, on an open channel */
    AH_RC_DOWN,  /* Harry is dead */
    AH_RC_COUNT
};

/* Comm portrait shapes */
enum
{
    AH_FACE_CYBIL,
    AH_FACE_HUMAN, /* nurses, doctors, the possessed Cybil */
    AH_FACE_DOG,
    AH_FACE_BIRD,
    AH_FACE_CHILD,
    AH_FACE_BEAST,
    AH_FACE_COUNT
};

typedef struct
{
    int         cat;
    int         face;
    const char* who; /* NULL: the calling monster's name */
    const char* line;
} s_AhRadioLine;

static const s_AhRadioLine s_radioLines[] = {
    { AH_RC_LOCK,  AH_FACE_CYBIL, "CYBIL", "HARRY, YOU'VE GOT A LOCK ON YOU!" },
    { AH_RC_LOCK,  AH_FACE_CYBIL, "CYBIL", "ONE OF THEM HAS YOU IN ITS SIGHTS!" },
    { AH_RC_KILL,  AH_FACE_CYBIL, "CYBIL", "NICE SHOT, HARRY." },
    { AH_RC_KILL,  AH_FACE_CYBIL, "CYBIL", "TARGET DOWN." },
    { AH_RC_KILL,  AH_FACE_CYBIL, "CYBIL", "SPLASH ONE. KEEP MOVING." },
    { AH_RC_HURT,  AH_FACE_CYBIL, "CYBIL", "YOU'RE HIT BAD. PATCH YOURSELF UP!" },
    { AH_RC_HURT,  AH_FACE_CYBIL, "CYBIL", "HARRY, YOU'RE BLEEDING. HEAL UP!" },
    { AH_RC_DRY,   AH_FACE_CYBIL, "CYBIL", "YOU'RE DRY! RELOAD!" },
    { AH_RC_DRY,   AH_FACE_CYBIL, "CYBIL", "MAGAZINE'S EMPTY, HARRY!" },
    { AH_RC_FLARE, AH_FACE_CYBIL, "CYBIL", "OUT OF FLARES. STAY OUT OF SIGHT." },
    { AH_RC_ZONE,  AH_FACE_CYBIL, "CYBIL", "NEW AREA. KEEP YOUR EYES OPEN." },
    { AH_RC_ZONE,  AH_FACE_CYBIL, "CYBIL", "I DON'T LIKE THE LOOK OF THIS PLACE." },
    { AH_RC_BOSS,  AH_FACE_CYBIL, "CYBIL", "IT'S DOWN! GOOD WORK, HARRY." },
    { AH_RC_DOWN,  AH_FACE_CYBIL, "CYBIL", "HARRY! HARRY, RESPOND!" },
    { AH_RC_DOWN,  AH_FACE_CYBIL, "CYBIL", "HARRY, DO YOU READ? HARRY!" },
    { AH_RC_TAUNT, AH_FACE_DOG,   NULL,    "GRRRRR... RAWR!" },
    { AH_RC_TAUNT, AH_FACE_DOG,   NULL,    "ARF! ARF! GRRRAAAH!" },
    { AH_RC_TAUNT, AH_FACE_BIRD,  NULL,    "SKREEEEEEEE!" },
    { AH_RC_TAUNT, AH_FACE_BIRD,  NULL,    "KRAAAAAAH!" },
    { AH_RC_TAUNT, AH_FACE_CHILD, NULL,    "HEE HEE HEE..." },
    { AH_RC_TAUNT, AH_FACE_CHILD, NULL,    "KYAAAAAA!" },
    { AH_RC_TAUNT, AH_FACE_HUMAN, NULL,    "...HHHHHHH..." },
    { AH_RC_TAUNT, AH_FACE_HUMAN, NULL,    "MMMNNNGH..." },
    { AH_RC_TAUNT, AH_FACE_BEAST, NULL,    "RAWR!" },
    { AH_RC_TAUNT, AH_FACE_BEAST, NULL,    "RAWR! RAWR!" },
    { AH_RC_TAUNT, AH_FACE_BEAST, NULL,    "GRAAAAAAGH!" },
};
#define AH_RADIO_LINES ((int)(sizeof(s_radioLines) / sizeof(s_radioLines[0])))

static int   s_radioQ[AH_RADIO_QUEUE];
static int   s_radioChara[AH_RADIO_QUEUE];
static int   s_radioN;
static float s_radioT;
static int   s_radioLast = -1;
static float s_radioCool[AH_RC_COUNT];
static int   s_newLockSlot = -1;
static int   s_radioPrevLock;
static float s_radioPrevHp = 100.0f;
static int   s_radioPrevWeapon = -1, s_radioPrevAmmo;

/* Zone banner and debrief */
typedef struct
{
    float timer;
    int   kills, fired, hits;
} s_AhStats;

typedef struct
{
    float time;
    int   kills, fired, hits;
    char  rank;
} s_AhDebrief;

static int         s_prevMap = -1;
static unsigned    s_zoneSeen;
static int         s_bannerZone = -1, s_bannerPending = -1;
static float       s_bannerT;
static unsigned    s_chapterSeen;
static int         s_missionBoss;
static int         s_statsValid;
static s_AhStats   s_statsStart;
static float       s_lastTimer;
static s_AhDebrief s_debrief;
static float       s_debriefT;

static s_AhFlare s_flares[AH_PARTICLES_MAX];
static unsigned  s_rng = 0x1234567u;

static float Ah_Rand(void)
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return (float)((s_rng >> 8) & 0xFFFF) / 65535.0f;
}

static float Ah_Turns(q3_12 a)
{
    return (float)(a & 0xFFF) / 4096.0f;
}

static float Ah_Q12f(s32 v)
{
    return (float)v / 4096.0f;
}

int Pc_FlightHud_Enabled(void)
{
    return g_PcConfig.flightHud != 0;
}

int Pc_FlightHud_AlertActive(void)
{
    return Pc_FlightHud_Enabled() && s_alert;
}

void Pc_FlightHud_FlareRequest(void)
{
    s_flareReq = 1;
}

/* ------------------------------------------------------------------ */
/* Stick-click chord guard                                             */
/* ------------------------------------------------------------------ */

int Pc_FlightHud_StickBindDeferred(int sdlButton)
{
    return Pc_FlightHud_Enabled() &&
           (sdlButton == SDL_CONTROLLER_BUTTON_LEFTSTICK || sdlButton == SDL_CONTROLLER_BUTTON_RIGHTSTICK);
}

int Pc_FlightHud_StickBindEdge(int sdlButton, int held, unsigned char* state)
{
    const int prev  = (*state & 1) != 0;
    int       chord = (*state & 2) != 0;
    int       fire;

    if (!Pc_FlightHud_StickBindDeferred(sdlButton))
    {
        *state = (unsigned char)(held ? 1 : 0);
        return held && !prev;
    }

    if (held)
    {
        int other = (sdlButton == SDL_CONTROLLER_BUTTON_LEFTSTICK) ? SDL_CONTROLLER_BUTTON_RIGHTSTICK
                                                                   : SDL_CONTROLLER_BUTTON_LEFTSTICK;
        if (PsyX_RawControllerBindHeld(other))
            chord = 1;
        *state = (unsigned char)(1 | (chord ? 2 : 0));
        return 0;
    }

    fire   = prev && !chord;
    *state = 0;
    return fire;
}

/* ------------------------------------------------------------------ */
/* Logic                                                               */
/* ------------------------------------------------------------------ */

static int Ah_IsEnemy(int charaId)
{
    if (charaId < Chara_AirScreamer || charaId > Chara_MonsterCybil)
        return 0;
    /* Unused, and the two model-less logic twins of the nurse and doctor. */
    return charaId != Chara_Chicken && charaId != Chara_DummyNurse && charaId != Chara_DummyDoctor;
}

static int Ah_NpcLive(const s_SubCharacter* npc)
{
    return Ah_IsEnemy(npc->model.charaId) && npc->health > Q12(0.0f) &&
           npc->collision.state != 0 && npc->collision.state != 1;
}

static int Ah_InGameplay(void)
{
    return g_GameWork.gameState == GameState_InGame && g_SysWork.sysState == SysState_Gameplay &&
           !(g_SysWork.sysFlags & SysFlag_DemoActive);
}

static void Ah_ResetLocks(void)
{
    int i;
    for (i = 0; i < NPC_COUNT_MAX; i++)
    {
        s_lockT[i]     = 0.0f;
        s_lockState[i] = 0;
    }
    s_anyTrack = s_anyLock = s_alert = 0;
}

static void Ah_SpawnOne(int side)
{
    const s_SubCharacter* pl = &g_SysWork.playerWork.player;
    float yaw = Ah_Turns(pl->rotation.vy) * 2.0f * AH_PI;
    float fx = sinf(yaw), fz = cosf(yaw);
    float rx = cosf(yaw), rz = -sinf(yaw);
    float px = Ah_Q12f(pl->position.vx), py = Ah_Q12f(pl->position.vy), pz = Ah_Q12f(pl->position.vz);
    float back, out, up;
    int   i;

    for (i = 0; i < AH_PARTICLES_MAX && s_flares[i].alive; i++)
        ;
    if (i == AH_PARTICLES_MAX)
        return;

    back = 1.2f + Ah_Rand() * 1.4f;
    out  = 2.2f + Ah_Rand() * 1.6f;
    up   = 3.2f + Ah_Rand() * 1.6f;

    s_flares[i].alive   = 1;
    s_flares[i].x       = px - fx * 0.25f + rx * side * 0.2f;
    s_flares[i].y       = py - 1.25f;
    s_flares[i].z       = pz - fz * 0.25f + rz * side * 0.2f;
    s_flares[i].vx      = -fx * back + rx * side * out;
    s_flares[i].vy      = -up;
    s_flares[i].vz      = -fz * back + rz * side * out;
    s_flares[i].floorY  = py;
    s_flares[i].lifeMax = 2.8f + Ah_Rand() * 0.9f;
    s_flares[i].life    = s_flares[i].lifeMax;
    s_flares[i].trailT  = 0.0f;
    s_flares[i].trailN  = 0;
}

static void Ah_FlareSim(float dt)
{
    int i, k;

    for (i = 0; i < AH_PARTICLES_MAX; i++)
    {
        s_AhFlare* f = &s_flares[i];
        float      drag;
        if (!f->alive)
            continue;

        f->life -= dt;
        if (f->life <= 0.0f)
        {
            f->alive = 0;
            continue;
        }

        f->trailT += dt;
        if (f->trailT >= AH_TRAIL_STEP)
        {
            f->trailT = 0.0f;
            for (k = AH_TRAIL_N - 1; k > 0; k--)
            {
                f->tx[k] = f->tx[k - 1];
                f->ty[k] = f->ty[k - 1];
                f->tz[k] = f->tz[k - 1];
            }
            f->tx[0] = f->x;
            f->ty[0] = f->y;
            f->tz[0] = f->z;
            if (f->trailN < AH_TRAIL_N)
                f->trailN++;
        }

        drag   = 1.0f - AH_FLARE_DRAG * dt;
        f->vx *= drag;
        f->vz *= drag;
        f->vy  = f->vy * drag + AH_GRAVITY * dt;
        f->x  += f->vx * dt;
        f->y  += f->vy * dt;
        f->z  += f->vz * dt;

        if (f->y > f->floorY)
        {
            f->y   = f->floorY;
            f->vy  = -f->vy * 0.25f;
            f->vx *= 0.5f;
            f->vz *= 0.5f;
        }
    }
}

int Pc_FlightHud_JamActive(void)
{
    return s_jamT > 0.0f;
}

int Pc_FlightHud_LockState(int slot)
{
    return (slot >= 0 && slot < NPC_COUNT_MAX) ? s_lockState[slot] : 0;
}

int Pc_FlightHud_SeekerLockedSlot(void)
{
    return (s_seekSlot >= 0 && s_seekT >= AH_SEEK_TIME) ? s_seekSlot : -1;
}

int Pc_FlightHud_FlarePositions(float* xyz, int max)
{
    int i, n = 0;
    for (i = 0; i < AH_PARTICLES_MAX && n < max; i++)
    {
        if (!s_flares[i].alive)
            continue;
        xyz[n * 3 + 0] = s_flares[i].x;
        xyz[n * 3 + 1] = s_flares[i].y;
        xyz[n * 3 + 2] = s_flares[i].z;
        n++;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Radio                                                               */
/* ------------------------------------------------------------------ */

static int Ah_FaceOf(int charaId)
{
    switch (charaId)
    {
        case Chara_Groaner:
        case Chara_Romper:
            return AH_FACE_DOG;
        case Chara_AirScreamer:
        case Chara_NightFlutter:
        case Chara_Floatstinger:
            return AH_FACE_BIRD;
        case Chara_GreyChild:
        case Chara_Mumbler:
        case Chara_LarvalStalker:
        case Chara_Stalker:
            return AH_FACE_CHILD;
        case Chara_PuppetNurse:
        case Chara_PuppetDoctor:
        case Chara_MonsterCybil:
            return AH_FACE_HUMAN;
        default:
            return AH_FACE_BEAST;
    }
}

static void Ah_RadioFrom(int cat, int charaId)
{
    const int face = (cat == AH_RC_TAUNT) ? Ah_FaceOf(charaId) : AH_FACE_CYBIL;
    int pick[AH_RADIO_LINES];
    int n = 0, i;

    if (s_radioCool[cat] > 0.0f || s_radioN >= AH_RADIO_QUEUE)
        return;

    for (i = 0; i < AH_RADIO_LINES; i++)
        if (s_radioLines[i].cat == cat && s_radioLines[i].face == face && i != s_radioLast)
            pick[n++] = i;
    if (n == 0)
        return;

    s_radioLast            = pick[(int)(Ah_Rand() * n) % n];
    s_radioQ[s_radioN]     = s_radioLast;
    s_radioChara[s_radioN] = charaId;
    if (s_radioN == 0)
        s_radioT = AH_RADIO_TIME;
    s_radioN++;
    s_radioCool[cat] = AH_RADIO_COOLDOWN;
}

static void Ah_Radio(int cat)
{
    Ah_RadioFrom(cat, 0);
}

static void Ah_RadioTick(float dt)
{
    const s_SubCharacter* pl = &g_SysWork.playerWork.player;
    const float hp = Ah_Q12f(pl->health);
    const int   w  = g_SavegamePtr->equippedWeapon;
    const int   ammo = g_SysWork.playerCombat.currentWeaponAmmo;
    int i;

    for (i = 0; i < AH_RC_COUNT; i++)
        if (s_radioCool[i] > 0.0f)
            s_radioCool[i] -= dt;

    if (s_anyLock && !s_radioPrevLock)
    {
        if (s_newLockSlot >= 0 && Ah_Rand() < 0.6f)
            Ah_RadioFrom(AH_RC_TAUNT, g_SysWork.npcs[s_newLockSlot].model.charaId);
        Ah_Radio(AH_RC_LOCK);
    }
    s_radioPrevLock = s_anyLock;

    if (hp > 0.0f && hp < 25.0f && s_radioPrevHp >= 25.0f)
        Ah_Radio(AH_RC_HURT);
    s_radioPrevHp = hp;

    if (w >= InvItemId_Handgun && w <= InvItemId_Shotgun && w == s_radioPrevWeapon &&
        ammo == 0 && s_radioPrevAmmo > 0)
        Ah_Radio(AH_RC_DRY);
    s_radioPrevWeapon = w;
    s_radioPrevAmmo   = ammo;

    if (s_radioN > 0)
    {
        s_radioT -= dt;
        if (s_radioT <= 0.0f)
        {
            for (i = 1; i < s_radioN; i++)
            {
                s_radioQ[i - 1]     = s_radioQ[i];
                s_radioChara[i - 1] = s_radioChara[i];
            }
            s_radioN--;
            s_radioT = (s_radioN > 0) ? AH_RADIO_TIME : 0.0f;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Zones, chapters, debrief                                            */
/* ------------------------------------------------------------------ */

static const char* const s_zoneNames[] = {
    "OLD SILENT HILL",              /* 0 */
    "CAFE 5TO2",                    /* 1 */
    "MIDWICH ELEMENTARY SCHOOL",    /* 2 */
    "OTHERWORLD SCHOOL",            /* 3 */
    "BALKAN CHURCH",                /* 4 */
    "CENTRAL SILENT HILL",          /* 5 */
    "ALCHEMILLA HOSPITAL",          /* 6 */
    "OTHERWORLD HOSPITAL",          /* 7 */
    "GREEN LION ANTIQUES",          /* 8 */
    "OTHERWORLD CENTRAL TOWN",      /* 9 */
    "CENTRAL SQUARE SHOPPING CENTER", /* 10 */
    "THE SEWERS",                   /* 11 */
    "RESORT AREA",                  /* 12 */
    "ANNIE'S BAR",                  /* 13 */
    "NORMAN'S MOTEL",               /* 14 */
    "OTHERWORLD RESORT AREA",       /* 15 */
    "LAKESIDE PIER",                /* 16 */
    "THE LIGHTHOUSE",               /* 17 */
    "LAKESIDE AMUSEMENT PARK",      /* 18 */
    "NOWHERE",                      /* 19 */
};

/* e_MapIdx -> zone, -1 for maps with no player-facing name. */
static const signed char s_mapZone[] = {
    0, 1, 0,                    /* MAP0 S00..S02 */
    2, 2, 3, 3, 2, 2, 2,        /* MAP1 S00..S06 */
    0, 4, 5, 5, 5,              /* MAP2 S00..S04 */
    6, 6, 6, 7, 7, 7, 6,        /* MAP3 S00..S06 */
    -1, 8, 9, 10, 6, 9, -1,     /* MAP4 S00..S06 */
    11, 12, 13, 14,             /* MAP5 S00..S03 */
    15, 16, 17, 11, 18, -1,     /* MAP6 S00..S05 */
    19, 19, 19, 19,             /* MAP7 S00..S03 */
};

/* MAPn of an e_MapIdx: the game's own chapter split. The story does not walk
 * them in order (the town is MAP2, the school after it MAP1). */
static int Ah_Chapter(int map)
{
    static const signed char first[] = { 0, 3, 10, 15, 22, 29, 33, 39, 43 };
    int c;
    for (c = 0; c < 8; c++)
        if (map >= first[c] && map < first[c + 1])
            return c;
    return -1;
}

static int Ah_IsBoss(int charaId)
{
    return charaId == Chara_SplitHead || charaId == Chara_Floatstinger || charaId == Chara_Twinfeeler ||
           charaId == Chara_Bloodsucker || charaId == Chara_Incubus || charaId == Chara_MonsterCybil;
}

int Pc_FlightHud_IsBoss(int charaId)
{
    return Ah_IsBoss(charaId);
}

static void Ah_StatsNow(s_AhStats* s)
{
    s->timer = Ah_Q12f(g_SavegamePtr->gameplayTimer);
    s->kills = (int)g_SavegamePtr->meleeKillCount + (int)g_SavegamePtr->rangedKillCount;
    s->fired = g_SavegamePtr->firedShotCount;
    s->hits  = g_SavegamePtr->closeRangeShotCount + g_SavegamePtr->midRangeShotCount +
               g_SavegamePtr->longRangeShotCount;
}

/* Accuracy is worth up to 60 points and kills up to 40 (4 each). A mission
 * fought with no shot fired gets three quarters of the accuracy points. */
static char Ah_Rank(int kills, int fired, int hits)
{
    float acc = (fired > 0) ? (float)hits / (float)fired : 0.75f;
    float pts = (acc > 1.0f ? 1.0f : acc) * 60.0f + (kills > 10 ? 10 : kills) * 4.0f;

    if (pts >= 85.0f) return 'S';
    if (pts >= 70.0f) return 'A';
    if (pts >= 50.0f) return 'B';
    return 'C';
}

static void Ah_Debrief(int always)
{
    s_AhStats now;

    Ah_StatsNow(&now);
    /* A stretch with nothing fought, like the opening walk, has nothing to grade. */
    if (!always && now.kills == s_statsStart.kills && now.fired == s_statsStart.fired)
    {
        s_statsStart = now;
        return;
    }
    s_debrief.time  = now.timer - s_statsStart.timer;
    s_debrief.kills = now.kills - s_statsStart.kills;
    s_debrief.fired = now.fired - s_statsStart.fired;
    s_debrief.hits  = now.hits - s_statsStart.hits;
    s_debrief.rank  = Ah_Rank(s_debrief.kills, s_debrief.fired, s_debrief.hits);
    s_debriefT      = AH_DEBRIEF_TIME;
    s_statsStart    = now;
    SH_DBG("[FLIGHTHUD] debrief: %d kills, %d/%d hits, rank %c", s_debrief.kills, s_debrief.hits,
           s_debrief.fired, s_debrief.rank);
}

static void Ah_ZoneTick(float dt)
{
    const int map = g_SavegamePtr->mapIdx;
    s_AhStats now;
    int       zone, chapter;

    Ah_StatsNow(&now);
    /* Counters running backwards mean another save was loaded: start over
     * from it, and do not greet its map as a new zone. */
    if (!s_statsValid || now.timer < s_lastTimer || now.kills < s_statsStart.kills ||
        now.fired < s_statsStart.fired)
    {
        s_statsStart = now;
        s_statsValid = 1;
        s_prevMap    = -1;
    }
    s_lastTimer = now.timer;

    if (s_debriefT > 0.0f)
        s_debriefT -= dt;

    zone    = (map >= 0 && map < (int)sizeof(s_mapZone)) ? s_mapZone[map] : -1;
    chapter = Ah_Chapter(map);

    if (map != s_prevMap)
    {
        Pc_FlightArcade_Reset();
        if (s_prevMap >= 0)
        {
            if (chapter >= 0 && !(s_chapterSeen & (1u << chapter)))
            {
                if (s_missionBoss)
                    s_statsStart = now;
                else
                    Ah_Debrief(0);
                s_missionBoss = 0;
            }
            if (zone >= 0 && !(s_zoneSeen & (1u << zone)))
                s_bannerPending = zone;
        }
        if (zone >= 0)
            s_zoneSeen |= 1u << zone;
        if (chapter >= 0)
            s_chapterSeen |= 1u << chapter;
        s_prevMap = map;
    }

    if (s_bannerPending >= 0 && s_debriefT <= 0.0f)
    {
        s_bannerZone    = s_bannerPending;
        s_bannerPending = -1;
        s_bannerT       = AH_BANNER_TIME;
        Ah_Radio(AH_RC_ZONE);
    }
    if (s_bannerT > 0.0f)
        s_bannerT -= dt;
}

static void Ah_OnKill(const s_SubCharacter* npc)
{
    s_AhKill* k = &s_kills[s_killNext];

    k->x   = Ah_Q12f(npc->position.vx + npc->collision.shapeOffsets.box.vx);
    k->y   = Ah_Q12f(npc->position.vy + npc->collision.box.offsetY);
    k->z   = Ah_Q12f(npc->position.vz + npc->collision.shapeOffsets.box.vz);
    k->age = 0.0f;
    s_killNext = (s_killNext + 1) % AH_KILL_MAX;

    if (g_PcConfig.flightHudSound)
        SD_Call(Sfx_MenuConfirm);

    if (Ah_IsBoss(npc->model.charaId))
    {
        Ah_Radio(AH_RC_BOSS);
        Ah_Debrief(1);
        s_missionBoss = 1;
    }
    else
    {
        Ah_Radio(AH_RC_KILL);
    }
}

static void Ah_OnHit(const s_SubCharacter* npc)
{
    s_AhHitFx* h = &s_hitFx[s_hitFxNext];

    h->x    = Ah_Q12f(npc->position.vx + npc->collision.shapeOffsets.box.vx);
    h->y    = Ah_Q12f(npc->position.vy + npc->collision.box.offsetY);
    h->z    = Ah_Q12f(npc->position.vz + npc->collision.shapeOffsets.box.vz);
    h->life = AH_HITFX_TIME;
    s_hitFxNext = (s_hitFxNext + 1) % AH_HITFX_MAX;
}

/* Runs before the lock scan: a shot this frame opens the miss window, and any
 * damage the scan sees (this frame or the next few) closes it. */
static void Ah_CombatTick(float dt)
{
    const s_SubCharacter* pl = &g_SysWork.playerWork.player;
    const int   fired = g_SavegamePtr->firedShotCount;
    const int   w     = g_SavegamePtr->equippedWeapon;
    const float hp    = Ah_Q12f(pl->health);
    int         k, slot = -1;

    for (k = 0; k < AH_HITFX_MAX; k++)
        if (s_hitFx[k].life > 0.0f)
            s_hitFx[k].life -= dt;
    if (s_missT > 0.0f)
        s_missT -= dt;

    if (s_shotWait > 0.0f)
    {
        if (s_hitSinceShot)
            s_shotWait = 0.0f;
        else
        {
            s_shotWait -= dt;
            if (s_shotWait <= 0.0f)
                s_missT = AH_HITFX_TIME;
        }
    }
    if (s_prevFired >= 0 && fired > s_prevFired)
    {
        s_shotWait     = AH_MISS_WAIT;
        s_hitSinceShot = 0;
    }
    s_prevFired = fired;

    if (s_prevHp >= 0.0f && hp > 0.0f && hp < s_prevHp - 0.01f)
    {
        s_hurtT   = AH_HURT_TIME;
        s_hurtAmt = (s_prevHp - hp) / 25.0f;
        if (s_hurtAmt < 0.35f) s_hurtAmt = 0.35f;
        if (s_hurtAmt > 1.0f)  s_hurtAmt = 1.0f;
    }
    s_prevHp = hp;
    if (s_hurtT > 0.0f)
        s_hurtT -= dt;

    if (hp <= 0.0f)
    {
        if (!s_dead)
        {
            s_dead  = 1;
            s_deadT = 0.0f;
            s_radioN = 0;
            s_radioCool[AH_RC_DOWN] = 0.0f;
            Ah_Radio(AH_RC_DOWN);
        }
        s_deadT += dt;
    }
    else
    {
        s_dead  = 0;
        s_deadT = 0.0f;
    }

    /* The seeker rides the game's own auto-aim pick, so it closes on exactly
     * what a shot would hit. Firearms only. */
    if (g_SysWork.playerCombat.isAiming && w >= InvItemId_Handgun && w <= InvItemId_HyperBlaster && hp > 0.0f)
    {
        const int t = g_SysWork.targetNpcIdx;
        if (t >= 0 && t < NPC_COUNT_MAX && Ah_NpcLive(&g_SysWork.npcs[t]))
            slot = t;
    }
    if (slot != s_seekSlot)
    {
        s_seekSlot = slot;
        s_seekT    = 0.0f;
    }
    else if (slot >= 0 && s_seekT < AH_SEEK_TIME)
    {
        s_seekT += dt;
    }
}

static void Ah_TryLaunch(void)
{
    if (s_salvoLeft > 0)
        return;

    if (s_flareStock <= 0)
    {
        s_emptyMsgT = 1.2f;
        SD_Call(Sfx_MenuError);
        return;
    }

    s_flareStock--;
    if (s_flareStock == 0)
        Ah_Radio(AH_RC_FLARE);
    s_salvoLeft = AH_SALVO_PAIRS;
    s_salvoT    = 0.0f;
    s_jamT      = AH_JAM_TIME;
    s_flareMsgT = 1.6f;
    Ah_ResetLocks();
    SD_Call(Sfx_MenuConfirm);
    SH_DBG("[FLIGHTHUD] flares away, %d left", s_flareStock);
}

static void Ah_LockScan(float dt)
{
    const s_SubCharacter* pl = &g_SysWork.playerWork.player;
    const float cone = AH_LOCK_CONE / 360.0f;
    int i;

    s_anyTrack = s_anyLock = s_danger = 0;
    s_newLockSlot = -1;

    for (i = 0; i < NPC_COUNT_MAX; i++)
    {
        const s_SubCharacter* npc = &g_SysWork.npcs[i];
        const int alive = Ah_IsEnemy(npc->model.charaId) && npc->health > Q12(0.0f);
        int   tracking = 0;

        if (s_lockChara[i] != npc->model.charaId)
        {
            s_lockChara[i] = npc->model.charaId;
            s_lockT[i]     = 0.0f;
            s_wasAlive[i]  = 0;
        }

        {
            const float hpNow = alive ? Ah_Q12f(npc->health) : 0.0f;
            if (s_wasAlive[i] && hpNow < s_npcHpPrev[i] - 0.01f)
            {
                s_hitSinceShot = 1;
                if (alive)
                    Ah_OnHit(npc);
            }
            s_npcHpPrev[i] = hpNow;
        }

        /* Same slot, same monster, health just ran out: that is a kill, not
         * a slot being freed (that clears charaId) or reused. */
        if (s_wasAlive[i] && !alive && Ah_IsEnemy(npc->model.charaId))
            Ah_OnKill(npc);
        if (alive)
        {
            float hp = Ah_Q12f(npc->health);
            if (!s_wasAlive[i] || hp > s_hpMax[i])
                s_hpMax[i] = hp;
        }
        s_wasAlive[i] = alive;

        if (Ah_NpcLive(npc) && s_jamT <= 0.0f && pl->health > Q12(0.0f))
        {
            float dx = Ah_Q12f(pl->position.vx - npc->position.vx);
            float dz = Ah_Q12f(pl->position.vz - npc->position.vz);
            float dist = sqrtf(dx * dx + dz * dz);

            s_lockDist[i] = dist;
            if (dist < AH_LOCK_RANGE)
            {
                float to   = atan2f(dx, dz) / (2.0f * AH_PI);
                float diff = to - Ah_Turns(npc->rotation.vy);
                diff -= floorf(diff + 0.5f);
                tracking = fabsf(diff) <= cone;
            }
        }

        if (tracking)
        {
            s_lockT[i] += dt;
            if (s_lockT[i] > AH_LOCK_TIME)
                s_lockT[i] = AH_LOCK_TIME;
        }
        else
        {
            s_lockT[i] -= dt * AH_TRACK_DECAY;
            if (s_lockT[i] < 0.0f)
                s_lockT[i] = 0.0f;
        }

        if (s_lockT[i] >= AH_LOCK_TIME)
        {
            if (s_lockState[i] != 2)
                s_newLockSlot = i;
            s_lockState[i] = 2;
        }
        else if (tracking)
            s_lockState[i] = 1;
        else if (s_lockState[i] == 2 && s_lockT[i] > 0.0f)
            s_lockState[i] = 2; /* a lock survives a brief break, like a seeker coasting */
        else
            s_lockState[i] = 0;

        if (s_lockState[i] == 2)
        {
            s_anyLock = 1;
            if (s_lockDist[i] < AH_DANGER_RANGE)
                s_danger = 1;
        }
        else if (s_lockState[i] == 1)
        {
            s_anyTrack = 1;
        }
    }

    /* A missile already flying keeps the alarm up after the lock that fired
     * it is gone. */
    if (Pc_FlightArcade_Inbound())
        s_danger = 1;
    s_alert = s_anyLock || s_danger;
}

/* ------------------------------------------------------------------ */
/* Alarm tone                                                          */
/* ------------------------------------------------------------------ */

/* A fighter's radar warning tones, made here: one looped stretch of a reedy
 * wave sits in SPU RAM (the 64 bytes libsd leaves under the reverb area) and
 * the pitch and volume registers play every pattern on it. It goes through the
 * game's own SPU, so it is mixed, paused and volume-scaled like any effect on
 * every renderer and on phones. Voices 22 and 23 are the radio's, so the tone
 * holds 21, and only while it sounds. */
#define AH_TONE_VOICE   21
#define AH_TONE_SAMPLES 112 /* four ADPCM blocks */
#define AH_TONE_CYCLES  4
#define AH_TONE_BASE_HZ (44100.0f * AH_TONE_CYCLES / AH_TONE_SAMPLES)

enum
{
    AH_TONE_NONE = 0,
    AH_TONE_TRACK,  /* WARNING: a short two-note warble, twice a second */
    AH_TONE_LOCK,   /* MISSILE ALERT: fast beeps on one note */
    AH_TONE_LAUNCH, /* EVADE: a continuous siren sweep */
    AH_TONE_SEEK,   /* Harry's seeker hunting: a low, fluttering growl */
    AH_TONE_SEEKLOCK, /* Harry's seeker on: a steady high tone */
    AH_TONE_LOWHP   /* low health: a slow double beep */
};

static u32   s_toneAddr;
static int   s_toneMode;
static float s_toneT;

static void Ah_ToneEncode(u8* out)
{
    static const int pos[5] = { 0, 60, 115, 98, 122 };
    static const int neg[5] = { 0, 0, -52, -55, -60 };
    s16 pcm[AH_TONE_SAMPLES];
    int s1 = 0, s2 = 0, b, i;

    for (i = 0; i < AH_TONE_SAMPLES; i++)
    {
        const float ph = 2.0f * AH_PI * AH_TONE_CYCLES * (float)i / AH_TONE_SAMPLES;
        const float v  = sinf(ph) + 0.33f * sinf(3.0f * ph) + 0.16f * sinf(5.0f * ph);
        pcm[i] = (s16)(v * 15000.0f);
    }

    for (b = 0; b < AH_TONE_SAMPLES / 28; b++)
    {
        const s16* src = &pcm[b * 28];
        u8*        blk = &out[b * 16];
        double     bestErr = -1.0;
        int        bestF = 0, bestSh = 0, bestS1 = 0, bestS2 = 0, f, sh;
        signed char bestQ[28];

        /* The first block is the loop start, where the history is whatever the
         * tail left, so it must not predict. */
        for (f = 0; f < (b == 0 ? 1 : 5); f++)
        {
            for (sh = 0; sh <= 12; sh++)
            {
                const int   step = 1 << (12 - sh);
                int         p1 = s1, p2 = s2;
                double      err = 0.0;
                signed char q[28];

                for (i = 0; i < 28; i++)
                {
                    const int pred = (p1 * pos[f] + p2 * neg[f] + 32) >> 6;
                    int       n    = (int)lrintf((float)(src[i] - pred) / (float)step);
                    int       dec;

                    if (n < -8) n = -8;
                    if (n >  7) n =  7;
                    dec = n * step + pred;
                    if (dec < -32768) dec = -32768;
                    if (dec >  32767) dec =  32767;
                    err += (double)(src[i] - dec) * (double)(src[i] - dec);
                    q[i] = (signed char)n;
                    p2 = p1;
                    p1 = dec;
                }
                if (bestErr < 0.0 || err < bestErr)
                {
                    bestErr = err;
                    bestF   = f;
                    bestSh  = sh;
                    bestS1  = p1;
                    bestS2  = p2;
                    memcpy(bestQ, q, sizeof(q));
                }
            }
        }

        memset(blk, 0, 16);
        blk[0] = (u8)((bestF << 4) | bestSh);
        blk[1] = (b == 0) ? 0x04 : (b == AH_TONE_SAMPLES / 28 - 1) ? 0x03 : 0x00;
        for (i = 0; i < 28; i++)
            blk[2 + i / 2] |= (u8)((bestQ[i] & 0xF) << ((i & 1) * 4));
        s1 = bestS1;
        s2 = bestS2;
    }
}

/* The slot follows the reverb mode, so it is checked at every start. */
static int Ah_ToneUpload(void)
{
    u8        wave[AH_TONE_SAMPLES / 28 * 16];
    const u32 addr = (u32)SD_SPU_ALLOC_TOP;

    if (sizeof(wave) > SD_PC_TONE_BYTES)
        return 0;
    if (s_toneAddr == addr)
        return 1;
    Ah_ToneEncode(wave);
    SpuSetTransferStartAddr(addr);
    if (SpuWrite(wave, sizeof(wave)) != sizeof(wave))
        return 0;
    s_toneAddr = addr;
    return 1;
}

static void Ah_ToneStop(void)
{
    if (s_toneMode == AH_TONE_NONE)
        return;
    s_toneMode = AH_TONE_NONE;
    SdPcHoldVoice(NO_VALUE);
}

/* Pitch and volume this frame; vol 0 is the gap between beeps. The voice
 * stays keyed on so the driver's free-voice scans keep passing it by. */
static void Ah_ToneSet(float hz, float vol)
{
    SpuVoiceAttr a;
    float        se = (float)Sd_GetVolSe(128) / 128.0f;
    int          p  = (int)(hz / AH_TONE_BASE_HZ * 4096.0f + 0.5f);
    int          v  = (int)(vol * se * 0x1C00);

    if (p < 1)      p = 1;
    if (p > 0x3FFF) p = 0x3FFF;
    memset(&a, 0, sizeof(a));
    a.voice        = spu_ch_tbl[AH_TONE_VOICE];
    a.mask         = SPU_VOICE_VOLL | SPU_VOICE_VOLR | SPU_VOICE_PITCH;
    a.volume.left  = (short)v;
    a.volume.right = (short)v;
    a.pitch        = (unsigned short)p;
    SpuSetVoiceAttr(&a);
}

static void Ah_ToneStart(int mode)
{
    SpuVoiceAttr a;

    if (!Ah_ToneUpload())
        return;
    SdPcHoldVoice(AH_TONE_VOICE);

    memset(&a, 0, sizeof(a));
    a.voice   = spu_ch_tbl[AH_TONE_VOICE];
    a.mask    = SPU_VOICE_VOLL | SPU_VOICE_VOLR | SPU_VOICE_VOLMODEL | SPU_VOICE_VOLMODER |
                SPU_VOICE_PITCH | SPU_VOICE_WDSA | SPU_VOICE_LSAX |
                SPU_VOICE_ADSR_AMODE | SPU_VOICE_ADSR_ADSR1 | SPU_VOICE_ADSR_ADSR2;
    a.pitch     = 0x1000;
    a.addr      = s_toneAddr;
    a.loop_addr = s_toneAddr;
    /* Attack and release of about a hundredth of a second, so a beep starts
     * and stops without a click; full sustain in between. */
    a.adsr1  = (8 << 10) | 0x000F;
    a.adsr2  = (0x1F << 8) | 0x0008;
    a.a_mode = 1;
    SpuSetVoiceAttr(&a);
    SpuSetKey(SPU_ON, spu_ch_tbl[AH_TONE_VOICE]);

    s_toneMode = mode;
    s_toneT    = 0.0f;
}

static void Ah_Tones(float dt)
{
    const float hp = Ah_Q12f(g_SysWork.playerWork.player.health);
    int         mode;
    float       t, hz, vol;

    /* Threats first: being shot at outranks aiming, aiming outranks the
     * health warning. */
    if (!g_PcConfig.flightHudSound || s_dead)
        mode = AH_TONE_NONE;
    else if (s_danger)
        mode = AH_TONE_LAUNCH;
    else if (s_anyLock)
        mode = AH_TONE_LOCK;
    else if (s_anyTrack)
        mode = AH_TONE_TRACK;
    else if (s_seekSlot >= 0)
        mode = s_seekT >= AH_SEEK_TIME ? AH_TONE_SEEKLOCK : AH_TONE_SEEK;
    else if (hp > 0.0f && hp < AH_LOW_HEALTH)
        mode = AH_TONE_LOWHP;
    else
        mode = AH_TONE_NONE;

    if (mode == AH_TONE_NONE)
    {
        Ah_ToneStop();
        return;
    }
    if (mode != s_toneMode)
    {
        Ah_ToneStop();
        Ah_ToneStart(mode);
    }
    if (s_toneMode != mode)
    {
        /* No room for the wave: the menu tick as before, for threats only. */
        if (mode >= AH_TONE_SEEK)
            return;
        s_beepT -= dt;
        if (s_beepT <= 0.0f)
        {
            SD_Call(Sfx_MenuMove);
            s_beepT = mode == AH_TONE_LAUNCH ? 0.08f : (mode == AH_TONE_LOCK ? 0.16f : 0.55f);
        }
        return;
    }

    /* Something else keyed the voice off (an all-effects stop): key it again. */
    if (SpuGetKeyStatus(spu_ch_tbl[AH_TONE_VOICE]) == SPU_OFF)
        SpuSetKey(SPU_ON, spu_ch_tbl[AH_TONE_VOICE]);

    s_toneT += dt;
    t = s_toneT;
    switch (mode)
    {
        case AH_TONE_TRACK:
        {
            const float ph = fmodf(t, 0.6f);
            hz  = (fmodf(ph, 0.08f) < 0.04f) ? 1150.0f : 900.0f;
            vol = (ph < 0.24f) ? 0.8f : 0.0f;
            break;
        }
        case AH_TONE_LOCK:
            hz  = 1000.0f;
            vol = (fmodf(t, 0.125f) < 0.07f) ? 1.0f : 0.0f;
            break;
        case AH_TONE_SEEK:
            hz  = 400.0f + 30.0f * sinf(t * 2.0f * AH_PI * 6.0f);
            vol = 0.45f + 0.2f * sinf(t * 2.0f * AH_PI * 11.0f);
            break;
        case AH_TONE_SEEKLOCK:
            hz  = 1600.0f;
            vol = 0.7f;
            break;
        case AH_TONE_LOWHP:
        {
            const float ph = fmodf(t, 1.4f);
            hz  = 620.0f;
            vol = (ph < 0.1f || (ph > 0.18f && ph < 0.28f)) ? 0.6f : 0.0f;
            break;
        }
        default:
        {
            const float sw = fmodf(t * 6.0f, 2.0f);
            hz  = 850.0f + 700.0f * (sw < 1.0f ? sw : 2.0f - sw);
            vol = 1.0f;
            break;
        }
    }
    Ah_ToneSet(hz, vol);
}

void Pc_FlightHud_Update(void)
{
    float dt;
    int   held, clicked;

    if (!Pc_FlightHud_Enabled())
    {
        Ah_ResetLocks();
        Ah_ToneStop();
        s_flareReq = 0;
        Pc_FlightArcade_Reset();
        return;
    }

    if (!Ah_InGameplay())
    {
        Ah_ToneStop();
        s_flareReq = 0;
        return;
    }

    dt = Ah_Q12f(g_DeltaTime);
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.1f) dt = 0.1f;

    if (s_flareStock < AH_FLARE_MAX)
    {
        s_rechargeT += dt;
        if (s_rechargeT >= AH_FLARE_RECHARGE)
        {
            s_rechargeT = 0.0f;
            s_flareStock++;
        }
    }
    else
    {
        s_rechargeT = 0.0f;
    }

    held    = (int)g_Controller0->heldBtnFlags;
    clicked = (int)g_Controller0->clickedBtnFlags;
    if (!g_PcConsoleInputActive && !g_PcQuickOptionsActive)
    {
        int chord = (held & ControllerFlag_L3) && (held & ControllerFlag_R3) &&
                    (clicked & (ControllerFlag_L3 | ControllerFlag_R3));
        if (chord || s_flareReq)
            Ah_TryLaunch();
    }
    s_flareReq = 0;

    if (s_salvoLeft > 0)
    {
        s_salvoT -= dt;
        if (s_salvoT <= 0.0f)
        {
            Ah_SpawnOne(-1);
            Ah_SpawnOne(1);
            s_salvoLeft--;
            s_salvoT = AH_SALVO_STEP;
        }
    }

    if (s_jamT > 0.0f)       s_jamT -= dt;
    if (s_flareMsgT > 0.0f)  s_flareMsgT -= dt;
    if (s_emptyMsgT > 0.0f)  s_emptyMsgT -= dt;

    {
        int k;
        for (k = 0; k < AH_KILL_MAX; k++)
            if (s_kills[k].age < AH_KILL_TIME)
                s_kills[k].age += dt;
    }

    Ah_FlareSim(dt);
    Ah_ZoneTick(dt);
    Ah_CombatTick(dt);
    Ah_LockScan(dt);
    Pc_FlightArcade_Update(dt);
    Ah_RadioTick(dt);
    Ah_Tones(dt);
}

/* ------------------------------------------------------------------ */
/* Geometry batch                                                      */
/* ------------------------------------------------------------------ */

/* HUD space: 480 units tall whatever the resolution, origin at the centre of the
 * picture, +y down. s_w2 is the half width in those units. */
#define AH_VERT_FLOATS 6
#define AH_HUD_VERTS   48000
#define AH_GLOW_VERTS  8000
#define AH_FILL_VERTS  600

static float s_hudV[AH_HUD_VERTS * AH_VERT_FLOATS];
static float s_glowV[AH_GLOW_VERTS * AH_VERT_FLOATS];
static float s_fillV[AH_FILL_VERTS * AH_VERT_FLOATS];

typedef struct
{
    float* v;
    int    n, cap;
} s_AhBatch;

static s_AhBatch s_hud  = { s_hudV, 0, AH_HUD_VERTS };
static s_AhBatch s_glow = { s_glowV, 0, AH_GLOW_VERTS };
static s_AhBatch s_fill = { s_fillV, 0, AH_FILL_VERTS }; /* translucent panels: no shadow pass */
static s_AhBatch* s_cur = &s_hud;

static float s_w2 = 320.0f;
static float s_col[4];

static void Ah_Color(float r, float g, float b, float a)
{
    s_col[0] = r; s_col[1] = g; s_col[2] = b; s_col[3] = a;
}

static void Ah_V(float x, float y, const float* c)
{
    float* p;
    if (s_cur->n >= s_cur->cap)
        return;
    p    = &s_cur->v[s_cur->n * AH_VERT_FLOATS];
    p[0] = (x + s_jx) / s_w2;
    p[1] = -(y + s_jy) / 240.0f;
    p[2] = c[0]; p[3] = c[1]; p[4] = c[2]; p[5] = c[3];
    s_cur->n++;
}

static void Ah_Tri(float x0, float y0, float x1, float y1, float x2, float y2)
{
    if (s_cur->n + 3 > s_cur->cap)
        return;
    Ah_V(x0, y0, s_col);
    Ah_V(x1, y1, s_col);
    Ah_V(x2, y2, s_col);
}

static void Ah_Quad(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3)
{
    Ah_Tri(x0, y0, x1, y1, x2, y2);
    Ah_Tri(x0, y0, x2, y2, x3, y3);
}

static void Ah_Rect(float l, float t, float r, float b)
{
    Ah_Quad(l, t, r, t, r, b, l, b);
}

static void Ah_Line(float x0, float y0, float x1, float y1, float th)
{
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    float nx, ny, ex, ey;

    if (len < 0.001f)
    {
        Ah_Rect(x0 - th * 0.5f, y0 - th * 0.5f, x0 + th * 0.5f, y0 + th * 0.5f);
        return;
    }
    nx = -dy / len * th * 0.5f;
    ny =  dx / len * th * 0.5f;
    /* Square caps, so strokes meet without notches at the joints. */
    ex = dx / len * th * 0.5f;
    ey = dy / len * th * 0.5f;
    Ah_Quad(x0 - ex + nx, y0 - ey + ny, x1 + ex + nx, y1 + ey + ny,
            x1 + ex - nx, y1 + ey - ny, x0 - ex - nx, y0 - ey - ny);
}

static void Ah_Box(float l, float t, float r, float b, float th)
{
    Ah_Line(l, t, r, t, th);
    Ah_Line(r, t, r, b, th);
    Ah_Line(r, b, l, b, th);
    Ah_Line(l, b, l, t, th);
}

static void Ah_Circle(float cx, float cy, float rad, float th, int seg)
{
    int i;
    for (i = 0; i < seg; i++)
    {
        float a0 = (float)i / seg * 2.0f * AH_PI;
        float a1 = (float)(i + 1) / seg * 2.0f * AH_PI;
        Ah_Line(cx + cosf(a0) * rad, cy + sinf(a0) * rad, cx + cosf(a1) * rad, cy + sinf(a1) * rad, th);
    }
}

/* Soft disc: opaque centre fading to a transparent rim. */
static void Ah_Glow(float cx, float cy, float rad, const float* inner, const float* outer)
{
    const int seg = 14;
    int i;
    if (s_cur->n + seg * 3 > s_cur->cap)
        return;
    for (i = 0; i < seg; i++)
    {
        float a0 = (float)i / seg * 2.0f * AH_PI;
        float a1 = (float)(i + 1) / seg * 2.0f * AH_PI;
        Ah_V(cx, cy, inner);
        Ah_V(cx + cosf(a0) * rad, cy + sinf(a0) * rad, outer);
        Ah_V(cx + cosf(a1) * rad, cy + sinf(a1) * rad, outer);
    }
}

/* ------------------------------------------------------------------ */
/* Stroke font                                                         */
/* ------------------------------------------------------------------ */

/* Each glyph is a run of segments "x0y0x1y1" on a 4x6 grid, top-left origin.
 * Thin vector strokes are what the real HUD uses, and they scale to any
 * resolution without a texture. */
static const char* const s_glyph[128] = {
    ['A'] = "06020220204242460343",
    ['B'] = "0006003030414142423303333344444545363606",
    ['C'] = "40101001010505161646",
    ['D'] = "000600303041414545363606",
    ['E'] = "4000000606460333",
    ['F'] = "400000060333",
    ['G'] = "413030101001010505161636364545434323",
    ['H'] = "000640460343",
    ['I'] = "103020261636",
    ['J'] = "4045453636161605",
    ['K'] = "000640030346",
    ['L'] = "00060646",
    ['M'] = "0600002323404046",
    ['N'] = "060000464640",
    ['O'] = "10303041414545363616160505010110",
    ['P'] = "060000303041414242333303",
    ['Q'] = "103030414145453636161605050101102446",
    ['R'] = "0600003030414142423333032346",
    ['S'] = "41303010100101020213133333444445453636161605",
    ['T'] = "00402026",
    ['U'] = "00050516163636454540",
    ['V'] = "00262640",
    ['W'] = "0016162323363640",
    ['X'] = "00464006",
    ['Y'] = "002340232326",
    ['Z'] = "004040060646",
    ['0'] = "103030414145453636161605050101104105",
    ['1'] = "112020261636",
    ['2'] = "011010303041414242060646",
    ['3'] = "01101030304141424233331333444445453636161605",
    ['4'] = "363030040444",
    ['5'] = "4000000303333344444545363606",
    ['6'] = "301010010105051616363645454444333303",
    ['7'] = "00404016",
    ['8'] = "103030414142423333131302020101103344444545363616160505040413",
    ['9'] = "431313020201011010303041414545363616",
    ['-'] = "1333",
    ['.'] = "2526",
    ['/'] = "4006",
    ['%'] = "064000101011110101003545454646363635",
    [':'] = "21222425",
    ['<'] = "40030346",
    ['>'] = "00434306",
    ['+'] = "21250343",
    ['['] = "301010161636",
    [']'] = "103030363616",
    ['!'] = "20242526",
    ['^'] = "03202043",
    ['|'] = "2026",
    ['='] = "02420444",
    ['\''] = "2021",
    [','] = "25262617",
    ['?'] = "0110103030414142422323242526",
};

#define AH_ADV 5.6f /* grid units per character */

static float Ah_TextWidth(const char* s, float size)
{
    int n = (int)strlen(s);
    return (n > 0) ? ((n * AH_ADV) - (AH_ADV - 4.0f)) * (size / 6.0f) : 0.0f;
}

/* align: 0 left, 1 centre, 2 right. y is the top of the glyphs. */
static void Ah_Text(const char* s, float x, float y, float size, int align)
{
    const float u  = size / 6.0f;
    const float th = (size * 0.12f < 1.1f) ? 1.1f : size * 0.12f;
    float       pen;

    if (align == 1)      x -= Ah_TextWidth(s, size) * 0.5f;
    else if (align == 2) x -= Ah_TextWidth(s, size);
    pen = x;

    for (; *s; s++)
    {
        int         c = (unsigned char)*s;
        const char* g;
        if (c >= 'a' && c <= 'z')
            c -= 'a' - 'A';
        g = (c < 128) ? s_glyph[c] : NULL;
        for (; g && g[0] && g[1] && g[2] && g[3]; g += 4)
        {
            Ah_Line(pen + (g[0] - '0') * u, y + (g[1] - '0') * u,
                    pen + (g[2] - '0') * u, y + (g[3] - '0') * u, th);
        }
        pen += AH_ADV * u;
    }
}

/* ------------------------------------------------------------------ */
/* Projection                                                          */
/* ------------------------------------------------------------------ */

static float   s_camH;
/* Projected PSX offsets from the picture centre -> HUD units:
 * h = s_c + p * s_k. Taken from the ortho the world pass really used, so the
 * widening, crop and pixel aspect of each device are whatever the renderer
 * chose; the 4:3 Hor+ guess before it put boxes well off the monsters on
 * wide phones. */
static float   s_kx = 1.0f, s_ky = 1.0f, s_cx, s_cy;

extern s32   Pc_WorldAnchorOfy(void);
extern float g_PsxWorldOrtho[4];
extern float g_PsxWorldDisp[2];
extern int   g_PsxWorldOrthoValid;

static void Ah_ScreenMap(float aspect)
{
    const float ofy = (float)Pc_WorldAnchorOfy();
    const float *o = g_PsxWorldOrtho, *d = g_PsxWorldDisp;

    if (g_PsxWorldOrthoValid && o[1] > o[0] && o[3] > o[2] && d[0] > 0.0f && d[1] > 0.0f)
    {
        s_kx = 2.0f * s_w2 / (o[1] - o[0]);
        s_cx = (d[0] * 0.5f - o[0]) * s_kx - s_w2;
        s_ky = 480.0f / (o[3] - o[2]);
        s_cy = (d[1] * 0.5f + ofy - o[2]) * s_ky - 240.0f;
    }
    else
    {
        const float halfH = (g_GameWork.gsScreenHeight > 0) ? g_GameWork.gsScreenHeight * 0.5f : 120.0f;
        s_kx = s_w2 / (120.0f * aspect);
        s_cx = 0.0f;
        s_ky = 240.0f / halfH;
        s_cy = ofy * s_ky;
    }
}

static void Ah_View(float wx, float wy, float wz, float* vx, float* vy, float* vz)
{
    /* The matrix the world was drawn with: the port's own cameras (TPS, OTS,
     * FPS) set it, while vcGetNowCamPos keeps reporting the game's camera. */
    const MATRIX* m = &GsWSMATRIX;

    *vx = ((m->m[0][0] * wx + m->m[0][1] * wy + m->m[0][2] * wz) / 16.0f + m->t[0]) / 256.0f;
    *vy = ((m->m[1][0] * wx + m->m[1][1] * wy + m->m[1][2] * wz) / 16.0f + m->t[1]) / 256.0f;
    *vz = ((m->m[2][0] * wx + m->m[2][1] * wy + m->m[2][2] * wz) / 16.0f + m->t[2]) / 256.0f;
}

/* World (m, game axes) -> HUD units. Returns 0 behind the camera. *outDepth is
 * the view depth in metres, for sizing. */
static int Ah_Project(float wx, float wy, float wz, float* hx, float* hy, float* outDepth)
{
    float vx, vy, vz;

    Ah_View(wx, wy, wz, &vx, &vy, &vz);
    if (vz < 0.3f)
        return 0;
    *hx = s_cx + (vx * s_camH / vz) * s_kx;
    *hy = s_cy + (vy * s_camH / vz) * s_ky;
    if (outDepth)
        *outDepth = vz;
    return 1;
}

/* ------------------------------------------------------------------ */
/* HUD elements                                                        */
/* ------------------------------------------------------------------ */

static float s_main[4], s_dim[4], s_hi[4];
static float s_th; /* standard stroke */

static void Ah_UseMain(void) { memcpy(s_col, s_main, sizeof(s_col)); }
static void Ah_UseDim(void)  { memcpy(s_col, s_dim,  sizeof(s_col)); }
static void Ah_UseHi(void)   { memcpy(s_col, s_hi,   sizeof(s_col)); }

static const char* Ah_EnemyName(int id)
{
    switch (id)
    {
        case Chara_AirScreamer:     return "AIR SCREAMER";
        case Chara_NightFlutter:    return "NIGHT FLUTTER";
        case Chara_Groaner:         return "GROANER";
        case Chara_Wormhead:        return "WORMHEAD";
        case Chara_LarvalStalker:   return "LARVAL STALKER";
        case Chara_Stalker:         return "STALKER";
        case Chara_GreyChild:       return "GREY CHILD";
        case Chara_Mumbler:         return "MUMBLER";
        case Chara_HangedScratcher: return "HANGED SCRATCHER";
        case Chara_Creeper:         return "CREEPER";
        case Chara_Romper:          return "ROMPER";
        case Chara_SplitHead:       return "SPLIT HEAD";
        case Chara_Floatstinger:    return "FLOATSTINGER";
        case Chara_PuppetNurse:     return "PUPPET NURSE";
        case Chara_PuppetDoctor:    return "PUPPET DOCTOR";
        case Chara_Twinfeeler:      return "TWINFEELER";
        case Chara_Bloodsucker:     return "BLOODSUCKER";
        case Chara_Incubus:         return "INCUBUS";
        case Chara_MonsterCybil:    return "CYBIL";
        default:                    return "BOGEY";
    }
}

/* flight_hud_callsigns: 0 the monster's name, 1 BANDIT once it is tracking
 * Harry and BOGEY until then, 2 TGT-nn by NPC slot. */
static const char* Ah_TargetName(int slot)
{
    static char num[NPC_COUNT_MAX][8];

    switch (g_PcConfig.flightHudCallsigns)
    {
        case 1:
            return s_lockState[slot] != 0 ? "BANDIT" : "BOGEY";
        case 2:
            snprintf(num[slot], sizeof(num[slot]), "TGT-%02d", slot + 1);
            return num[slot];
        default:
            return Ah_EnemyName(g_SysWork.npcs[slot].model.charaId);
    }
}

static const char* Ah_WeaponName(int id)
{
    switch (id)
    {
        case InvItemId_KitchenKnife: return "KNIFE";
        case InvItemId_SteelPipe:    return "STEEL PIPE";
        case InvItemId_RockDrill:    return "ROCK DRILL";
        case InvItemId_Hammer:       return "HAMMER";
        case InvItemId_Chainsaw:     return "CHAINSAW";
        case InvItemId_Katana:       return "KATANA";
        case InvItemId_Axe:          return "AXE";
        case InvItemId_Handgun:      return "HANDGUN";
        case InvItemId_HuntingRifle: return "RIFLE";
        case InvItemId_Shotgun:      return "SHOTGUN";
        case InvItemId_HyperBlaster: return "HYPER BLASTER";
        default:                     return "UNARMED";
    }
}

/* Box with one end drawn as a point (dir -1 left, +1 right), the shape used
 * for the speed and altitude readouts. */
static void Ah_PointerBox(float cx, float cy, float w, float h, int dir)
{
    const float hw = w * 0.5f, hh = h * 0.5f, tip = hh * 0.8f;
    float l = cx - hw, r = cx + hw, t = cy - hh, b = cy + hh;

    if (dir > 0)
    {
        Ah_Line(l, t, r, t, s_th);
        Ah_Line(r, t, r + tip, cy, s_th);
        Ah_Line(r + tip, cy, r, b, s_th);
        Ah_Line(r, b, l, b, s_th);
        Ah_Line(l, b, l, t, s_th);
    }
    else
    {
        Ah_Line(r, t, l, t, s_th);
        Ah_Line(l, t, l - tip, cy, s_th);
        Ah_Line(l - tip, cy, l, b, s_th);
        Ah_Line(l, b, r, b, s_th);
        Ah_Line(r, b, r, t, s_th);
    }
}

static void Ah_Readout(float cx, const char* title, const char* value, int dir)
{
    Ah_UseMain();
    Ah_Text(title, cx, -24.0f, 8.0f, 1);
    Ah_PointerBox(cx, -3.0f, 46.0f, 15.0f, dir);
    Ah_Text(value, cx, -7.5f, 9.0f, 1);
}

static void Ah_GunReticle(void)
{
    Ah_UseMain();
    Ah_Circle(0.0f, 0.0f, 13.0f, s_th, 28);
    Ah_Line(0.0f, -13.0f, 0.0f, -18.0f, s_th);
    Ah_Line(0.0f, 13.0f, 0.0f, 18.0f, s_th);
    Ah_Line(-13.0f, 0.0f, -18.0f, 0.0f, s_th);
    Ah_Line(13.0f, 0.0f, 18.0f, 0.0f, s_th);
    Ah_Rect(-1.2f, -1.2f, 1.2f, 1.2f);
}

/* Remaining health against the most this slot has been seen with: the maximum
 * differs by monster and difficulty, and the game keeps no copy of it. */
static void Ah_TargetHpBar(int slot, float cx, float top, float w)
{
    float f = (s_hpMax[slot] > 0.0f) ? Ah_Q12f(g_SysWork.npcs[slot].health) / s_hpMax[slot] : 1.0f;

    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    Ah_UseDim();
    Ah_Box(cx - w * 0.5f, top, cx + w * 0.5f, top + 5.0f, 1.0f);
    Ah_UseMain();
    Ah_Rect(cx - w * 0.5f + 1.5f, top + 1.5f, cx - w * 0.5f + 1.5f + (w - 3.0f) * f, top + 3.5f);
}

/* Draws every live enemy's container and returns the index of the nearest one
 * (the "TGT"), or -1. *outShoot is set when that target sits under the reticle
 * while Harry is aiming. */
/* Harry's seeker: a diamond that starts wide and closes onto the target box
 * (inner is that box's half size), then turns red and reads LOCK ON. */
static void Ah_SeekerMark(float x, float y, float inner, float nowS)
{
    float f = s_seekT / AH_SEEK_TIME, d;

    if (f > 1.0f) f = 1.0f;
    d = 46.0f + (inner + 6.0f - 46.0f) * f;
    if (f >= 1.0f)
        Ah_Color(1.0f, 0.25f, 0.2f, s_main[3]);
    else
        Ah_UseMain();
    Ah_Line(x, y - d, x + d, y, s_th);
    Ah_Line(x + d, y, x, y + d, s_th);
    Ah_Line(x, y + d, x - d, y, s_th);
    Ah_Line(x - d, y, x, y - d, s_th);
    if (f >= 1.0f && fmodf(nowS, 0.4f) < 0.28f)
        Ah_Text("LOCK ON", x, y - d - 20.0f, 7.0f, 1); /* clear of Classic's name over the box */
}

static void Ah_HitFx(void)
{
    int k;

    Ah_UseHi();
    for (k = 0; k < AH_HITFX_MAX; k++)
    {
        const s_AhHitFx* h = &s_hitFx[k];
        float hx, hy;

        if (h->life <= 0.0f || !Ah_Project(h->x, h->y, h->z, &hx, &hy, NULL))
            continue;
        Ah_Text("HIT", hx, hy - 46.0f - 12.0f * (1.0f - h->life / AH_HITFX_TIME), 8.0f, 1);
    }
    if (s_missT > 0.0f)
    {
        Ah_UseMain();
        Ah_Text("MISS", 0.0f, -34.0f, 9.0f, 1);
    }
}

/* Torn scanlines across the picture, k from 0 (none) to 1. */
static void Ah_Static(float k)
{
    const int n = 4 + (int)(14.0f * k);
    int       i;

    s_cur = &s_fill;
    for (i = 0; i < n; i++)
    {
        const float y = (Ah_Rand() * 2.0f - 1.0f) * 240.0f;
        const float x = (Ah_Rand() * 2.0f - 1.0f) * s_w2;
        const float w = (0.2f + Ah_Rand() * 0.8f) * s_w2;
        Ah_Color(s_main[0], s_main[1], s_main[2], (0.12f + 0.25f * Ah_Rand()) * k * s_main[3]);
        Ah_Rect(x - w, y, x + w, y + 1.0f + 2.0f * Ah_Rand());
    }
    s_cur = &s_hud;
}

/* Hit: the HUD shakes and tears for a moment. Low health: a slow red warning. */
static void Ah_Warnings(float nowS)
{
    const float hp = Ah_Q12f(g_SysWork.playerWork.player.health);

    if (s_dead)
        Ah_Static(s_deadT < AH_DEAD_FAIL ? s_deadT / AH_DEAD_FAIL : 1.0f);
    else if (s_hurtT > 0.0f)
    {
        Ah_Static(s_hurtAmt * s_hurtT / AH_HURT_TIME);
        if (fmodf(nowS, 0.16f) < 0.1f)
        {
            Ah_UseHi();
            Ah_Text("DAMAGE", 0.0f, 88.0f, 10.0f, 1);
        }
    }
    if (hp > 0.0f && hp < AH_LOW_HEALTH && fmodf(nowS, 1.0f) < 0.6f)
    {
        Ah_Color(1.0f, 0.25f, 0.2f, s_main[3]);
        Ah_Text("WARNING: LOW HEALTH", 0.0f, 150.0f, 9.0f, 1);
    }
}

static void Ah_RadioBox(float top);

/* Harry is down: the HUD gives way to the failure call and Cybil on the
 * radio, until the game's own GAME OVER takes the screen. */
static void Ah_BuildDead(float nowS)
{
    const int   touch = Pc_Touch_IsDrivingInput();
    const float w     = Ah_TextWidth("MISSION FAILED", 22.0f) * 0.5f + 16.0f;

    s_th = 1.4f;
    Ah_Static(1.0f);
    Ah_UseMain();
    Ah_Line(-w, -35.0f, w, -35.0f, s_th);
    Ah_Line(-w, 9.0f, w, 9.0f, s_th);
    Ah_Text("MISSION FAILED", 0.0f, -28.0f, 22.0f, 1);
    if (fmodf(nowS, 0.5f) < 0.3f)
    {
        Ah_UseHi();
        Ah_Text("SIGNAL LOST", 0.0f, 18.0f, 9.0f, 1);
    }
    Ah_RadioBox(AH_TOUCH_QUICKSAVE() ? -172.0f : -232.0f);
}

static int Ah_Remaining(void)
{
    int i, n = 0;
    for (i = 0; i < NPC_COUNT_MAX; i++)
        if (Ah_NpcLive(&g_SysWork.npcs[i]))
            n++;
    return n;
}

static int Ah_Targets(float nowS, int* outShoot)
{
    const s_SubCharacter* pl = &g_SysWork.playerWork.player;
    float hx[NPC_COUNT_MAX], hy[NPC_COUNT_MAX], dist[NPC_COUNT_MAX];
    int   vis[NPC_COUNT_MAX];
    int   i, best = -1;

    *outShoot = 0;

    for (i = 0; i < NPC_COUNT_MAX; i++)
    {
        const s_SubCharacter* npc = &g_SysWork.npcs[i];
        float wx, wy, wz, dx, dz, depth;

        vis[i] = 0;
        if (!Ah_NpcLive(npc))
            continue;

        wx = Ah_Q12f(npc->position.vx + npc->collision.shapeOffsets.box.vx);
        wz = Ah_Q12f(npc->position.vz + npc->collision.shapeOffsets.box.vz);
        wy = Ah_Q12f(npc->position.vy + npc->collision.box.offsetY);
        dx = wx - Ah_Q12f(pl->position.vx);
        dz = wz - Ah_Q12f(pl->position.vz);
        dist[i] = sqrtf(dx * dx + dz * dz);
        if (dist[i] > AH_TARGET_RANGE)
            continue;
        if (!Ah_Project(wx, wy, wz, &hx[i], &hy[i], &depth))
            continue;
        if (hx[i] < -s_w2 - 40.0f || hx[i] > s_w2 + 40.0f || hy[i] < -280.0f || hy[i] > 280.0f)
            continue;

        vis[i] = 1;
        if (best < 0 || dist[i] < dist[best])
            best = i;
    }

    for (i = 0; i < NPC_COUNT_MAX; i++)
    {
        const float half = 7.0f;
        char  buf[16];
        float x, y;

        if (!vis[i])
            continue;
        x = hx[i];
        y = hy[i];

        if (s_lockState[i] != 0 && fmodf(nowS, 0.3f) < 0.15f)
            Ah_UseHi();
        else
            Ah_UseMain();

        Ah_Box(x - half, y - half, x + half, y + half, s_th);

        if (i == best)
        {
            const float d = half + 5.0f;
            Ah_Line(x, y - d, x + d, y, s_th);
            Ah_Line(x + d, y, x, y + d, s_th);
            Ah_Line(x, y + d, x - d, y, s_th);
            Ah_Line(x - d, y, x, y - d, s_th);
        }
        if (i == s_seekSlot)
        {
            Ah_SeekerMark(x, y, half + 5.0f, nowS);
            Ah_UseMain();
        }

        Ah_Text(s_lockState[i] == 2 ? "LOCK" : "TGT", x - half - 4.0f, y - half - 8.0f, 6.0f, 2);
        snprintf(buf, sizeof(buf), "%d", (int)(dist[i] + 0.5f));
        Ah_Text(buf, x + half + 5.0f, y - half, 6.0f, 0);
        if (i == best)
        {
            Ah_Text(Ah_TargetName(i), x + half + 5.0f, y - half + 8.0f, 6.0f, 0);
            Ah_TargetHpBar(i, x, y + half + 8.0f, 26.0f);
        }

        if (i == best && g_SysWork.playerCombat.isAiming && dist[i] < 20.0f &&
            fabsf(x) < 60.0f && fabsf(y) < 60.0f)
            *outShoot = 1;
    }

    return best;
}

/* Radar: a dark square window, range rings, heading-up, cardinal letters
 * riding the outer ring. */
static void Ah_Radar(float l, float t, float w, float h, float yawTurns, float nowS)
{
    static const float enemy[4] = { 1.0f, 0.3f, 0.25f, 0.95f };
    static const char* const card[4] = { "N", "E", "S", "W" };
    const s_SubCharacter* pl = &g_SysWork.playerWork.player;
    const float cx = l + w * 0.5f, cy = t + h * 0.55f, r = h * 0.42f;
    float yaw = yawTurns * 2.0f * AH_PI;
    float fx = sinf(yaw), fz = cosf(yaw);
    float rx = cosf(yaw), rz = -sinf(yaw);
    float px = Ah_Q12f(pl->position.vx), pz = Ah_Q12f(pl->position.vz);
    int   i;

    s_cur = &s_fill;
    Ah_Color(0.0f, 0.18f, 0.05f, 0.45f * s_main[3]);
    Ah_Rect(l, t, l + w, t + h);
    s_cur = &s_hud;

    Ah_UseMain();
    Ah_Box(l, t, l + w, t + h, s_th);
    Ah_UseDim();
    Ah_Circle(cx, cy, r, s_th, 40);
    Ah_Circle(cx, cy, r * 0.5f, s_th * 0.8f, 28);
    Ah_Line(cx - r, cy, cx + r, cy, s_th * 0.7f);
    Ah_Line(cx, cy - r, cx, cy + r, s_th * 0.7f);

    Ah_UseMain();
    for (i = 0; i < 4; i++)
    {
        float a = (i * 0.25f - yawTurns) * 2.0f * AH_PI;
        float lx = cx + sinf(a) * (r - 8.0f), ly = cy - cosf(a) * (r - 8.0f);
        if (lx < l + 4.0f || lx > l + w - 4.0f || ly < t + 4.0f || ly > t + h - 4.0f)
            continue;
        Ah_Text(card[i], lx, ly - 3.5f, 7.0f, 1);
    }

    Ah_Tri(cx, cy - 6.0f, cx - 4.5f, cy + 4.0f, cx + 4.5f, cy + 4.0f);

    for (i = 0; i < NPC_COUNT_MAX; i++)
    {
        const s_SubCharacter* npc = &g_SysWork.npcs[i];
        float dx, dz, lx, ly, len, bx, by;
        if (!Ah_NpcLive(npc))
            continue;

        dx  = Ah_Q12f(npc->position.vx) - px;
        dz  = Ah_Q12f(npc->position.vz) - pz;
        lx  = dx * rx + dz * rz;
        ly  = dx * fx + dz * fz;
        len = sqrtf(lx * lx + ly * ly);
        if (len > AH_RADAR_RANGE)
        {
            lx *= AH_RADAR_RANGE / len;
            ly *= AH_RADAR_RANGE / len;
        }
        bx = cx + lx / AH_RADAR_RANGE * r;
        by = cy - ly / AH_RADAR_RANGE * r;
        if (bx < l + 3.0f || bx > l + w - 3.0f || by < t + 3.0f || by > t + h - 3.0f)
            continue;

        if (s_lockState[i] != 0 && fmodf(nowS, 0.3f) < 0.15f)
            Ah_UseHi();
        else
            memcpy(s_col, enemy, sizeof(s_col));
        Ah_Tri(bx, by - 3.5f, bx - 3.0f, by + 2.5f, bx + 3.0f, by + 2.5f);
    }
}

static int Ah_Kills(void)
{
    return (int)g_SavegamePtr->meleeKillCount + (int)g_SavegamePtr->rangedKillCount;
}

static void Ah_WeaponRow(char* name, char* value, size_t n)
{
    int w = g_SavegamePtr->equippedWeapon;

    snprintf(name, n, "%s", Ah_WeaponName(w));
    if (w == InvItemId_HyperBlaster)
        snprintf(value, n, "INF");
    else if (w >= InvItemId_Handgun && w <= InvItemId_Shotgun)
        snprintf(value, n, "%d/%d", g_SysWork.playerCombat.currentWeaponAmmo,
                 g_SysWork.playerCombat.totalWeaponAmmo);
    else
        snprintf(value, n, "---");
}

/* Green, yellow, orange, red as health drops: the damage colours of a
 * jet HUD's aircraft silhouette. Keeps its own colour under a lock, or the alert would
 * hide how hurt Harry is. */
static void Ah_HealthColor(float hp, float nowS)
{
    const float a = (float)g_PcConfig.flightHudOpacity / 100.0f;

    if (hp >= 75.0f)      Ah_Color(0.45f, 1.0f, 0.55f, 0.95f * a);
    else if (hp >= 50.0f) Ah_Color(1.0f, 0.92f, 0.25f, 0.95f * a);
    else if (hp >= 25.0f) Ah_Color(1.0f, 0.55f, 0.12f, 0.95f * a);
    else                  Ah_Color(1.0f, 0.18f, 0.12f, (fmodf(nowS, 0.6f) < 0.4f ? 0.95f : 0.45f) * a);
}

/* Harry in place of the aircraft: head, torso, arms, legs. (cx, top), h tall. */
static int Ah_WireHarry(float cx, float top, float h);

static void Ah_Silhouette(float cx, float top, float h)
{
    const float k = h / 60.0f;
    const float limb = 4.5f * k, leg = 5.5f * k;
    int i;

    if (Ah_WireHarry(cx, top, h))
        return;

    for (i = 0; i < 8; i++)
    {
        float a0 = i / 8.0f * 2.0f * AH_PI, a1 = (i + 1) / 8.0f * 2.0f * AH_PI;
        Ah_Tri(cx, top + 6.0f * k,
               cx + cosf(a0) * 5.5f * k, top + 6.0f * k + sinf(a0) * 5.5f * k,
               cx + cosf(a1) * 5.5f * k, top + 6.0f * k + sinf(a1) * 5.5f * k);
    }
    Ah_Quad(cx - 10.0f * k, top + 13.5f * k, cx + 10.0f * k, top + 13.5f * k,
            cx + 7.0f * k, top + 33.0f * k, cx - 7.0f * k, top + 33.0f * k);
    Ah_Line(cx - 11.0f * k, top + 15.5f * k, cx - 15.5f * k, top + 33.0f * k, limb);
    Ah_Line(cx + 11.0f * k, top + 15.5f * k, cx + 15.5f * k, top + 33.0f * k, limb);
    Ah_Line(cx - 3.8f * k, top + 34.0f * k, cx - 6.0f * k, top + 57.0f * k, leg);
    Ah_Line(cx + 3.8f * k, top + 34.0f * k, cx + 6.0f * k, top + 57.0f * k, leg);
}

static void Ah_OffscreenArrows(float nowS)
{
    const s_SubCharacter* pl = &g_SysWork.playerWork.player;
    const float ex = s_w2 - 20.0f, ey = 240.0f - 20.0f;
    int i;

    for (i = 0; i < NPC_COUNT_MAX; i++)
    {
        const s_SubCharacter* npc = &g_SysWork.npcs[i];
        float wx, wy, wz, dx, dz, dist, vx, vy, vz, len, ux, uy, t, px, py;
        char  buf[8];

        if (!Ah_NpcLive(npc))
            continue;

        wx = Ah_Q12f(npc->position.vx + npc->collision.shapeOffsets.box.vx);
        wz = Ah_Q12f(npc->position.vz + npc->collision.shapeOffsets.box.vz);
        wy = Ah_Q12f(npc->position.vy + npc->collision.box.offsetY);
        dx = wx - Ah_Q12f(pl->position.vx);
        dz = wz - Ah_Q12f(pl->position.vz);
        dist = sqrtf(dx * dx + dz * dz);
        if (dist > AH_TARGET_RANGE)
            continue;

        Ah_View(wx, wy, wz, &vx, &vy, &vz);
        if (vz >= 0.3f)
        {
            dx = s_cx + (vx * s_camH / vz) * s_kx;
            dz = s_cy + (vy * s_camH / vz) * s_ky;
            if (fabsf(dx) <= s_w2 - 8.0f && fabsf(dz) <= 232.0f)
                continue;
        }
        else
        {
            /* Behind the camera the perspective divide flips the side. */
            dx = vx * s_camH * s_kx;
            dz = vy * s_camH * s_ky;
        }

        len = sqrtf(dx * dx + dz * dz);
        if (len < 0.001f)
        {
            dx  = 0.0f;
            dz  = 1.0f;
            len = 1.0f;
        }
        ux = dx / len;
        uy = dz / len;
        t  = 1e9f;
        if (fabsf(ux) > 0.0001f && ex / fabsf(ux) < t) t = ex / fabsf(ux);
        if (fabsf(uy) > 0.0001f && ey / fabsf(uy) < t) t = ey / fabsf(uy);
        px = ux * t;
        py = uy * t;

        if (s_lockState[i] != 0 && fmodf(nowS, 0.3f) < 0.15f)
            Ah_UseHi();
        else
            Ah_UseMain();
        Ah_Tri(px + ux * 7.0f, py + uy * 7.0f,
               px - ux * 5.0f - uy * 6.0f, py - uy * 5.0f + ux * 6.0f,
               px - ux * 5.0f + uy * 6.0f, py - uy * 5.0f - ux * 6.0f);
        snprintf(buf, sizeof(buf), "%d", (int)(dist + 0.5f));
        Ah_Text(buf, px - ux * 20.0f, py - uy * 20.0f - 3.25f, 6.5f, 1);
    }
}

/* "DESTROYED" over each fresh wreck, and a "+1000" flying to (scoreX, scoreY). */
static void Ah_KillFx(float scoreX, float scoreY, float nowS)
{
    int k;

    for (k = 0; k < AH_KILL_MAX; k++)
    {
        const s_AhKill* kk = &s_kills[k];
        float hx = 0.0f, hy = 0.0f, f;
        int   seen;

        if (kk->age >= AH_KILL_TIME)
            continue;

        seen = Ah_Project(kk->x, kk->y, kk->z, &hx, &hy, NULL);
        Ah_UseHi();
        if (seen && (kk->age > 0.35f || fmodf(nowS, 0.12f) < 0.08f))
            Ah_Text("DESTROYED", hx, hy - 22.0f, 9.0f, 1);

        if (kk->age < AH_SCORE_FLY)
        {
            f = kk->age / AH_SCORE_FLY;
            f = f * f;
            Ah_Text("+1000", hx + (scoreX - hx) * f, (hy - 36.0f) + (scoreY - (hy - 36.0f)) * f, 8.0f, 1);
        }
    }
}

/* Live portrait state; the capture and its GL live with the draw below. */
static GLuint s_portTex[Chara_Count];
static float  s_portQual[Chara_Count];   /* crop radius in pixels at capture */
static Uint32 s_portTakenMs[Chara_Count];
static Uint32 s_portSavedMs[Chara_Count];
static int    s_portLoaded;
static GLuint s_portFbo;
static int    s_portLive = -1;           /* charaId whose feed the radio shows */

static GLuint s_texProg;
static GLint  s_texLocTex, s_texLocTint, s_texLocMono, s_texLocTime;

static struct
{
    int   on, chara, monster;
    float l, t, size, age;
} s_portDraw;

/* The 3D comm portrait asked for by the radio box (see Ah_Portrait3dPass). */
static int   s_p3dKey = -1, s_p3dMonster;

static int Ah_PortraitKey(int charaId)
{
    if (charaId == Chara_EndingCybil)
        return Chara_Cybil;
    if (charaId == Chara_Cybil || Ah_IsEnemy(charaId))
        return charaId;
    return -1;
}

/* Fallback comm portrait, until the game has shown the speaker's face (see
 * Ah_PortraitCapture): a wireframe bust built from a deformed ellipsoid,
 * turned and lit by depth. */
typedef struct
{
    float rx, ry, rz;  /* half extents */
    float snout, sharp; /* muzzle length, and how narrow it is (cos power) */
    float jaw;         /* jaw drop at full open */
    float ears;        /* ears / horns on the upper sides */
    int   hair, bust, glowEyes;
} s_AhFace;

static const s_AhFace s_faces[AH_FACE_COUNT] = {
    [AH_FACE_CYBIL] = { 0.78f, 1.00f, 0.86f, 0.10f, 2.0f, 0.14f, 0.00f, 1, 1, 0 },
    [AH_FACE_HUMAN] = { 0.78f, 1.00f, 0.86f, 0.10f, 2.0f, 0.30f, 0.00f, 0, 1, 1 },
    [AH_FACE_DOG]   = { 0.70f, 0.72f, 0.80f, 1.50f, 2.5f, 0.55f, 0.55f, 0, 0, 1 },
    [AH_FACE_BIRD]  = { 0.62f, 0.70f, 0.70f, 2.00f, 8.0f, 0.45f, 0.00f, 0, 0, 1 },
    [AH_FACE_CHILD] = { 0.80f, 0.92f, 0.82f, 0.00f, 2.0f, 0.40f, 0.00f, 0, 1, 1 },
    [AH_FACE_BEAST] = { 0.95f, 0.88f, 0.90f, 0.55f, 2.0f, 0.70f, 0.85f, 0, 0, 1 },
};

#define AH_FACE_RINGS 9
#define AH_FACE_SEGS  14
#define AH_MOUTH_TH   (0.62f * AH_PI)

static void Ah_FacePoint(const s_AhFace* f, float th, float ph, float open, float* x, float* y, float* z)
{
    const float front = cosf(ph) > 0.0f ? cosf(ph) : 0.0f;
    const float side  = fabsf(sinf(ph));
    float m, h, k;

    *x = f->rx * sinf(th) * sinf(ph);
    *y = -f->ry * cosf(th);
    *z = f->rz * sinf(th) * cosf(ph);

    m   = (th - AH_MOUTH_TH) / (0.22f * AH_PI);
    *z += f->snout * powf(front, f->sharp) * expf(-m * m);

    if (th > AH_MOUTH_TH)
    {
        k   = (th - AH_MOUTH_TH) / (AH_PI - AH_MOUTH_TH);
        *y += f->jaw * open * (0.4f + 0.6f * k) * front;
    }

    m  = (th - 0.22f * AH_PI) / (0.1f * AH_PI);
    h  = f->ears * powf(side, 8.0f) * expf(-m * m);
    *y -= h * 0.9f;
    *x += (sinf(ph) < 0.0f ? -h : h) * 0.3f;
}

static void Ah_Portrait(float l, float t, float size, int face, int chara, int monster, float age, float nowS)
{
    static const float red[4] = { 1.0f, 0.3f, 0.25f, 0.95f };
    const s_AhFace* f  = &s_faces[face];
    const float     cx = l + size * 0.5f, cy = t + size * 0.42f, sc = size * 0.3f;
    const float     yaw = 0.4f + 0.35f * sinf(nowS * 0.7f), pitch = 0.08f * sinf(nowS * 0.9f);
    const float     cyw = cosf(yaw), syw = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
    float base[4];
    float px[AH_FACE_RINGS + 1][AH_FACE_SEGS], py[AH_FACE_RINGS + 1][AH_FACE_SEGS], pz[AH_FACE_RINGS + 1][AH_FACE_SEGS];
    float open = 0.0f, y;
    int   j, k, n;

    memcpy(base, monster ? red : s_main, sizeof(base));
    base[3] = s_main[3];

    s_cur = &s_fill;
    Ah_Color(0.0f, 0.08f, 0.03f, 0.6f * s_main[3]);
    Ah_Rect(l, t, l + size, t + size);
    Ah_Color(base[0], base[1], base[2], 0.045f * s_main[3]);
    for (y = t + 2.0f; y < t + size; y += 4.0f)
        Ah_Rect(l, y, l + size, y + 0.8f);
    s_cur = &s_hud;

    Ah_UseMain();
    Ah_Box(l, t, l + size, t + size, s_th);

    if (monster)
        s_portLive = chara;
    if (chara >= 0 && g_PcConfig.flightHudPortrait3d)
    {
        s_p3dKey     = chara;
        s_p3dMonster = monster;
    }
    if (age >= 0.25f && chara >= 0 && s_portTex[chara])
    {
        s_portDraw.on      = 1;
        s_portDraw.chara   = chara;
        s_portDraw.monster = monster;
        s_portDraw.l       = l;
        s_portDraw.t       = t;
        s_portDraw.size    = size;
        s_portDraw.age     = age;
    }

    /* The channel opens on static; monsters never quite come through clean. */
    n = (age < 0.25f) ? 14 : (monster ? 3 : 0);
    for (k = 0; k < n; k++)
    {
        float ny = t + 2.0f + Ah_Rand() * (size - 4.0f), nx = l + 2.0f + Ah_Rand() * (size * 0.5f);
        Ah_Color(base[0], base[1], base[2], (0.15f + 0.35f * Ah_Rand()) * s_main[3]);
        Ah_Rect(nx, ny, nx + 6.0f + Ah_Rand() * size * 0.45f, ny + 1.0f);
    }
    if (age < 0.25f || s_portDraw.on)
        return;

    if (age < AH_RADIO_TIME * 0.7f)
        open = fabsf(sinf(nowS * 11.0f)) * (0.55f + 0.45f * sinf(nowS * 4.3f + 1.0f));

    for (j = 0; j <= AH_FACE_RINGS; j++)
    {
        for (k = 0; k < AH_FACE_SEGS; k++)
        {
            float x, yy, z, x1, z1;
            Ah_FacePoint(f, (float)j / AH_FACE_RINGS * AH_PI, (float)k / AH_FACE_SEGS * 2.0f * AH_PI, open, &x, &yy, &z);
            x1 = x * cyw + z * syw;
            z1 = -x * syw + z * cyw;
            px[j][k] = cx + x1 * sc;
            py[j][k] = cy + (yy * cp - z1 * sp) * sc;
            pz[j][k] = yy * sp + z1 * cp;
        }
    }

    for (j = 0; j <= AH_FACE_RINGS; j++)
    {
        for (k = 0; k < AH_FACE_SEGS; k++)
        {
            const int k2 = (k + 1) % AH_FACE_SEGS;
            float     d, a, th;

            th = (f->hair && j <= 3) ? 1.4f : 0.9f;
            if (j > 0 && j < AH_FACE_RINGS)
            {
                d = (pz[j][k] + pz[j][k2]) * 0.5f;
                a = 0.2f + 0.8f * (d + 0.9f) / 1.8f;
                if (a > 1.0f) a = 1.0f;
                if (a < 0.2f) a = 0.2f;
                Ah_Color(base[0], base[1], base[2], base[3] * a);
                Ah_Line(px[j][k], py[j][k], px[j][k2], py[j][k2], th);
            }
            if (j < AH_FACE_RINGS)
            {
                d = (pz[j][k] + pz[j + 1][k]) * 0.5f;
                a = 0.2f + 0.8f * (d + 0.9f) / 1.8f;
                if (a > 1.0f) a = 1.0f;
                if (a < 0.2f) a = 0.2f;
                Ah_Color(base[0], base[1], base[2], base[3] * a);
                Ah_Line(px[j][k], py[j][k], px[j + 1][k], py[j + 1][k], th);
            }
        }
    }

    /* Blink every few seconds; glowing eyes never do. */
    if (f->glowEyes || fmodf(nowS, 3.7f) > 0.12f)
    {
        for (k = -1; k <= 1; k += 2)
        {
            float x, yy, z, x1, z1, ex, ey, ez, r = f->glowEyes ? 1.6f : 1.0f;
            Ah_FacePoint(f, 0.47f * AH_PI, k * 0.38f, open, &x, &yy, &z);
            z += 0.04f;
            x1 = x * cyw + z * syw;
            z1 = -x * syw + z * cyw;
            ex = cx + x1 * sc;
            ey = cy + (yy * cp - z1 * sp) * sc;
            ez = yy * sp + z1 * cp;
            if (ez < 0.1f)
                continue;
            if (f->glowEyes)
                Ah_Color(1.0f, 0.85f, 0.4f, s_main[3]);
            else
                Ah_UseMain();
            Ah_Rect(ex - r, ey - r * 0.6f, ex + r, ey + r * 0.6f);
        }
    }

    if (f->bust)
    {
        const float neckY = cy + f->ry * sc * 0.9f, shY = t + size * 0.88f, bot = t + size - 1.5f;
        Ah_Color(base[0], base[1], base[2], base[3] * 0.8f);
        for (k = -1; k <= 1; k += 2)
        {
            Ah_Line(cx + k * 0.28f * sc, neckY, cx + k * 0.32f * sc, shY - 0.15f * sc, 1.0f);
            Ah_Line(cx + k * 0.32f * sc, shY - 0.15f * sc, cx + k * 1.15f * sc, shY, 1.0f);
            Ah_Line(cx + k * 1.15f * sc, shY, cx + k * 1.45f * sc, bot, 1.0f);
        }
    }
}

static void Ah_RadioBox(float top)
{
    static const float red[4] = { 1.0f, 0.3f, 0.25f, 0.95f };
    const s_AhRadioLine* m;
    const int  chara = s_radioChara[0];
    const char* who;
    char  buf[64];
    float w, h = 32.0f;

    if (s_radioN == 0)
        return;
    m   = &s_radioLines[s_radioQ[0]];
    who = m->who ? m->who : Ah_EnemyName(chara);
    snprintf(buf, sizeof(buf), "<< %s >>", m->line);
    w = Ah_TextWidth(buf, 8.0f) + 20.0f;

    s_cur = &s_fill;
    Ah_Color(0.0f, 0.12f, 0.04f, 0.5f * s_main[3]);
    Ah_Rect(-w * 0.5f, top, w * 0.5f, top + h);
    s_cur = &s_hud;

    Ah_UseMain();
    Ah_Box(-w * 0.5f, top, w * 0.5f, top + h, s_th);
    Ah_Text(buf, 0.0f, top + 18.0f, 8.0f, 1);
    if (m->who)
        Ah_UseHi();
    else
        Ah_Color(red[0], red[1], red[2], red[3] * s_main[3]);
    Ah_Text(who, -w * 0.5f + 8.0f, top + 5.0f, 7.0f, 0);

    Ah_Portrait(w * 0.5f + 4.0f, top, 72.0f, m->face,
                Ah_PortraitKey(m->who ? Chara_Cybil : chara), m->who == NULL, AH_RADIO_TIME - s_radioT,
                (float)SDL_GetTicks() / 1000.0f);
}

static void Ah_Banner(float cy)
{
    const char* zone;
    float age, open, w;

    if (s_bannerT <= 0.0f || s_bannerZone < 0)
        return;

    zone = s_zoneNames[s_bannerZone];
    age  = AH_BANNER_TIME - s_bannerT;
    open = age / AH_BANNER_EDGE;
    if (s_bannerT / AH_BANNER_EDGE < open)
        open = s_bannerT / AH_BANNER_EDGE;
    if (open > 1.0f) open = 1.0f;
    if (open < 0.0f) open = 0.0f;

    w = Ah_TextWidth(zone, 11.0f);
    if (w < Ah_TextWidth("MISSION UPDATE", 9.0f))
        w = Ah_TextWidth("MISSION UPDATE", 9.0f);
    w = (w * 0.5f + 16.0f) * open;

    Ah_UseMain();
    Ah_Line(-w, cy - 20.0f, w, cy - 20.0f, s_th);
    Ah_Line(-w, cy + 20.0f, w, cy + 20.0f, s_th);
    if (open >= 1.0f)
    {
        Ah_UseHi();
        Ah_Text("MISSION UPDATE", 0.0f, cy - 15.0f, 9.0f, 1);
        Ah_UseMain();
        Ah_Text(zone, 0.0f, cy + 2.0f, 11.0f, 1);
    }
}

static void Ah_DebriefPanel(void)
{
    const float l = -125.0f, r = 125.0f, top = -128.0f, bot = -8.0f;
    const s_AhDebrief* d = &s_debrief;
    Uint32 t = (Uint32)(d->time < 0.0f ? 0.0f : d->time);
    char   buf[32];
    int    row;
    static const char* const label[5] = { "TIME", "SCORE", "KILLS", "SHOTS / HITS", "ACCURACY" };

    if (s_debriefT <= 0.0f)
        return;

    s_cur = &s_fill;
    Ah_Color(0.0f, 0.1f, 0.03f, 0.6f * s_main[3]);
    Ah_Rect(l, top, r, bot);
    s_cur = &s_hud;

    Ah_UseMain();
    Ah_Box(l, top, r, bot, s_th);
    Ah_Text("MISSION COMPLETE", 0.0f, top + 8.0f, 12.0f, 1);
    Ah_Line(l + 8.0f, top + 26.0f, r - 8.0f, top + 26.0f, s_th);

    for (row = 0; row < 5; row++)
    {
        const float y = top + 34.0f + row * 15.0f;
        switch (row)
        {
            case 0: snprintf(buf, sizeof(buf), "%02u:%02u:%02u", t / 3600u, (t / 60u) % 60u, t % 60u); break;
            case 1: snprintf(buf, sizeof(buf), "%d", d->kills * 1000); break;
            case 2: snprintf(buf, sizeof(buf), "%d", d->kills); break;
            case 3: snprintf(buf, sizeof(buf), "%d / %d", d->fired, d->hits); break;
            default:
                if (d->fired > 0)
                    snprintf(buf, sizeof(buf), "%d%%", (int)(100.0f * d->hits / d->fired + 0.5f));
                else
                    snprintf(buf, sizeof(buf), "---");
                break;
        }
        Ah_UseDim();
        Ah_Text(label[row], l + 12.0f, y, 8.0f, 0);
        Ah_UseMain();
        Ah_Text(buf, r - 62.0f, y, 8.0f, 2);
    }

    Ah_UseMain();
    Ah_Text("RANK", r - 30.0f, top + 34.0f, 8.0f, 1);
    buf[0] = d->rank;
    buf[1] = '\0';
    Ah_UseHi();
    Ah_Text(buf, r - 30.0f, top + 50.0f, 32.0f, 1);
}

static void Ah_GradQuad(float ox0, float oy0, float ox1, float oy1, float ix1, float iy1, float ix0, float iy0,
                        const float* outer, const float* inner)
{
    if (s_cur->n + 6 > s_cur->cap)
        return;
    Ah_V(ox0, oy0, outer);
    Ah_V(ox1, oy1, outer);
    Ah_V(ix1, iy1, inner);
    Ah_V(ox0, oy0, outer);
    Ah_V(ix1, iy1, inner);
    Ah_V(ix0, iy0, inner);
}

/* Third alarm level: a red pulse along the screen edges. Left out while the
 * low-health glow is pulsing, so the two reds never stack. */
static void Ah_DangerEdge(float nowS)
{
    const float hp = Ah_Q12f(g_SysWork.playerWork.player.health);
    const float W = s_w2, H = 240.0f, t = 30.0f;
    float o[4], in[4];

    if (!s_danger || hp <= 0.0f)
        return;
    if (g_PcConfig.lowHealthGlow && hp < 20.0f)
        return;

    o[0] = 1.0f; o[1] = 0.08f; o[2] = 0.04f;
    o[3] = (0.25f + 0.75f * (0.5f + 0.5f * sinf(nowS * 2.0f * AH_PI * 3.0f))) * 0.6f *
           (float)g_PcConfig.flightHudOpacity / 100.0f;
    memcpy(in, o, sizeof(in));
    in[3] = 0.0f;

    s_cur = &s_fill;
    Ah_GradQuad(-W, -H, W, -H, W - t, -H + t, -W + t, -H + t, o, in);
    Ah_GradQuad(W, -H, W, H, W - t, H - t, W - t, -H + t, o, in);
    Ah_GradQuad(W, H, -W, H, -W + t, H - t, W - t, H - t, o, in);
    Ah_GradQuad(-W, H, -W, -H, -W + t, -H + t, -W + t, H - t, o, in);
    s_cur = &s_hud;
}

/* The event layer both styles share. radioTop and bannerY place the two boxes
 * clear of each style's own furniture. */
static void Ah_Events(float scoreX, float scoreY, float radioTop, float bannerY, float nowS)
{
    Ah_DangerEdge(nowS);
    Ah_Warnings(nowS);
    Ah_OffscreenArrows(nowS);
    Ah_KillFx(scoreX, scoreY, nowS);
    Ah_HitFx();
    Ah_RadioBox(radioTop);
    Ah_Banner(bannerY);
    Ah_DebriefPanel();
}

/* Flare recharge: an outlined track so it reads even when it has barely
 * started filling. Nothing while the rack is full. */
static void Ah_RechargeBar(float l, float t, float w, float h)
{
    float f = s_rechargeT / AH_FLARE_RECHARGE;

    if (s_flareStock >= AH_FLARE_MAX)
        return;
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    Ah_UseDim();
    Ah_Box(l, t, l + w, t + h, 0.8f);
    Ah_UseMain();
    if (f > 0.0f)
        Ah_Rect(l + 1.0f, t + 1.0f, l + 1.0f + (w - 2.0f) * f, t + h - 1.0f);
}

/* ------------------------------------------------------------------ */
/* First person: pitch ladder and compass tape                         */
/* ------------------------------------------------------------------ */

extern int g_PcFpsCam;
extern int Pc_ScriptOwnsScene(void);

/* The fixed cameras have no pitch to read, so the ladder only exists when the
 * picture is Harry's own eyes. */
static int Ah_FirstPerson(void)
{
    return g_PcFpsCam && !Pc_ScriptOwnsScene();
}

/* Heading and pitch of the matrix the world was drawn with, so mouse look and
 * the head bob both move the ladder exactly as they move the picture. */
static void Ah_ViewAngles(float* headingDeg, float* pitchDeg)
{
    const MATRIX* m  = &GsWSMATRIX;
    const float   fx = (float)m->m[2][0], fy = (float)m->m[2][1], fz = (float)m->m[2][2];
    float         h  = atan2f(fx, fz) * (180.0f / AH_PI);

    if (h < 0.0f)
        h += 360.0f;
    *headingDeg = h;
    *pitchDeg   = atan2f(-fy, sqrtf(fx * fx + fz * fz)) * (180.0f / AH_PI);
}

/* A world direction (game axes, Y down) to its vanishing point in HUD units. */
static int Ah_ProjectDir(float dx, float dy, float dz, float* hx, float* hy)
{
    const MATRIX* m = &GsWSMATRIX;
    const float   vx = m->m[0][0] * dx + m->m[0][1] * dy + m->m[0][2] * dz;
    const float   vy = m->m[1][0] * dx + m->m[1][1] * dy + m->m[1][2] * dz;
    const float   vz = m->m[2][0] * dx + m->m[2][1] * dy + m->m[2][2] * dz;

    if (vz < 0.05f * 4096.0f)
        return 0;
    *hx = s_cx + (vx * s_camH / vz) * s_kx;
    *hy = s_cy + (vy * s_camH / vz) * s_ky;
    return 1;
}

static void Ah_PitchDir(float hRad, float pDeg, float* dx, float* dy, float* dz)
{
    const float p = pDeg * (AH_PI / 180.0f);
    *dx = sinf(hRad) * cosf(p);
    *dy = -sinf(p);
    *dz = cosf(hRad) * cosf(p);
}

static void Ah_PitchLadder(float headingDeg, float pitchDeg)
{
    const float hRad = headingDeg * (AH_PI / 180.0f);
    const float rX = cosf(hRad), rZ = -sinf(hRad);
    const float gap = 24.0f, len = 38.0f, tick = 6.0f;
    const float winX = 190.0f, winY = 135.0f;
    char        buf[12];
    int         p;
    int         lo = (int)floorf((pitchDeg - 30.0f) / 5.0f) * 5;
    int         hi = (int)ceilf((pitchDeg + 30.0f) / 5.0f) * 5;

    if (lo < -85) lo = -85;
    if (hi >  85) hi =  85;

    for (p = lo; p <= hi; p += 5)
    {
        float dx, dy, dz, cx, cy, ax, ay, bx, by, tx, ty, nx, ny, tl, ext, side;
        int   s;

        Ah_PitchDir(hRad, (float)p, &dx, &dy, &dz);
        if (!Ah_ProjectDir(dx, dy, dz, &cx, &cy))
            continue;
        if (fabsf(cx) > winX || fabsf(cy) > winY)
            continue;

        /* The rung's slant on screen comes from projecting a sideways nudge,
         * so it tilts with any roll in the view instead of assuming none. */
        if (!Ah_ProjectDir(dx + rX * 0.05f, dy, dz + rZ * 0.05f, &ax, &ay) ||
            !Ah_ProjectDir(dx - rX * 0.05f, dy, dz - rZ * 0.05f, &bx, &by))
            continue;
        tx = ax - bx;
        ty = ay - by;
        tl = sqrtf(tx * tx + ty * ty);
        if (tl < 0.001f)
            continue;
        tx /= tl;
        ty /= tl;
        if (tx < 0.0f)
        {
            tx = -tx;
            ty = -ty;
        }
        nx = -ty;
        ny = tx;

        if (p == 0)
        {
            Ah_UseMain();
            ext = gap + len * 2.6f;
            Ah_Line(cx + tx * gap, cy + ty * gap, cx + tx * ext, cy + ty * ext, s_th);
            Ah_Line(cx - tx * gap, cy - ty * gap, cx - tx * ext, cy - ty * ext, s_th);
            continue;
        }

        Ah_UseMain();
        ext = gap + len;
        for (s = -1; s <= 1; s += 2)
        {
            /* Ticks point at the horizon: down above it, up below it. */
            const float tk = (p > 0) ? tick : -tick;
            const float ox = cx + tx * ext * s, oy = cy + ty * ext * s;

            side = (float)s;
            if (p > 0)
            {
                Ah_Line(cx + tx * gap * side, cy + ty * gap * side, ox, oy, s_th);
            }
            else
            {
                int k;
                for (k = 0; k < 3; k++)
                {
                    const float d0 = gap + len * (k / 3.0f), d1 = d0 + len * 0.2f;
                    Ah_Line(cx + tx * d0 * side, cy + ty * d0 * side,
                            cx + tx * d1 * side, cy + ty * d1 * side, s_th);
                }
            }
            Ah_Line(ox, oy, ox + nx * tk, oy + ny * tk, s_th);

            snprintf(buf, sizeof(buf), "%d", p < 0 ? -p : p);
            Ah_Text(buf, ox + tx * 5.0f * side, oy + ty * 5.0f * side - 3.5f, 7.0f, s < 0 ? 2 : 0);
        }
    }
}

static const char* Ah_CardinalOf(int deg)
{
    static const char* const names[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    return (deg % 45) == 0 ? names[(deg / 45) % 8] : NULL;
}

/* Ticks every 5 degrees, the eight compass points and the 15-degree marks
 * between them labelled, the exact heading boxed in the middle: at 199 the
 * band reads S, 199, SW. */
static void Ah_CompassTape(float headingDeg, float cy)
{
    const float half = 120.0f, ppd = 3.2f;
    char        buf[8];
    int         d;
    int         base = (int)floorf(headingDeg / 5.0f) * 5;

    for (d = base - 40; d <= base + 40; d += 5)
    {
        const float x  = (d - headingDeg) * ppd;
        const int   dn = ((d % 360) + 360) % 360;
        const char* lbl;

        if (x < -half || x > half)
            continue;

        Ah_UseDim();
        Ah_Line(x, cy, x, cy + ((dn % 15) == 0 ? 8.0f : 4.0f), s_th);
        if (fabsf(x) < 22.0f)
            continue;
        lbl = Ah_CardinalOf(dn);
        if (lbl)
        {
            Ah_UseMain();
            Ah_Text(lbl, x, cy + 11.0f, 8.0f, 1);
        }
        else if ((dn % 15) == 0)
        {
            snprintf(buf, sizeof(buf), "%d", dn);
            Ah_UseDim();
            Ah_Text(buf, x, cy + 11.0f, 6.0f, 1);
        }
    }

    Ah_UseMain();
    snprintf(buf, sizeof(buf), "%03d", ((int)(headingDeg + 0.5f)) % 360);
    Ah_Box(-17.0f, cy - 22.0f, 17.0f, cy - 6.0f, s_th);
    Ah_Text(buf, 0.0f, cy - 18.5f, 9.0f, 1);
    Ah_Line(0.0f, cy - 6.0f, -4.0f, cy - 1.0f, s_th);
    Ah_Line(0.0f, cy - 6.0f,  4.0f, cy - 1.0f, s_th);
}

/* One outline per missile Harry can carry, left to right from l: ready ones
 * filled, the one coming back filled as far as its recharge. */
static float Ah_MissileIcons(float l, float midY, float len)
{
    const int   n     = Pc_FlightArcade_StockMax();
    const int   stock = Pc_FlightArcade_Stock();
    const float h     = len * 0.22f, gap = len * 0.3f;
    int         i;

    for (i = 0; i < n; i++)
    {
        const float x0 = l + i * (len + gap), nose = x0 + len, body = nose - len * 0.25f;
        const float t = midY - h * 0.5f, b = midY + h * 0.5f;
        float       f = i < stock ? 1.0f : (i == stock ? Pc_FlightArcade_Recharge01() : 0.0f);

        Ah_UseDim();
        Ah_Box(x0, t, body, b, 0.8f);
        Ah_Line(body, t, nose, midY, 0.8f);
        Ah_Line(body, b, nose, midY, 0.8f);
        Ah_Line(x0, t, x0 - len * 0.12f, t - h * 0.6f, 0.8f);
        Ah_Line(x0, b, x0 - len * 0.12f, b + h * 0.6f, 0.8f);
        if (f <= 0.0f)
            continue;
        Ah_UseMain();
        Ah_Rect(x0, t, x0 + (body - x0) * f, b);
        if (f >= 1.0f)
            Ah_Tri(body, t, nose, midY, body, b);
    }
    return n * (len + gap) - gap;
}

/* Blinks faster as the missile closes: 0.5 s at 12 m, 0.08 s at contact. */
static void Ah_MissileMarks(float nowS)
{
    const AfMissile* m;
    const int        n  = Pc_FlightArcade_Missiles(&m);
    const AfVec3     me = { Ah_Q12f(g_SysWork.playerWork.player.position.vx),
                            Ah_Q12f(g_SysWork.playerWork.player.position.vy),
                            Ah_Q12f(g_SysWork.playerWork.player.position.vz) };
    int              i;

    for (i = 0; i < n; i++)
    {
        float       hx, hy, depth, k, period;
        const float r = 10.0f;

        if (!m[i].alive || m[i].fromHarry || m[i].decoyed)
            continue;
        if (!Ah_Project(m[i].pos.x, m[i].pos.y, m[i].pos.z, &hx, &hy, &depth))
            continue;
        k = Af_Dist(m[i].pos, me) / AH_LOCK_RANGE;
        if (k > 1.0f) k = 1.0f;
        period = 0.08f + 0.42f * k;
        if (fmodf(nowS, period) >= period * 0.6f)
            continue;
        Ah_Color(1.0f, 0.15f, 0.1f, 1.0f);
        Ah_Line(hx, hy - r, hx + r, hy, s_th);
        Ah_Line(hx + r, hy, hx, hy + r, s_th);
        Ah_Line(hx, hy + r, hx - r, hy, s_th);
        Ah_Line(hx - r, hy, hx, hy - r, s_th);
    }
}

static void Ah_BuildHud(void)
{
    const s_SubCharacter* pl = &g_SysWork.playerWork.player;
    const int    touch  = Pc_Touch_IsDrivingInput();
    const float  nowS   = (float)SDL_GetTicks() / 1000.0f;
    const float  yawT   = Ah_Turns(pl->rotation.vy);
    const float  hp     = Ah_Q12f(pl->health);
    float        dmg    = 100.0f - hp;
    float        speedKmh = fabsf(Ah_Q12f(pl->moveSpeed)) * 3.6f;
    float        altFt    = -Ah_Q12f(pl->position.vy) * 3.28f;
    const int    fp     = Ah_FirstPerson();
    float        rx, scoreX, radioTop;
    int          tgt, shoot;
    char         buf[96], wName[24], wVal[24];

    if (dmg < 0.0f)   dmg = 0.0f;
    if (dmg > 100.0f) dmg = 100.0f;

    s_th = 1.4f;

    /* Touch puts Quick Save / Quick Load in the top band when they are on. */
    radioTop = AH_TOUCH_QUICKSAVE() ? -172.0f : -232.0f;
    if (fp)
    {
        float vh, vp;
        Ah_ViewAngles(&vh, &vp);
        Ah_PitchLadder(vh, vp);
        Ah_CompassTape(vh, -170.0f);
        if (radioTop > -200.0f)
            radioTop = -134.0f;
    }

    rx = s_w2 * 0.38f;
    if (rx > 150.0f) rx = 150.0f;
    snprintf(buf, sizeof(buf), "%d", (int)(speedKmh + 0.5f));
    Ah_Readout(-rx, "SPEED", buf, 1);
    snprintf(buf, sizeof(buf), "%d", (int)floorf(altFt + 0.5f));
    Ah_Readout(rx, "ALT", buf, -1);

    if (!(g_SysWork.playerCombat.isAiming && g_PcConfig.crosshair))
        Ah_GunReticle();

    tgt = Ah_Targets(nowS, &shoot);
    if (shoot)
    {
        Ah_UseMain();
        Ah_Box(-19.0f, 46.0f, 19.0f, 60.0f, s_th);
        Ah_Text("SHOOT", 0.0f, 49.5f, 7.0f, 1);
    }

    Ah_UseMain();
    {
        const float x = -s_w2 + 18.0f;
        Uint32 t = (Uint32)(g_SavegamePtr->gameplayTimer >> 12);
        snprintf(buf, sizeof(buf), "TIME: %02u:%02u:%02u", t / 3600u, (t / 60u) % 60u, t % 60u);
        Ah_Text(buf, x, -224.0f, 8.0f, 0);
        snprintf(buf, sizeof(buf), "SCORE: %d", Ah_Kills() * 1000);
        Ah_Text(buf, x, -210.0f, 8.0f, 0);
        scoreX = x + Ah_TextWidth(buf, 8.0f) + 24.0f;
        if (tgt >= 0)
        {
            snprintf(buf, sizeof(buf), "TARGET: %s +1000", Ah_TargetName(tgt));
            Ah_Text(buf, x, -196.0f, 8.0f, 0);
        }
        if (Ah_Remaining() > 0)
        {
            snprintf(buf, sizeof(buf), "TGT REMAINING: %d", Ah_Remaining());
            Ah_Text(buf, x, -182.0f, 8.0f, 0);
        }
    }

    Ah_WeaponRow(wName, wVal, sizeof(wName));

    if (!touch)
    {
        const float colR = s_w2 - 20.0f;
        const float colL = colR - 120.0f;
        const float size = 8.0f;

        Ah_Radar(-s_w2 + 18.0f, 128.0f, 136.0f, 100.0f, yawT, nowS);

        Ah_UseMain();
        Ah_Text(">", colL - 9.0f, 96.0f, size, 0);
        Ah_Text(wName, colL, 96.0f, size, 0);
        Ah_Text(wVal, colR, 96.0f, size, 2);

        Ah_UseMain();
        if (s_flareMsgT > 0.0f)
            Ah_UseHi();
        Ah_Text("FLR", colL, 112.0f, size, 0);
        snprintf(buf, sizeof(buf), "%d", s_flareStock);
        Ah_Text(buf, colR, 112.0f, size, 2);
        Ah_RechargeBar(colL + 28.0f, 113.0f, colR - colL - 44.0f, 5.0f);

        {
            const int   arcade = Pc_FlightArcade_Active();
            const float dmgY   = arcade ? 144.0f : 128.0f;

            if (arcade)
            {
                if (Pc_FlightArcade_LaunchMsgT() > 0.0f)
                    Ah_UseHi();
                else
                    Ah_UseMain();
                Ah_Text("MSL", colL, 128.0f, size, 0);
                snprintf(buf, sizeof(buf), "%d", Pc_FlightArcade_Stock());
                Ah_Text(buf, colR, 128.0f, size, 2);
                Ah_MissileIcons(colL + 30.0f, 128.0f + size * 0.5f, 16.0f);
            }

            Ah_UseMain();
            Ah_Text("DMG", colL, dmgY, size, 0);
            snprintf(buf, sizeof(buf), "%d%%", (int)(dmg + 0.5f));
            Ah_Text(buf, colR, dmgY, size, 2);

            Ah_HealthColor(hp, nowS);
            Ah_Silhouette(colR - 45.0f, dmgY + 20.0f, 72.0f);
        }
    }
    else
    {
        /* Touch puts buttons in the bottom-right corner, so the panels collapse
         * to one row in the free strip at the bottom centre. The radar stays
         * bottom-left: the movement stick floats and the HUD takes no touches,
         * so at worst a thumb covers it while steering. */
        float w;
        Ah_Radar(-s_w2 + 18.0f, 138.0f, 116.0f, 86.0f, yawT, nowS);
        const int arcade = Pc_FlightArcade_Active();
        if (arcade)
            snprintf(buf, sizeof(buf), "DMG %d%%  %s %s  FLR %d  MSL %d", (int)(dmg + 0.5f), wName, wVal,
                     s_flareStock, Pc_FlightArcade_Stock());
        else
            snprintf(buf, sizeof(buf), "DMG %d%%  %s %s  FLR %d", (int)(dmg + 0.5f), wName, wVal, s_flareStock);
        w = Ah_TextWidth(buf, 8.0f);
        Ah_UseMain();
        if (s_flareMsgT > 0.0f)
            Ah_UseHi();
        Ah_Text(buf, 10.0f, 214.0f, 8.0f, 1);
        {
            const float fw = Ah_TextWidth("FLR 4", 8.0f);
            const float mw = arcade ? Ah_TextWidth("  MSL 2", 8.0f) : 0.0f;
            Ah_RechargeBar(10.0f + w * 0.5f - fw - mw, 224.0f, fw, 4.0f);
            if (arcade)
                Ah_MissileIcons(10.0f + w * 0.5f + 8.0f, 218.0f, 12.0f);
        }
        Ah_HealthColor(hp, nowS);
        Ah_Silhouette(10.0f - w * 0.5f - 14.0f, 196.0f, 30.0f);
    }

    if (s_alert && pl->health > Q12(0.0f))
    {
        if (fmodf(nowS, 0.5f) < 0.32f)
        {
            const float w = Ah_TextWidth("MISSILE ALERT", 20.0f) * 0.5f + 12.0f;
            Ah_UseMain();
            Ah_Text("MISSILE ALERT", 0.0f, -90.0f, 20.0f, 1);
            Ah_Line(-w, -96.0f, w, -96.0f, s_th);
            Ah_Line(-w, -62.0f, w, -62.0f, s_th);
        }
        if (s_danger && fmodf(nowS, 0.25f) < 0.16f)
        {
            Ah_UseHi();
            Ah_Text("EVADE", 0.0f, -55.0f, 11.0f, 1);
        }
    }
    else if (s_anyTrack)
    {
        Ah_UseMain();
        Ah_Text("WARNING", 0.0f, -82.0f, 11.0f, 1);
    }

    if (s_flareMsgT > 0.0f && fmodf(nowS, 0.2f) < 0.13f)
    {
        Ah_UseHi();
        Ah_Text("FLARE", 0.0f, 70.0f, 10.0f, 1);
    }
    else if (s_emptyMsgT > 0.0f)
    {
        Ah_UseMain();
        Ah_Text("NO FLARES", 0.0f, 70.0f, 10.0f, 1);
    }

    if (Pc_FlightArcade_NoMslT() > 0.0f)
    {
        Ah_UseMain();
        Ah_Text("NO MISSILES", 0.0f, 86.0f, 10.0f, 1);
    }

    Ah_MissileMarks(nowS);
    Ah_Events(scoreX, -210.0f, radioTop, 104.0f, nowS);
}

/* ------------------------------------------------------------------ */
/* Classic style (flight_hud = 2): tapes, boresight, round radar      */
/* ------------------------------------------------------------------ */

static void Ah_HeadingTape(float headingDeg)
{
    const float cy = -196.0f, half = 120.0f, ppd = 4.0f;
    char  buf[8];
    int   d;
    int   base = (int)floorf(headingDeg / 5.0f) * 5;

    for (d = base - 35; d <= base + 35; d += 5)
    {
        float x = (d - headingDeg) * ppd;
        int   dn = ((d % 360) + 360) % 360;
        if (x < -half || x > half)
            continue;

        Ah_UseDim();
        Ah_Line(x, cy, x, cy + ((dn % 10) == 0 ? 8.0f : 4.0f), s_th);
        if ((dn % 30) == 0)
        {
            const char* lbl = buf;
            switch (dn)
            {
                case 0:   lbl = "N"; break;
                case 90:  lbl = "E"; break;
                case 180: lbl = "S"; break;
                case 270: lbl = "W"; break;
                default:  snprintf(buf, sizeof(buf), "%02d", dn / 10); break;
            }
            Ah_UseMain();
            Ah_Text(lbl, x, cy + 11.0f, 8.0f, 1);
        }
    }

    Ah_UseMain();
    snprintf(buf, sizeof(buf), "%03d", ((int)(headingDeg + 0.5f)) % 360);
    Ah_Box(-17.0f, cy - 22.0f, 17.0f, cy - 6.0f, s_th);
    Ah_Text(buf, 0.0f, cy - 18.5f, 9.0f, 1);
    Ah_Line(0.0f, cy - 6.0f, -4.0f, cy - 1.0f, s_th);
    Ah_Line(0.0f, cy - 6.0f,  4.0f, cy - 1.0f, s_th);
}

/* Vertical tape with a value box. side: -1 left, +1 right; ticks and labels
 * face outward, the box sits on the inner edge. */
static void Ah_Tape(float x, float value, float unitsPerTick, int labelEvery,
                    const char* title, const char* boxText, int side)
{
    const float half = 80.0f, pp = 7.0f;
    char  buf[12];
    int   base = (int)floorf(value / unitsPerTick);
    int   k;

    for (k = base - 13; k <= base + 13; k++)
    {
        float v = k * unitsPerTick;
        float y = -(v - value) / unitsPerTick * pp;
        int   major = (labelEvery > 0) && (k % labelEvery) == 0;
        if (y < -half || y > half)
            continue;

        Ah_UseDim();
        Ah_Line(x, y, x + side * (major ? 9.0f : 5.0f), y, s_th);
        if (major && fabsf(y) > 12.0f)
        {
            snprintf(buf, sizeof(buf), "%d", (int)v);
            Ah_Text(buf, x + side * 12.0f, y - 3.5f, 7.0f, side < 0 ? 2 : 0);
        }
    }

    Ah_UseDim();
    Ah_Line(x, -half, x, half, s_th);

    Ah_UseMain();
    {
        float w = Ah_TextWidth(boxText, 10.0f) + 10.0f;
        float l = (side < 0) ? x + 4.0f : x - 4.0f - w;
        Ah_Box(l, -9.0f, l + w, 9.0f, s_th);
        Ah_Text(boxText, l + w * 0.5f, -5.0f, 10.0f, 1);
    }
    Ah_Text(title, x, -half - 14.0f, 8.0f, 1);
}

static void Ah_Boresight(void)
{
    Ah_UseMain();
    Ah_Line(-16.0f, 0.0f, -8.0f, 0.0f, s_th);
    Ah_Line(-8.0f, 0.0f, -4.0f, 6.0f, s_th);
    Ah_Line(-4.0f, 6.0f, 0.0f, 0.0f, s_th);
    Ah_Line(0.0f, 0.0f, 4.0f, 6.0f, s_th);
    Ah_Line(4.0f, 6.0f, 8.0f, 0.0f, s_th);
    Ah_Line(8.0f, 0.0f, 16.0f, 0.0f, s_th);
}

static void Ah_TargetsClassic(float nowS)
{
    const s_SubCharacter* pl = &g_SysWork.playerWork.player;
    float hx[NPC_COUNT_MAX], hy[NPC_COUNT_MAX], dist[NPC_COUNT_MAX], depth[NPC_COUNT_MAX];
    int   vis[NPC_COUNT_MAX];
    int   i, best = -1;

    for (i = 0; i < NPC_COUNT_MAX; i++)
    {
        const s_SubCharacter* npc = &g_SysWork.npcs[i];
        float wx, wy, wz, dx, dz;

        vis[i] = 0;
        if (!Ah_NpcLive(npc))
            continue;

        wx = Ah_Q12f(npc->position.vx + npc->collision.shapeOffsets.box.vx);
        wz = Ah_Q12f(npc->position.vz + npc->collision.shapeOffsets.box.vz);
        wy = Ah_Q12f(npc->position.vy + npc->collision.box.offsetY);
        dx = wx - Ah_Q12f(pl->position.vx);
        dz = wz - Ah_Q12f(pl->position.vz);
        dist[i] = sqrtf(dx * dx + dz * dz);
        if (dist[i] > AH_TARGET_RANGE)
            continue;
        if (!Ah_Project(wx, wy, wz, &hx[i], &hy[i], &depth[i]))
            continue;
        if (hx[i] < -s_w2 - 40.0f || hx[i] > s_w2 + 40.0f || hy[i] < -280.0f || hy[i] > 280.0f)
            continue;

        vis[i] = 1;
        if (best < 0 || dist[i] < dist[best])
            best = i;
    }

    for (i = 0; i < NPC_COUNT_MAX; i++)
    {
        float half, below;
        char  buf[16];

        if (!vis[i])
            continue;

        half = 260.0f / depth[i];
        if (half < 9.0f)  half = 9.0f;
        if (half > 30.0f) half = 30.0f;

        if (s_lockState[i] == 1 && fmodf(nowS, 0.3f) < 0.15f)
            Ah_UseHi();
        else
            Ah_UseMain();

        Ah_Box(hx[i] - half, hy[i] - half, hx[i] + half, hy[i] + half, s_th);

        if (s_lockState[i] == 2)
        {
            float d = half + 7.0f;
            Ah_Line(hx[i], hy[i] - d, hx[i] + d, hy[i], s_th);
            Ah_Line(hx[i] + d, hy[i], hx[i], hy[i] + d, s_th);
            Ah_Line(hx[i], hy[i] + d, hx[i] - d, hy[i], s_th);
            Ah_Line(hx[i] - d, hy[i], hx[i], hy[i] - d, s_th);
            half = d;
        }

        if (i == s_seekSlot)
        {
            Ah_SeekerMark(hx[i], hy[i], half, nowS);
            Ah_UseMain();
        }

        Ah_Text(Ah_TargetName(i), hx[i], hy[i] - half - 10.0f, 6.5f, 1);
        below = hy[i] + half + 4.0f;
        if (i == best)
        {
            Ah_TargetHpBar(i, hx[i], below, 2.0f * half);
            below += 9.0f;
            Ah_UseMain();
        }
        snprintf(buf, sizeof(buf), "%d", (int)(dist[i] + 0.5f));
        Ah_Text(buf, hx[i], below, 6.5f, 1);
    }
}

static void Ah_RadarClassic(float cx, float cy, float r, float yawTurns, float nowS)
{
    const s_SubCharacter* pl = &g_SysWork.playerWork.player;
    float yaw = yawTurns * 2.0f * AH_PI;
    float fx = sinf(yaw), fz = cosf(yaw);
    float rx = cosf(yaw), rz = -sinf(yaw);
    float px = Ah_Q12f(pl->position.vx), pz = Ah_Q12f(pl->position.vz);
    int   i;

    Ah_UseDim();
    Ah_Circle(cx, cy, r, s_th, 40);
    Ah_Circle(cx, cy, r * 0.5f, s_th * 0.7f, 28);
    Ah_Line(cx, cy - r, cx, cy - r + 6.0f, s_th);

    Ah_UseMain();
    Ah_Tri(cx, cy - 6.0f, cx - 4.5f, cy + 4.0f, cx + 4.5f, cy + 4.0f);

    for (i = 0; i < NPC_COUNT_MAX; i++)
    {
        const s_SubCharacter* npc = &g_SysWork.npcs[i];
        float dx, dz, lx, ly, len, bx, by;
        if (!Ah_NpcLive(npc))
            continue;

        dx  = Ah_Q12f(npc->position.vx) - px;
        dz  = Ah_Q12f(npc->position.vz) - pz;
        lx  = dx * rx + dz * rz;
        ly  = dx * fx + dz * fz;
        len = sqrtf(lx * lx + ly * ly);
        if (len > AH_RADAR_RANGE)
        {
            lx *= AH_RADAR_RANGE / len;
            ly *= AH_RADAR_RANGE / len;
        }
        bx = cx + lx / AH_RADAR_RANGE * (r - 3.0f);
        by = cy - ly / AH_RADAR_RANGE * (r - 3.0f);

        if (s_lockState[i] != 0 && fmodf(nowS, 0.3f) < 0.15f)
            Ah_UseHi();
        else
            Ah_UseMain();
        Ah_Rect(bx - 2.5f, by - 2.5f, bx + 2.5f, by + 2.5f);
    }
}

static void Ah_WeaponLines(char* l1, char* l2, size_t n)
{
    int w = g_SavegamePtr->equippedWeapon;

    snprintf(l1, n, "%s", Ah_WeaponName(w));
    if (w == InvItemId_HyperBlaster)
        snprintf(l2, n, "AMMO INF");
    else if (w >= InvItemId_Handgun && w <= InvItemId_Shotgun)
        snprintf(l2, n, "AMMO %d/%d", g_SysWork.playerCombat.currentWeaponAmmo,
                 g_SysWork.playerCombat.totalWeaponAmmo);
    else if (w >= InvItemId_KitchenKnife && w <= InvItemId_Axe)
        snprintf(l2, n, "MELEE");
    else
        l2[0] = '\0';
}

static void Ah_FlareLine(float xRight, float y, float size)
{
    char buf[16];
    float w;
    snprintf(buf, sizeof(buf), "FLR %d", s_flareStock);
    if (s_flareMsgT > 0.0f)
        Ah_UseHi();
    else
        Ah_UseMain();
    Ah_Text(buf, xRight, y, size, 2);

    w = Ah_TextWidth(buf, size);
    Ah_RechargeBar(xRight - w, y + size + 3.0f, w, 4.0f);
}

static void Ah_MslLine(float xRight, float y, float size)
{
    char buf[16];
    if (!Pc_FlightArcade_Active())
        return;
    snprintf(buf, sizeof(buf), "MSL %d", Pc_FlightArcade_Stock());
    if (Pc_FlightArcade_LaunchMsgT() > 0.0f)
        Ah_UseHi();
    else
        Ah_UseMain();
    Ah_Text(buf, xRight, y, size, 2);
    {
        const float len = size * 1.6f;
        const float iw  = Pc_FlightArcade_StockMax() * len * 1.3f - len * 0.3f;
        Ah_MissileIcons(xRight - Ah_TextWidth(buf, size) - 8.0f - iw, y + size * 0.5f, len);
    }
}

static void Ah_BuildHudClassic(float vpW, float vpH)
{
    const s_SubCharacter* pl = &g_SysWork.playerWork.player;
    const int    touch  = Pc_Touch_IsDrivingInput();
    const float  nowS   = (float)SDL_GetTicks() / 1000.0f;
    const float  yawT   = Ah_Turns(pl->rotation.vy);
    const float  hdg    = yawT * 360.0f;
    const float  hp     = Ah_Q12f(pl->health);
    float        dmg    = 100.0f - hp;
    float        speedKmh = fabsf(Ah_Q12f(pl->moveSpeed)) * 3.6f;
    float        altFt    = -Ah_Q12f(pl->position.vy) * 3.28f;
    float        tapeX, scoreX;
    char         buf[48], l1[24], l2[24];

    (void)vpH;
    if (dmg < 0.0f)   dmg = 0.0f;
    if (dmg > 100.0f) dmg = 100.0f;

    s_th = 1.4f;

    /* In first person the view's own compass takes the tape's place. */
    if (Ah_FirstPerson())
    {
        float vh, vp;
        Ah_ViewAngles(&vh, &vp);
        Ah_PitchLadder(vh, vp);
        Ah_CompassTape(vh, -196.0f);
    }
    else
        Ah_HeadingTape(hdg);

    tapeX = s_w2 * 0.55f;
    if (tapeX > 190.0f) tapeX = 190.0f;
    snprintf(buf, sizeof(buf), "%d", (int)(speedKmh + 0.5f));
    Ah_Tape(-tapeX, speedKmh, 1.0f, 5, "SPEED", buf, -1);
    snprintf(l1, sizeof(l1), "%d", (int)floorf(altFt + 0.5f));
    Ah_Tape(tapeX, altFt, 2.0f, 5, "ALT", l1, 1);

    if (!(g_SysWork.playerCombat.isAiming && g_PcConfig.crosshair))
        Ah_Boresight();

    Ah_TargetsClassic(nowS);

    Ah_UseMain();
    {
        Uint32 t = (Uint32)(g_SavegamePtr->gameplayTimer >> 12);
        snprintf(buf, sizeof(buf), "TIME %02u:%02u:%02u", t / 3600u, (t / 60u) % 60u, t % 60u);
        Ah_Text(buf, s_w2 - 18.0f, -222.0f, 8.0f, 2);
        snprintf(buf, sizeof(buf), "SCORE %06d", Ah_Kills() * 1000);
        Ah_Text(buf, s_w2 - 18.0f, -208.0f, 8.0f, 2);
        scoreX = s_w2 - 18.0f - Ah_TextWidth(buf, 8.0f) - 24.0f;
        if (Ah_Remaining() > 0)
        {
            snprintf(buf, sizeof(buf), "TGT REMAINING %d", Ah_Remaining());
            Ah_Text(buf, s_w2 - 18.0f, -194.0f, 8.0f, 2);
        }
    }

    Ah_WeaponLines(l1, l2, sizeof(l1));

    if (!touch)
    {
        const float lx = -s_w2 + 20.0f;
        const float rr = 50.0f;
        const float rcx = s_w2 - 20.0f - rr, rcy = 170.0f;
        const float wx  = rcx - rr - 14.0f;

        Ah_UseMain();
        Ah_Text("DMG", lx, 140.0f, 8.0f, 0);
        snprintf(buf, sizeof(buf), "%d%%", (int)(dmg + 0.5f));
        Ah_Text(buf, lx, 154.0f, 16.0f, 0);
        Ah_UseDim();
        Ah_Box(lx, 178.0f, lx + 110.0f, 186.0f, s_th);
        Ah_UseMain();
        Ah_Rect(lx + 2.0f, 180.0f, lx + 2.0f + 106.0f * (hp < 0.0f ? 0.0f : (hp > 100.0f ? 1.0f : hp / 100.0f)), 184.0f);

        Ah_HealthColor(hp, nowS);
        Ah_Silhouette(lx + 150.0f, 118.0f, 72.0f);
        Ah_UseMain();

        Ah_Text(l1, wx, 146.0f, 9.0f, 2);
        if (l2[0])
            Ah_Text(l2, wx, 162.0f, 9.0f, 2);
        Ah_FlareLine(wx, 178.0f, 9.0f);
        Ah_MslLine(wx, 200.0f, 9.0f);

        Ah_RadarClassic(rcx, rcy, rr, yawT, nowS);
    }
    else
    {
        /* Touch puts buttons in the bottom-right corner, so the panels collapse
         * to one row in the free strip at the bottom centre. The radar stays
         * bottom-left: the movement stick floats and the HUD takes no touches,
         * so at worst a thumb covers it while steering. */
        Ah_RadarClassic(-s_w2 + 65.0f, 180.0f, 45.0f, yawT, nowS);
        snprintf(buf, sizeof(buf), "DMG %d%%", (int)(dmg + 0.5f));
        Ah_UseMain();
        Ah_Text(buf, -12.0f, 214.0f, 8.0f, 2);
        Ah_HealthColor(hp, nowS);
        Ah_Silhouette(-12.0f - Ah_TextWidth(buf, 8.0f) - 14.0f, 196.0f, 30.0f);
        Ah_UseMain();
        snprintf(buf, sizeof(buf), "%s %s", l1, l2);
        Ah_Text(buf, 12.0f, 214.0f, 8.0f, 0);
        Ah_FlareLine(Ah_TextWidth("FLR 4", 8.0f) * 0.5f, 194.0f, 8.0f);
        Ah_MslLine(Ah_TextWidth("FLR 4", 8.0f) * 0.5f, 178.0f, 8.0f);
    }

    if (s_alert && pl->health > Q12(0.0f))
    {
        if (fmodf(nowS, 0.5f) < 0.32f)
        {
            const float w = Ah_TextWidth("MISSILE ALERT", 20.0f) * 0.5f + 12.0f;
            Ah_UseMain();
            Ah_Text("MISSILE ALERT", 0.0f, -80.0f, 20.0f, 1);
            Ah_Line(-w, -86.0f, w, -86.0f, s_th);
            Ah_Line(-w, -52.0f, w, -52.0f, s_th);
        }
        if (s_danger && fmodf(nowS, 0.25f) < 0.16f)
        {
            Ah_UseHi();
            Ah_Text("EVADE", 0.0f, -45.0f, 11.0f, 1);
        }
    }
    else if (s_anyTrack)
    {
        Ah_UseMain();
        Ah_Text("WARNING", 0.0f, -72.0f, 11.0f, 1);
    }

    if (s_flareMsgT > 0.0f && fmodf(nowS, 0.2f) < 0.13f)
    {
        Ah_UseHi();
        Ah_Text("FLARE", 0.0f, 40.0f, 10.0f, 1);
    }
    else if (s_emptyMsgT > 0.0f)
    {
        Ah_UseMain();
        Ah_Text("NO FLARES", 0.0f, 40.0f, 10.0f, 1);
    }

    if (Pc_FlightArcade_NoMslT() > 0.0f)
    {
        Ah_UseMain();
        Ah_Text("NO MISSILES", 0.0f, 56.0f, 10.0f, 1);
    }

    /* Below the heading tape, which owns the top band. */
    Ah_MissileMarks(nowS);
    Ah_Events(scoreX, -208.0f, -168.0f, 104.0f, nowS);

    (void)vpW;
}

static void Ah_BuildMissiles(void)
{
    const AfMissile* m;
    const int        n = Pc_FlightArcade_Missiles(&m);
    int              i;

    for (i = 0; i < n; i++)
    {
        float hx, hy, depth, rad;
        float inner[4] = { 1.0f, 0.9f, 0.7f, 0.95f }, outer[4] = { 1.0f, 0.25f, 0.1f, 0.0f };

        if (!m[i].alive || !Ah_Project(m[i].pos.x, m[i].pos.y, m[i].pos.z, &hx, &hy, &depth))
            continue;
        if (m[i].fromHarry)
        {
            inner[0] = 0.8f; inner[1] = 1.0f; inner[2] = 0.85f;
            outer[0] = 0.3f; outer[1] = 1.0f; outer[2] = 0.5f;
        }
        rad = 0.18f * s_camH / depth * s_ky;
        if (rad < 3.0f)  rad = 3.0f;
        if (rad > 32.0f) rad = 32.0f;
        Ah_Glow(hx, hy, rad, inner, outer);
    }
}

static void Ah_BuildFlares(void)
{
    static const float core[4] = { 1.0f, 1.0f, 0.95f, 1.0f };
    int i, k;

    for (i = 0; i < AH_PARTICLES_MAX; i++)
    {
        const s_AhFlare* f = &s_flares[i];
        float hx, hy, depth, rad, a;
        float inner[4], outer[4] = { 1.0f, 0.45f, 0.1f, 0.0f };
        float px, py, pd, qx, qy, qd;

        if (!f->alive || !Ah_Project(f->x, f->y, f->z, &hx, &hy, &depth))
            continue;

        a = f->life / 0.8f;
        if (a > 1.0f) a = 1.0f;
        a *= 0.8f + 0.2f * Ah_Rand();

        /* Burning trail, oldest segment faintest. */
        px = hx; py = hy; pd = depth;
        for (k = 0; k < f->trailN; k++)
        {
            float ta = a * (1.0f - (float)(k + 1) / (AH_TRAIL_N + 1)) * 0.7f;
            if (!Ah_Project(f->tx[k], f->ty[k], f->tz[k], &qx, &qy, &qd))
                break;
            Ah_Color(1.0f, 0.55f, 0.2f, ta);
            Ah_Line(px, py, qx, qy, 2.4f * 3.0f / (pd < 1.0f ? 1.0f : pd) + 0.8f);
            px = qx; py = qy; pd = qd;
        }

        rad = 0.14f * s_camH / depth * s_ky;
        if (rad < 2.5f)  rad = 2.5f;
        if (rad > 28.0f) rad = 28.0f;

        inner[0] = 1.0f; inner[1] = 0.85f; inner[2] = 0.55f; inner[3] = a;
        Ah_Glow(hx, hy, rad, inner, outer);
        {
            float c[4], o[4];
            memcpy(c, core, sizeof(c));
            c[3] = a;
            o[0] = 1.0f; o[1] = 0.95f; o[2] = 0.8f; o[3] = 0.0f;
            Ah_Glow(hx, hy, rad * 0.35f, c, o);
        }
    }
}

/* ------------------------------------------------------------------ */
/* GL                                                                  */
/* ------------------------------------------------------------------ */

static GLuint s_prog, s_vao, s_vbo;
static GLint  s_locOfs, s_locShadow;
static int    s_glReady;

static GLuint Ah_Shader(GLenum type, const char* src)
{
    GLuint sh = glCreateShader(type);
    GLint  ok = 0;
    const char* parts[2] = { PsyX_Shader_Preamble(type == GL_FRAGMENT_SHADER, PSYX_GLSL_LEGACY), src };
    glShaderSource(sh, 2, parts, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[512];
        glGetShaderInfoLog(sh, (GLsizei)sizeof(log), NULL, log);
        SH_DBG("[FLIGHTHUD] shader failed: %s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static void Ah_GlInit(void)
{
    static const char* vs_src =
        "attribute vec2 a_pos;\n"
        "attribute vec4 a_col;\n"
        "uniform vec2 u_ofs;\n"
        "varying vec4 v_col;\n"
        "void main() {\n"
        "    v_col = a_col;\n"
        "    gl_Position = vec4(a_pos + u_ofs, 0.0, 1.0);\n"
        "}\n";
    static const char* fs_src =
        "#ifdef GL_ES\n"
        "precision mediump float;\n"
        "#endif\n"
        "varying vec4 v_col;\n"
        "uniform float u_shadow;\n"
        "void main() {\n"
        "    gl_FragColor = mix(v_col, vec4(0.0, 0.0, 0.0, v_col.a * 0.6), u_shadow);\n"
        "}\n";
    GLuint vs, fs;
    GLint  ok = 0, prevVao = 0, prevBuf = 0;

    s_glReady = -1;

    vs = Ah_Shader(GL_VERTEX_SHADER, vs_src);
    fs = Ah_Shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs)
        return;

    s_prog = glCreateProgram();
    glAttachShader(s_prog, vs);
    glAttachShader(s_prog, fs);
    glBindAttribLocation(s_prog, 0, "a_pos");
    glBindAttribLocation(s_prog, 1, "a_col");
    glLinkProgram(s_prog);
    glGetProgramiv(s_prog, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok)
    {
        char log[512];
        glGetProgramInfoLog(s_prog, (GLsizei)sizeof(log), NULL, log);
        SH_DBG("[FLIGHTHUD] program link failed: %s", log);
        glDeleteProgram(s_prog);
        s_prog = 0;
        return;
    }
    s_locOfs    = glGetUniformLocation(s_prog, "u_ofs");
    s_locShadow = glGetUniformLocation(s_prog, "u_shadow");

    /* Runs before the draw's own state save, so hand back exactly what was bound
     * on entry: PsyCross caches its bindings and would otherwise keep drawing
     * through our buffer (see pc_ra_toast.c). */
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
    glGenVertexArrays(1, &s_vao);
    glBindVertexArray(s_vao);
    glGenBuffers(1, &s_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(s_hudV), NULL, GL_STREAM_DRAW);
    glBindVertexArray((GLuint)prevVao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevBuf);

    s_glReady = 1;
}

static void Ah_Submit(const s_AhBatch* b)
{
    glBufferData(GL_ARRAY_BUFFER, sizeof(s_hudV), NULL, GL_STREAM_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)b->n * AH_VERT_FLOATS * sizeof(float), b->v);
    glDrawArrays(GL_TRIANGLES, 0, b->n);
}

/* ------------------------------------------------------------------ */
/* Comm portraits from the game's own picture                          */
/* ------------------------------------------------------------------ */

/* The speaker's face is cut out of the rendered frame: the real model, lit and
 * textured by the game, projected through the same world->screen matrix the
 * frame was drawn with (GsWSMATRIX, Q8 world units). A monster on the radio
 * gets a live feed while it is on screen; otherwise the best close-up seen so
 * far is shown. Cybil is rarely on screen when she calls, so her best capture
 * is kept on disk and survives restarts. With no capture yet, the wireframe
 * bust stands in. */

#define AH_PORT_SIZE  128
#define AH_PORT_DIR   "gamedata/hud_portraits"
#define AH_PORT_MAGIC 0x31504853u /* "SHP1" */

static void Ah_PortraitPath(int key, char* buf, size_t n)
{
    snprintf(buf, n, AH_PORT_DIR "/%d.rgba", key);
}

static GLuint Ah_PortraitNewTex(const unsigned char* rgba)
{
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, AH_PORT_SIZE, AH_PORT_SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    return t;
}

/* File: magic, quality, then RGBA rows bottom-up as GL reads them. */
static void Ah_PortraitLoadAll(void)
{
    static unsigned char px[AH_PORT_SIZE * AH_PORT_SIZE * 4];
    int key;

    s_portLoaded = 1;
    for (key = 0; key < Chara_Count; key++)
    {
        char     path[96];
        FILE*    f;
        unsigned hdr[2];

        if (Ah_PortraitKey(key) != key)
            continue;
        Ah_PortraitPath(key, path, sizeof(path));
        f = fopen(path, "rb");
        if (!f)
            continue;
        if (fread(hdr, sizeof(hdr), 1, f) == 1 && hdr[0] == AH_PORT_MAGIC &&
            fread(px, sizeof(px), 1, f) == 1)
        {
            s_portTex[key]  = Ah_PortraitNewTex(px);
            s_portQual[key] = (float)hdr[1];
        }
        fclose(f);
    }
}

static void Ah_PortraitSave(int key)
{
    static unsigned char px[AH_PORT_SIZE * AH_PORT_SIZE * 4];
    char     path[96];
    unsigned hdr[2];
    FILE*    f;

#ifdef _WIN32
    _mkdir("gamedata");
    _mkdir(AH_PORT_DIR);
#else
    mkdir("gamedata", 0775);
    mkdir(AH_PORT_DIR, 0775);
#endif
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, AH_PORT_SIZE, AH_PORT_SIZE, GL_RGBA, GL_UNSIGNED_BYTE, px);

    Ah_PortraitPath(key, path, sizeof(path));
    f = fopen(path, "wb");
    if (!f)
        return;
    hdr[0] = AH_PORT_MAGIC;
    hdr[1] = (unsigned)s_portQual[key];
    fwrite(hdr, sizeof(hdr), 1, f);
    fwrite(px, sizeof(px), 1, f);
    fclose(f);
}

/* Where to aim the camera on each body plan, metres above the feet, and how
 * much of it to frame. Flyers and the big ones use their collision centre. */
static void Ah_HeadOf(const s_SubCharacter* npc, int key, float* wx, float* wy, float* wz, float* rad)
{
    const float yaw = Ah_Turns(npc->rotation.vy) * 2.0f * AH_PI;
    float up = -1.0f, fwd = 0.0f;

    *rad = 0.32f;
    if (key == Chara_Cybil)
        up = 1.60f;
    else
    {
        switch (Ah_FaceOf(key))
        {
            case AH_FACE_HUMAN: up = 1.52f; break;
            case AH_FACE_CHILD: up = 1.02f; *rad = 0.30f; break;
            case AH_FACE_DOG:   up = 0.48f; fwd = 0.35f; *rad = 0.40f; break;
            default:            *rad = 0.65f; break;
        }
    }

    *wx = Ah_Q12f(npc->position.vx) + sinf(yaw) * fwd;
    *wz = Ah_Q12f(npc->position.vz) + cosf(yaw) * fwd;
    *wy = (up > 0.0f) ? Ah_Q12f(npc->position.vy) - up
                      : Ah_Q12f(npc->position.vy + npc->collision.box.offsetY);
}

static void Ah_PortraitCapture(const GLint* vp, float nowS)
{
    const MATRIX* m = &GsWSMATRIX;
    const float   H = s_camH;
    const Uint32  now = SDL_GetTicks();
    float camX, camZ, tx, ty, tz;
    GLint prevRead = 0, prevDraw = 0, prevTex = 0, prevUnit = 0;
    GLboolean scissor = GL_FALSE;
    int   i, did = 0;

    (void)nowS;

    /* Camera position on the ground plane: -R^T t, back in metres. */
    tx = (float)m->t[0]; ty = (float)m->t[1]; tz = (float)m->t[2];
    camX = -(m->m[0][0] * tx + m->m[1][0] * ty + m->m[2][0] * tz) / 4096.0f / 256.0f;
    camZ = -(m->m[0][2] * tx + m->m[1][2] * ty + m->m[2][2] * tz) / 4096.0f / 256.0f;

    for (i = 0; i < NPC_COUNT_MAX; i++)
    {
        const s_SubCharacter* npc = &g_SysWork.npcs[i];
        const int key = Ah_PortraitKey(npc->model.charaId);
        float wx, wy, wz, rad, vx, vy, vz, px, py, r, fx, fz, cx, cz, cl;
        int   live, srcX0, srcY0, srcX1, srcY1;

        if (key < 0 || npc->health <= Q12(0.0f))
            continue;
        if (key != Chara_Cybil && !Ah_NpcLive(npc))
            continue;

        if (s_portQual[key] >= 10000.0f && g_PcConfig.flightHudPortrait3d)
            continue;
        live = (key == s_portLive);
        if (now - s_portTakenMs[key] < (Uint32)(live ? 90 : 400))
            continue;

        Ah_HeadOf(npc, key, &wx, &wy, &wz, &rad);
        wx *= 256.0f; wy *= 256.0f; wz *= 256.0f;
        vx = (m->m[0][0] * wx + m->m[0][1] * wy + m->m[0][2] * wz) / 4096.0f + m->t[0];
        vy = (m->m[1][0] * wx + m->m[1][1] * wy + m->m[1][2] * wz) / 4096.0f + m->t[1];
        vz = (m->m[2][0] * wx + m->m[2][1] * wy + m->m[2][2] * wz) / 4096.0f + m->t[2];
        if (vz < 0.6f * 256.0f || vz > 12.0f * 256.0f)
            continue;

        px = (float)vp[0] + (0.5f + (s_cx + (vx * H / vz) * s_kx) / (2.0f * s_w2)) * (float)vp[2];
        py = (float)vp[1] + (0.5f - (s_cy + (vy * H / vz) * s_ky) / 480.0f) * (float)vp[3];
        r  = (rad * 256.0f * H / vz) * s_ky / 480.0f * (float)vp[3];
        if (r < 20.0f)
            continue;
        if (px - r < vp[0] || py - r < vp[1] || px + r > vp[0] + vp[2] || py + r > vp[1] + vp[3])
            continue;

        /* A face, not the back of a head: the body must turn toward the lens. */
        if (Ah_FaceOf(key) != AH_FACE_BIRD && Ah_FaceOf(key) != AH_FACE_BEAST)
        {
            fx = sinf(Ah_Turns(npc->rotation.vy) * 2.0f * AH_PI);
            fz = cosf(Ah_Turns(npc->rotation.vy) * 2.0f * AH_PI);
            cx = camX - wx / 256.0f;
            cz = camZ - wz / 256.0f;
            cl = sqrtf(cx * cx + cz * cz);
            if (cl < 0.01f || (fx * cx + fz * cz) / cl < 0.35f)
                continue;
        }

        if (!live && s_portTex[key] && r < s_portQual[key] * 0.8f)
            continue;

        if (!did)
        {
            did = 1;
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDraw);
            glGetIntegerv(GL_ACTIVE_TEXTURE, &prevUnit);
            glActiveTexture(GL_TEXTURE0);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
            scissor = glIsEnabled(GL_SCISSOR_TEST);
            if (scissor)
                glDisable(GL_SCISSOR_TEST);
            if (!s_portFbo)
                glGenFramebuffers(1, &s_portFbo);
        }

        if (!s_portTex[key])
            s_portTex[key] = Ah_PortraitNewTex(NULL);

        srcX0 = (int)(px - r); srcX1 = (int)(px + r);
        srcY0 = (int)(py - r * 1.05f); srcY1 = (int)(py + r * 0.95f);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, GR_ScreenReadFBO());
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_portFbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_portTex[key], 0);
        glBlitFramebuffer(srcX0, srcY0, srcX1, srcY1, 0, 0, AH_PORT_SIZE, AH_PORT_SIZE,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);

        s_portTakenMs[key] = now;
        if (r >= s_portQual[key] || !live)
        {
            s_portQual[key] = r;
            if (now - s_portSavedMs[key] > 5000)
            {
                s_portSavedMs[key] = now;
                glBindFramebuffer(GL_READ_FRAMEBUFFER, s_portFbo);
                Ah_PortraitSave(key);
            }
        }
    }

    if (did)
    {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prevRead);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prevDraw);
        glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
        glActiveTexture((GLenum)prevUnit);
        if (scissor)
            glEnable(GL_SCISSOR_TEST);
    }
}

static void Ah_PortraitTexInit(void)
{
    static const char* vs_src =
        "attribute vec2 a_pos;\n"
        "attribute vec2 a_uv;\n"
        "varying vec2 v_uv;\n"
        "void main() {\n"
        "    v_uv = a_uv;\n"
        "    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
        "}\n";
    /* A comm screen, not a photo: colour pulled toward the HUD tint, scanlines,
     * and a slow roll bar. */
    static const char* fs_src =
        "#ifdef GL_ES\n"
        "precision mediump float;\n"
        "#endif\n"
        "varying vec2 v_uv;\n"
        "uniform sampler2D u_tex;\n"
        "uniform vec4 u_tint;\n"
        "uniform float u_mono;\n"
        "uniform float u_time;\n"
        "void main() {\n"
        "    vec3 c = texture2D(u_tex, v_uv).rgb;\n"
        "    float l = dot(c, vec3(0.3, 0.59, 0.11));\n"
        "    c = mix(c, l * 1.6 * u_tint.rgb, u_mono);\n"
        "    float scan = 0.78 + 0.22 * step(0.5, fract(gl_FragCoord.y * 0.5));\n"
        "    float roll = 1.0 + 0.12 * smoothstep(0.92, 1.0, fract(v_uv.y * 0.7 - u_time * 0.25));\n"
        "    gl_FragColor = vec4(c * scan * roll, u_tint.a);\n"
        "}\n";
    GLuint vs = Ah_Shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = Ah_Shader(GL_FRAGMENT_SHADER, fs_src);
    GLint  ok = 0;

    if (!vs || !fs)
        return;
    s_texProg = glCreateProgram();
    glAttachShader(s_texProg, vs);
    glAttachShader(s_texProg, fs);
    glBindAttribLocation(s_texProg, 0, "a_pos");
    glBindAttribLocation(s_texProg, 1, "a_uv");
    glLinkProgram(s_texProg);
    glGetProgramiv(s_texProg, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok)
    {
        glDeleteProgram(s_texProg);
        s_texProg = 0;
        return;
    }
    s_texLocTex  = glGetUniformLocation(s_texProg, "u_tex");
    s_texLocTint = glGetUniformLocation(s_texProg, "u_tint");
    s_texLocMono = glGetUniformLocation(s_texProg, "u_mono");
    s_texLocTime = glGetUniformLocation(s_texProg, "u_time");
}

/* Draws the queued portrait quad, then hands the colour program its vertex
 * layout back. */
static void Ah_PortraitDraw(float nowS)
{
    const int key = s_portDraw.chara;
    float l, t, r, b, v[6][4], inset = 2.0f;
    GLint prevTex = 0;

    if (!s_portDraw.on || !s_texProg || key < 0 || !s_portTex[key])
        return;

    l = (s_portDraw.l + inset) / s_w2;
    r = (s_portDraw.l + s_portDraw.size - inset) / s_w2;
    t = -(s_portDraw.t + inset) / 240.0f;
    b = -(s_portDraw.t + s_portDraw.size - inset) / 240.0f;
    v[0][0] = l; v[0][1] = t; v[0][2] = 0.0f; v[0][3] = 1.0f;
    v[1][0] = l; v[1][1] = b; v[1][2] = 0.0f; v[1][3] = 0.0f;
    v[2][0] = r; v[2][1] = t; v[2][2] = 1.0f; v[2][3] = 1.0f;
    v[3][0] = r; v[3][1] = t; v[3][2] = 1.0f; v[3][3] = 1.0f;
    v[4][0] = l; v[4][1] = b; v[4][2] = 0.0f; v[4][3] = 0.0f;
    v[5][0] = r; v[5][1] = b; v[5][2] = 1.0f; v[5][3] = 0.0f;

    glUseProgram(s_texProg);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glBindTexture(GL_TEXTURE_2D, s_portTex[key]);
    glUniform1i(s_texLocTex, 0);
    if (s_portDraw.monster)
        glUniform4f(s_texLocTint, 1.0f, 0.35f, 0.3f, s_main[3]);
    else
        glUniform4f(s_texLocTint, s_main[0], s_main[1], s_main[2], s_main[3]);
    glUniform1f(s_texLocMono, s_portDraw.monster ? 0.55f : 0.35f);
    glUniform1f(s_texLocTime, nowS);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);

    glUseProgram(s_prog);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, AH_VERT_FLOATS * sizeof(float), (void*)0);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, AH_VERT_FLOATS * sizeof(float), (void*)(2 * sizeof(float)));
}

/* ------------------------------------------------------------------ */
/* 3D comm portrait: the speaker's model, converted and drawn here     */
/* ------------------------------------------------------------------ */

/* The radio portrait as the speaker's own model, drawn by this module into the
 * portrait texture with its own camera, depth buffer and light: nothing passes
 * through the game's renderer, so nothing can spill out of the box.
 *
 * SH1 characters are rigid parts, one per bone, no skinning. The global chara
 * pool keeps every character's model, animation and texture resident, so the
 * conversion is done on the fly every frame, mirroring the game's own draw:
 *  - parts in skeleton order, each part's vertices taken from its bone frame
 *    into world space (R*v/4096 + T) and written into a shared vertex pool at
 *    ModelHeader.vertexOffset; prims index that POOL, which is how a part
 *    picks up the already-placed seam vertices of its neighbour (see
 *    pc_port/tools/ilm_obj.py, resolve_pool);
 *  - each prim's tpage/clut is looked up the same way the renderer finds the
 *    pool's GL textures (HiresOverride_LookupByTpageClut), with the PSX UVs
 *    mapped into the native TIM;
 *  - the pose is the live NPC's when the speaker is in the scene, else the
 *    first keyframe of its pool animation. */

#define AH_P3D_MAX_BONES 57
#define AH_P3D_POOL      256
#define AH_P3D_MAX_TRIS  4096
#define AH_P3D_FLOATS    8       /* pos3, uv2, normal3 */
#define AH_P3D_TEXCACHE  32

extern void* Pc_CharaPool_ModelOf(int charaId);
extern void  Math_MatrixTransform(VECTOR3* pos, SVECTOR* rot, GsCOORDINATE2* coord);
extern int   Pc_WideLm_IsWide(const s_ModelHeader* modelHdr);
#include "hires_override.h"

static GsCOORDINATE2 s_p3dCoords[AH_P3D_MAX_BONES];
static int           s_p3dPuppetKey = -1;
static float         s_p3dVerts[AH_P3D_MAX_TRIS * 3 * AH_P3D_FLOATS];

typedef struct
{
    GLuint tex;
    int    first, count; /* vertices in s_p3dVerts */
} s_AhP3dBatch;

static s_AhP3dBatch s_p3dBatches[AH_P3D_MAX_TRIS];
static int          s_p3dBatchN;

static GLuint s_mdlProg, s_mdlVao, s_mdlVbo, s_mdlFbo, s_mdlDepth;
static GLint  s_mdlLocMvp, s_mdlLocTex, s_mdlLocTint, s_mdlLocLight, s_mdlLocAmb;
static int    s_mdlReady;

static void Ah_ModelGlInit(void)
{
    static const char* vs_src =
        "attribute vec3 a_pos;\n"
        "attribute vec2 a_uv;\n"
        "attribute vec3 a_nrm;\n"
        "uniform mat4 u_mvp;\n"
        "varying vec2 v_uv;\n"
        "varying vec3 v_nrm;\n"
        "void main() {\n"
        "    v_uv = a_uv;\n"
        "    v_nrm = a_nrm;\n"
        "    gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
        "}\n";
    static const char* fs_src =
        "#ifdef GL_ES\n"
        "precision mediump float;\n"
        "#endif\n"
        "varying vec2 v_uv;\n"
        "varying vec3 v_nrm;\n"
        "uniform sampler2D u_tex;\n"
        "uniform vec3 u_light;\n"
        "uniform float u_amb;\n"
        "uniform vec4 u_tint;\n"
        "void main() {\n"
        "    vec4 t = texture2D(u_tex, v_uv);\n"
        "    if (t.a < 0.5) discard;\n"
        "    vec3 n = normalize(v_nrm);\n"
        "    float d = abs(dot(n, u_light));\n"
        "    float rim = pow(1.0 - abs(n.z), 3.0);\n"
        "    vec3 c = t.rgb * (u_amb + (1.0 - u_amb) * d) * 1.25 + rim * 0.35 * u_tint.rgb;\n"
        "    gl_FragColor = vec4(c, 1.0);\n"
        "}\n";
    GLuint vs, fs;
    GLint  ok = 0, prevVao = 0, prevBuf = 0;

    s_mdlReady = -1;
    vs = Ah_Shader(GL_VERTEX_SHADER, vs_src);
    fs = Ah_Shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs)
        return;
    s_mdlProg = glCreateProgram();
    glAttachShader(s_mdlProg, vs);
    glAttachShader(s_mdlProg, fs);
    glBindAttribLocation(s_mdlProg, 0, "a_pos");
    glBindAttribLocation(s_mdlProg, 1, "a_uv");
    glBindAttribLocation(s_mdlProg, 2, "a_nrm");
    glLinkProgram(s_mdlProg);
    glGetProgramiv(s_mdlProg, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok)
    {
        glDeleteProgram(s_mdlProg);
        s_mdlProg = 0;
        return;
    }
    s_mdlLocMvp   = glGetUniformLocation(s_mdlProg, "u_mvp");
    s_mdlLocTex   = glGetUniformLocation(s_mdlProg, "u_tex");
    s_mdlLocTint  = glGetUniformLocation(s_mdlProg, "u_tint");
    s_mdlLocLight = glGetUniformLocation(s_mdlProg, "u_light");
    s_mdlLocAmb   = glGetUniformLocation(s_mdlProg, "u_amb");

    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
    glGenVertexArrays(1, &s_mdlVao);
    glBindVertexArray(s_mdlVao);
    glGenBuffers(1, &s_mdlVbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_mdlVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(s_p3dVerts), NULL, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, AH_P3D_FLOATS * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, AH_P3D_FLOATS * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, AH_P3D_FLOATS * sizeof(float), (void*)(5 * sizeof(float)));
    glBindVertexArray((GLuint)prevVao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevBuf);

    glGenFramebuffers(1, &s_mdlFbo);
    glGenRenderbuffers(1, &s_mdlDepth);
    s_mdlReady = 1;
}

typedef struct
{
    int    tpage, clut;
    GLuint tex;
    float  su, sv, ou, ov;
} s_AhTexCache;

static GLuint Ah_ModelTex(s_AhTexCache* cache, int* n, int tpage, int clut, float* su, float* sv, float* ou, float* ov)
{
    int    i, nw = 0, nh = 0, ox = 0, oy = 0, hw = 0, hh = 0;
    GLuint t;

    for (i = 0; i < *n; i++)
    {
        if (cache[i].tpage == tpage && cache[i].clut == clut)
        {
            *su = cache[i].su; *sv = cache[i].sv; *ou = cache[i].ou; *ov = cache[i].ov;
            return cache[i].tex;
        }
    }
    t = HiresOverride_LookupByTpageClut(tpage, clut, &nw, &nh, &ox, &oy, &hw, &hh);
    if (t == 0 || nw <= 0 || nh <= 0)
        return 0;
    *su = 1.0f / (float)nw;
    *sv = 1.0f / (float)nh;
    *ou = (float)ox + 0.5f;
    *ov = (float)oy + 0.5f;
    if (*n < AH_P3D_TEXCACHE)
    {
        cache[*n].tpage = tpage; cache[*n].clut = clut; cache[*n].tex = t;
        cache[*n].su = *su; cache[*n].sv = *sv; cache[*n].ou = *ou; cache[*n].ov = *ov;
        (*n)++;
    }
    return t;
}

/* Converts the posed model into s_p3dVerts / s_p3dBatches, world units in
 * metres (game axes, +Y down). Returns the triangle count. */
static int Ah_ModelBuild(const s_CharaModel* model, GsCOORDINATE2* coords, int boneCount)
{
    static float vpool[AH_P3D_POOL][3], npool[AH_P3D_POOL][3];
    s_AhTexCache cache[AH_P3D_TEXCACHE];
    int          cacheN = 0, tris = 0;
    const s_LinkedBone* lb;

    s_p3dBatchN = 0;
    memset(vpool, 0, sizeof(vpool));
    memset(npool, 0, sizeof(npool));

    for (lb = model->skeleton.bones_4; lb != NULL; lb = lb->next)
    {
        const s_ModelHeader* mh = lb->bone.modelInfo.modelHdr;
        const int            bi = (u8)lb->bone.idx;
        MATRIX               W;
        float                R[9], T[3];
        int                  k;

        if (lb->bone.modelInfo.field_0 < 0 || mh == NULL || bi >= boneCount || Pc_WideLm_IsWide(mh))
            continue;

        Vw_CoordHierarchyMatrixCompute(&coords[bi], &W);
        for (k = 0; k < 9; k++)
            R[k] = (float)W.m[k / 3][k % 3] / 4096.0f;
        T[0] = (float)W.t[0]; T[1] = (float)W.t[1]; T[2] = (float)W.t[2];

        for (k = 0; k < mh->meshCount; k++)
        {
            const s_MeshHeader* me = &mh->meshHdrs[k];
            int j, p;

            for (j = 0; j < me->vertexCount && mh->vertexOffset + j < AH_P3D_POOL; j++)
            {
                const float x = me->verticesXy[j].vx, y = me->verticesXy[j].vy, z = me->verticesZ[j];
                float* o = vpool[mh->vertexOffset + j];
                o[0] = (R[0] * x + R[1] * y + R[2] * z + T[0]) / 256.0f;
                o[1] = (R[3] * x + R[4] * y + R[5] * z + T[1]) / 256.0f;
                o[2] = (R[6] * x + R[7] * y + R[8] * z + T[2]) / 256.0f;
            }
            for (j = 0; j < me->normalCount && mh->normalOffset + j < AH_P3D_POOL; j++)
            {
                /* Stored pointing inward. */
                const float x = -me->normals[j].nx, y = -me->normals[j].ny, z = -me->normals[j].nz;
                float* o = npool[mh->normalOffset + j];
                o[0] = R[0] * x + R[1] * y + R[2] * z;
                o[1] = R[3] * x + R[4] * y + R[5] * z;
                o[2] = R[6] * x + R[7] * y + R[8] * z;
            }

            for (p = 0; p < me->primitiveCount; p++)
            {
                static const int quadTris[6] = { 0, 1, 2, 1, 3, 2 };
                const s_Primitive* pr = &me->primitives[p];
                const u16 uvw[4] = { pr->field_0, pr->field_4, pr->field_8, pr->field_A };
                const int quad = pr->field_C[3] != 0xFF;
                const int nIdx = quad ? 6 : 3;
                float     su, sv, ou, ov;
                GLuint    tex;
                int       c;

                tex = Ah_ModelTex(cache, &cacheN, pr->field_6.bits.field_6_0, pr->field_2, &su, &sv, &ou, &ov);
                if (tex == 0 || tris + (quad ? 2 : 1) > AH_P3D_MAX_TRIS)
                    continue;

                if (s_p3dBatchN == 0 || s_p3dBatches[s_p3dBatchN - 1].tex != tex)
                {
                    s_p3dBatches[s_p3dBatchN].tex   = tex;
                    s_p3dBatches[s_p3dBatchN].first = tris * 3;
                    s_p3dBatches[s_p3dBatchN].count = 0;
                    s_p3dBatchN++;
                }

                for (c = 0; c < nIdx; c++)
                {
                    const int   corner = quad ? quadTris[c] : c;
                    const int   vi = pr->field_C[corner], ni = pr->field_10[corner];
                    float*      o = &s_p3dVerts[(tris * 3 + c) * AH_P3D_FLOATS];
                    const float* v = vpool[vi < AH_P3D_POOL ? vi : 0];
                    const float* nn = npool[ni < AH_P3D_POOL ? ni : 0];

                    o[0] = v[0]; o[1] = v[1]; o[2] = v[2];
                    o[3] = ((float)(uvw[corner] & 0xFF) + ou) * su;
                    o[4] = ((float)(uvw[corner] >> 8) + ov) * sv;
                    o[5] = nn[0]; o[6] = nn[1]; o[7] = nn[2];
                }
                tris += quad ? 2 : 1;
                s_p3dBatches[s_p3dBatchN - 1].count += nIdx;
            }
        }
    }
    return tris;
}

/* ------------------------------------------------------------------ */
/* Health figure: Harry's own model as a wireframe                     */
/* ------------------------------------------------------------------ */

#define AH_WIRE_MAX  4000
#define AH_WIRE_TURN 0.35f /* rad: a quarter turn short of front-on reads as 3D */

/* Edges in figure space: x centred on 0, y from 0 (head) to 1 (feet). */
static float          s_wire[AH_WIRE_MAX][4];
static int            s_wireN;
static const void*    s_wireLm;
static const void*    s_wireAnm;
static int            s_wireMap = -1;
static GsCOORDINATE2  s_wireCoords[AH_P3D_MAX_BONES];

/* Harry standing still: keyframe 0 of his base animation, the pose the
 * TransitionToStill loop holds, arms down at his sides. */
static int Ah_WireBuild(void)
{
    static unsigned char seen[AH_P3D_POOL][AH_P3D_POOL / 8];
    static float         vpool[AH_P3D_POOL][3];
    const s_CharaModel*  model = &g_WorldGfxWork.harryModel;
    s_AnmHeader*         anm   = g_CharaModelAnimsData[0].activeAnmHdr;
    const s_LinkedBone*  lb;
    const float          ct = cosf(AH_WIRE_TURN), st = sinf(AH_WIRE_TURN);
    float                minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f, k, mx;
    VECTOR3              pos  = { 0, 0, 0 };
    SVECTOR              prot = { 0, 0, 0 };
    int                  i, n = 0;

    if (model->lmHdr == NULL || model->skeleton.bones_4 == NULL || anm == NULL ||
        anm->boneCount == 0 || anm->boneCount > AH_P3D_MAX_BONES || anm->keyframeCount == 0)
        return 0;
    /* A map load can reload the player model in place (Play As). */
    if (s_wireLm == model->lmHdr && s_wireAnm == anm && s_wireMap == g_SavegamePtr->mapIdx)
        return s_wireN > 0;
    s_wireLm  = model->lmHdr;
    s_wireAnm = anm;
    s_wireMap = g_SavegamePtr->mapIdx;
    s_wireN   = 0;

    Anim_BoneInit(anm, s_wireCoords);
    Math_MatrixTransform(&pos, &prot, &s_wireCoords[0]);
    for (i = 0; i < anm->boneCount; i++)
        s_wireCoords[i].flg = 0;
    Anim_BoneUpdate(anm, s_wireCoords, 0, 0, Q12(0.0f));

    memset(vpool, 0, sizeof(vpool));
    for (lb = model->skeleton.bones_4; lb != NULL; lb = lb->next)
    {
        const s_ModelHeader* mh = lb->bone.modelInfo.modelHdr;
        const int            bi = (u8)lb->bone.idx;
        MATRIX               W;
        float                R[9], T[3];
        int                  m;

        if (lb->bone.modelInfo.field_0 < 0 || mh == NULL || bi >= anm->boneCount || Pc_WideLm_IsWide(mh))
            continue;

        Vw_CoordHierarchyMatrixCompute(&s_wireCoords[bi], &W);
        for (i = 0; i < 9; i++)
            R[i] = (float)W.m[i / 3][i % 3] / 4096.0f;
        T[0] = (float)W.t[0]; T[1] = (float)W.t[1]; T[2] = (float)W.t[2];

        for (m = 0; m < mh->meshCount; m++)
        {
            const s_MeshHeader* me = &mh->meshHdrs[m];
            int j, p;

            for (j = 0; j < me->vertexCount && mh->vertexOffset + j < AH_P3D_POOL; j++)
            {
                const float x = me->verticesXy[j].vx, y = me->verticesXy[j].vy, z = me->verticesZ[j];
                float* o = vpool[mh->vertexOffset + j];
                o[0] = (R[0] * x + R[1] * y + R[2] * z + T[0]) / 256.0f;
                o[1] = (R[3] * x + R[4] * y + R[5] * z + T[1]) / 256.0f;
                o[2] = (R[6] * x + R[7] * y + R[8] * z + T[2]) / 256.0f;
            }

            /* Pool slots are reused bone to bone, so an edge is only a
             * duplicate of one seen in the same mesh. */
            memset(seen, 0, sizeof(seen));
            for (p = 0; p < me->primitiveCount; p++)
            {
                static const int triRing[4] = { 0, 1, 2, 0 }, quadRing[5] = { 0, 1, 3, 2, 0 };
                const s_Primitive* pr   = &me->primitives[p];
                const int          quad = pr->field_C[3] != 0xFF;
                const int*         ring = quad ? quadRing : triRing;
                const int          ne   = quad ? 4 : 3;
                int                e;

                for (e = 0; e < ne && n < AH_WIRE_MAX; e++)
                {
                    int a = pr->field_C[ring[e]], b = pr->field_C[ring[e + 1]], lo, hi;
                    const float *va, *vb;

                    lo = a < b ? a : b;
                    hi = a < b ? b : a;
                    if (seen[lo][hi >> 3] & (1 << (hi & 7)))
                        continue;
                    seen[lo][hi >> 3] |= (unsigned char)(1 << (hi & 7));

                    va = vpool[a];
                    vb = vpool[b];
                    s_wire[n][0] = va[0] * ct + va[2] * st;
                    s_wire[n][1] = va[1];
                    s_wire[n][2] = vb[0] * ct + vb[2] * st;
                    s_wire[n][3] = vb[1];
                    n++;
                }
            }
        }
    }

    for (i = 0; i < n; i++)
    {
        int c;
        for (c = 0; c < 4; c += 2)
        {
            if (s_wire[i][c] < minX)     minX = s_wire[i][c];
            if (s_wire[i][c] > maxX)     maxX = s_wire[i][c];
            if (s_wire[i][c + 1] < minY) minY = s_wire[i][c + 1];
            if (s_wire[i][c + 1] > maxY) maxY = s_wire[i][c + 1];
        }
    }
    if (n == 0 || maxY - minY < 0.3f)
        return 0;

    k  = 1.0f / (maxY - minY);
    mx = (minX + maxX) * 0.5f;
    for (i = 0; i < n; i++)
    {
        s_wire[i][0] = (s_wire[i][0] - mx) * k;
        s_wire[i][1] = (s_wire[i][1] - minY) * k;
        s_wire[i][2] = (s_wire[i][2] - mx) * k;
        s_wire[i][3] = (s_wire[i][3] - minY) * k;
    }
    s_wireN = n;
    SH_DBG("[FLIGHT_HUD] health wireframe: %d edges, %.2f m tall", n, maxY - minY);
    return 1;
}

static int Ah_WireHarry(float cx, float top, float h)
{
    float th = h * 0.011f;
    int   i;

    if (!Ah_WireBuild())
        return 0;
    if (th < 0.5f) th = 0.5f;
    if (th > 1.0f) th = 1.0f;
    for (i = 0; i < s_wireN; i++)
        Ah_Line(cx + s_wire[i][0] * h, top + s_wire[i][1] * h,
                cx + s_wire[i][2] * h, top + s_wire[i][3] * h, th);
    return 1;
}

static void Ah_Mat4Mul(float* o, const float* a, const float* b)
{
    int r, c, k;
    for (c = 0; c < 4; c++)
        for (r = 0; r < 4; r++)
        {
            float s = 0.0f;
            for (k = 0; k < 4; k++)
                s += a[k * 4 + r] * b[c * 4 + k];
            o[c * 4 + r] = s;
        }
}

/* Renders the speaker into s_portTex[key]. Returns 1 on success. */
static int Ah_ModelPortraitRender(int key, int monster, float nowS)
{
    const s_CharaModel* model = (const s_CharaModel*)Pc_CharaPool_ModelOf(key);
    GsCOORDINATE2* coords = NULL;
    s_AnmHeader*   anm    = NULL;
    float          yaw = 0.0f, minY = 1e9f, maxY = -1e9f, h, half, dist, a;
    float          tx = 0.0f, tz = 0.0f, ty, ex, ey, ez, fx, fy, fz, rx, ry, rz, ux, uy, uz, l;
    float          view[16], proj[16], mvp[16], fov;
    int            i, nb, tris, topN = 0;
    GLint          prevRead = 0, prevDraw = 0, prevVp[4], prevProg = 0, prevVao = 0, prevBuf = 0, prevTex = 0, prevUnit = 0;
    GLint          prevDepthFunc = GL_LESS;
    GLboolean      prevDepth, prevBlend, prevCull, prevScissor, prevDepthMask = GL_TRUE;
    GLfloat        prevClear[4];
    MATRIX         m;

    if (model == NULL || model->lmHdr == NULL || model->skeleton.bones_4 == NULL)
        return 0;

    /* Pose: the speaker's live skeleton if it is in the scene and posed by the
     * pool's own animation (the bone layout must match the pool model). */
    for (i = 0; i < NPC_COUNT_MAX; i++)
    {
        const s_SubCharacter* npc = &g_SysWork.npcs[i];
        const int idx = g_CharaAnimDataIdxs[key];
        if (Ah_PortraitKey(npc->model.charaId) != key || npc->health <= Q12(0.0f) ||
            !(npc->model.anim.flags & AnimFlag_Visible) || idx < 0 || idx >= CHARA_ANIM_DATA_COUNT ||
            g_CharaModelAnimsData[idx].boneCoords == NULL || g_CharaModelAnimsData[idx].activeAnmHdr == NULL)
            continue;
        coords = g_CharaModelAnimsData[idx].boneCoords;
        anm    = g_CharaModelAnimsData[idx].activeAnmHdr;
        yaw    = Ah_Turns(npc->rotation.vy) * 2.0f * AH_PI;
        break;
    }
    if (coords == NULL)
    {
        VECTOR3 pos  = { 0, 0, 0 };
        SVECTOR prot = { 0, 0, 0 };

        anm = g_CharaModelAnimsData[PC_CHARA_ANIM_SLOT(key)].activeAnmHdr;
        if (anm == NULL || anm->boneCount == 0 || anm->boneCount > AH_P3D_MAX_BONES || anm->keyframeCount == 0)
            return 0;
        if (s_p3dPuppetKey != key)
        {
            Anim_BoneInit(anm, s_p3dCoords);
            s_p3dPuppetKey = key;
        }
        Math_MatrixTransform(&pos, &prot, &s_p3dCoords[0]);
        for (i = 0; i < anm->boneCount; i++)
            s_p3dCoords[i].flg = 0;
        Anim_BoneUpdate(anm, s_p3dCoords, 0, anm->keyframeCount > 1 ? 1 : 0, Q12(0.0f));
        coords = s_p3dCoords;
    }
    nb = anm->boneCount;
    if (nb <= 0 || nb > AH_P3D_MAX_BONES)
        return 0;

    tris = Ah_ModelBuild(model, coords, nb);
    if (tris == 0)
        return 0;

    /* Frame on the top of the body: the head on anything upright, the front of
     * the body on anything that is not. */
    for (i = 0; i < nb; i++)
    {
        Vw_CoordHierarchyMatrixCompute(&coords[i], &m);
        if (m.t[1] / 256.0f < minY) minY = m.t[1] / 256.0f;
        if (m.t[1] / 256.0f > maxY) maxY = m.t[1] / 256.0f;
    }
    h = maxY - minY;
    if (h < 0.3f) h = 0.3f;
    for (i = 0; i < nb; i++)
    {
        Vw_CoordHierarchyMatrixCompute(&coords[i], &m);
        if (m.t[1] / 256.0f <= minY + h * 0.3f)
        {
            tx += m.t[0] / 256.0f;
            tz += m.t[2] / 256.0f;
            topN++;
        }
    }
    if (topN == 0)
        return 0;
    tx /= topN;
    tz /= topN;
    /* Bone origins are joints: the highest one on a person is the neck, so a
     * point below it framed the chest. The face sits just above. */
    if (key == Chara_Cybil || Ah_FaceOf(key) == AH_FACE_HUMAN || Ah_FaceOf(key) == AH_FACE_CHILD)
        ty = minY - h * 0.04f;
    else
        ty = minY + h * 0.2f;
    half = h * 0.24f;
    if (half < 0.22f) half = 0.22f;
    if (half > 0.8f)  half = 0.8f;
    dist = half * 4.0f;

    /* Three-quarter view a little above the eyes, with a slow idle drift. */
    a  = yaw + 0.45f + 0.12f * sinf(nowS * 0.6f);
    ex = tx + sinf(a) * dist;
    ez = tz + cosf(a) * dist;
    ey = ty - dist * 0.08f;

    /* Look-at, world +Y is down so "up" is -Y. */
    fx = tx - ex; fy = ty - ey; fz = tz - ez;
    l  = sqrtf(fx * fx + fy * fy + fz * fz); fx /= l; fy /= l; fz /= l;
    rx = fy * 0.0f - fz * -1.0f; ry = fz * 0.0f - fx * 0.0f; rz = fx * -1.0f - fy * 0.0f;
    l  = sqrtf(rx * rx + ry * ry + rz * rz); rx /= l; ry /= l; rz /= l;
    ux = ry * fz - rz * fy; uy = rz * fx - rx * fz; uz = rx * fy - ry * fx;
    view[0] = rx; view[4] = ry; view[8]  = rz; view[12] = -(rx * ex + ry * ey + rz * ez);
    view[1] = ux; view[5] = uy; view[9]  = uz; view[13] = -(ux * ex + uy * ey + uz * ez);
    view[2] = -fx; view[6] = -fy; view[10] = -fz; view[14] = (fx * ex + fy * ey + fz * ez);
    view[3] = 0.0f; view[7] = 0.0f; view[11] = 0.0f; view[15] = 1.0f;

    fov = 2.0f * atanf(half / dist);
    {
        const float f = 1.0f / tanf(fov * 0.5f), zn = 0.05f, zf = 50.0f;
        memset(proj, 0, sizeof(proj));
        proj[0]  = f;
        proj[5]  = f;
        proj[10] = (zf + zn) / (zn - zf);
        proj[11] = -1.0f;
        proj[14] = (2.0f * zf * zn) / (zn - zf);
    }
    Ah_Mat4Mul(mvp, proj, view);

    if (!s_portTex[key])
        s_portTex[key] = Ah_PortraitNewTex(NULL);

    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDraw);
    glGetIntegerv(GL_DEPTH_FUNC, &prevDepthFunc);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);
    glGetIntegerv(GL_VIEWPORT, prevVp);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevUnit);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, prevClear);
    prevDepth   = glIsEnabled(GL_DEPTH_TEST);
    prevBlend   = glIsEnabled(GL_BLEND);
    prevCull    = glIsEnabled(GL_CULL_FACE);
    prevScissor = glIsEnabled(GL_SCISSOR_TEST);

    glBindFramebuffer(GL_FRAMEBUFFER, s_mdlFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_portTex[key], 0);
    {
        static int s_depthSized;
        if (!s_depthSized)
        {
            glBindRenderbuffer(GL_RENDERBUFFER, s_mdlDepth);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, AH_PORT_SIZE, AH_PORT_SIZE);
            glBindRenderbuffer(GL_RENDERBUFFER, 0);
            s_depthSized = 1;
        }
    }
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, s_mdlDepth);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE)
    {
        int b;

        glViewport(0, 0, AH_PORT_SIZE, AH_PORT_SIZE);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_TRUE);
        if (monster)
            glClearColor(0.10f, 0.02f, 0.02f, 1.0f);
        else
            glClearColor(0.02f, 0.08f, 0.04f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glUseProgram(s_mdlProg);
        glBindVertexArray(s_mdlVao);
        glBindBuffer(GL_ARRAY_BUFFER, s_mdlVbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, AH_P3D_FLOATS * sizeof(float), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, AH_P3D_FLOATS * sizeof(float), (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, AH_P3D_FLOATS * sizeof(float), (void*)(5 * sizeof(float)));
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)tris * 3 * AH_P3D_FLOATS * sizeof(float), s_p3dVerts, GL_STREAM_DRAW);

        glUniformMatrix4fv(s_mdlLocMvp, 1, GL_FALSE, mvp);
        glUniform1i(s_mdlLocTex, 0);
        /* Key light from the camera's upper side, in world space. */
        {
            float lx = -fx + rx * 0.4f - ux * 0.5f, ly = -fy + ry * 0.4f - uy * 0.5f, lz = -fz + rz * 0.4f - uz * 0.5f;
            const float ll = sqrtf(lx * lx + ly * ly + lz * lz);
            glUniform3f(s_mdlLocLight, lx / ll, ly / ll, lz / ll);
        }
        glUniform1f(s_mdlLocAmb, 0.45f);
        if (monster)
            glUniform4f(s_mdlLocTint, 1.0f, 0.3f, 0.25f, 1.0f);
        else
            glUniform4f(s_mdlLocTint, 0.45f, 1.0f, 0.55f, 1.0f);

        for (b = 0; b < s_p3dBatchN; b++)
        {
            glBindTexture(GL_TEXTURE_2D, s_p3dBatches[b].tex);
            glDrawArrays(GL_TRIANGLES, s_p3dBatches[b].first, s_p3dBatches[b].count);
        }
    }

    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prevRead);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prevDraw);
    glDepthFunc((GLenum)prevDepthFunc);
    glDepthMask(prevDepthMask);
    glViewport(prevVp[0], prevVp[1], prevVp[2], prevVp[3]);
    glClearColor(prevClear[0], prevClear[1], prevClear[2], prevClear[3]);
    glBindVertexArray((GLuint)prevVao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevBuf);
    glUseProgram((GLuint)prevProg);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    glActiveTexture((GLenum)prevUnit);
    if (prevDepth)  glEnable(GL_DEPTH_TEST);  else glDisable(GL_DEPTH_TEST);
    if (prevBlend)  glEnable(GL_BLEND);       else glDisable(GL_BLEND);
    if (prevCull)   glEnable(GL_CULL_FACE);   else glDisable(GL_CULL_FACE);
    if (prevScissor) glEnable(GL_SCISSOR_TEST);

    /* Drawn on purpose, so it outranks any crop of the same character. */
    s_portQual[key]    = 10000.0f;
    s_portTakenMs[key] = SDL_GetTicks();
    return 1;
}

void Pc_FlightHud_Draw(void)
{
    GLint vp[4];
    float vpW, vpH, aspect;
    GLint prevProg = 0, prevVao = 0, prevBuf = 0;
    GLint prevSrcRgb = GL_ONE, prevDstRgb = GL_ZERO, prevSrcA = GL_ONE, prevDstA = GL_ZERO;
    GLint prevEqRgb = GL_FUNC_ADD, prevEqA = GL_FUNC_ADD;
    GLboolean prevBlend, prevDepth, prevCull;
    int   red;

    /* The inventory, the map and the menus leave the game state, and with it
     * the update, so the tone is cut from here as well. */
    if (!Ah_InGameplay())
        Ah_ToneStop();
    if (!Pc_FlightHud_Enabled() || g_PcConsoleInputActive || g_GameWork.gameState != GameState_InGame)
        return;

    glGetIntegerv(GL_VIEWPORT, vp);
    if (vp[2] <= 0 || vp[3] <= 0)
        return;
    vpW    = (float)vp[2];
    vpH    = (float)vp[3];
    aspect = vpW / vpH;

    if (!s_glReady)
    {
        Ah_GlInit();
        Ah_PortraitTexInit();
    }
    if (s_glReady != 1)
        return;

    s_w2    = 240.0f * aspect;
    s_camH  = (float)ReadGeomScreen();
    Ah_ScreenMap(aspect);
    if (s_camH < 1.0f)
        s_camH = 1.0f;

    /* Cutscenes count: they are where the cast is seen up close. Menus, the
     * map and the pause screen draw something else over the world. */
    if (!s_portLoaded)
        Ah_PortraitLoadAll();
    if (g_PcConfig.flightHudPortrait3d && s_p3dKey >= 0 && Ah_InGameplay())
    {
        if (!s_mdlReady)
            Ah_ModelGlInit();
        if (s_mdlReady == 1)
            Ah_ModelPortraitRender(s_p3dKey, s_p3dMonster, (float)SDL_GetTicks() / 1000.0f);
    }
    switch (g_SysWork.sysState)
    {
        case SysState_Gameplay:
        case SysState_ReadMessage:
        case SysState_EventCallback:
        case SysState_EventSetFlag:
        case SysState_EventPlaySound:
            Ah_PortraitCapture(vp, (float)SDL_GetTicks() / 1000.0f);
            break;
        default:
            break;
    }

    if (!Ah_InGameplay())
        return;

    red = (s_alert && g_SysWork.playerWork.player.health > Q12(0.0f)) || s_dead;
    {
        const float o = (float)g_PcConfig.flightHudOpacity / 100.0f;
        if (red)
        {
            s_main[0] = 1.0f;  s_main[1] = 0.22f; s_main[2] = 0.16f; s_main[3] = 0.95f * o;
            s_hi[0]   = 1.0f;  s_hi[1]   = 0.85f; s_hi[2]   = 0.75f; s_hi[3]   = 1.0f * o;
        }
        else
        {
            s_main[0] = 0.45f; s_main[1] = 1.0f;  s_main[2] = 0.55f; s_main[3] = 0.9f * o;
            s_hi[0]   = 1.0f;  s_hi[1]   = 0.9f;  s_hi[2]   = 0.35f; s_hi[3]   = 1.0f * o;
        }
        memcpy(s_dim, s_main, sizeof(s_dim));
        s_dim[3] *= 0.55f;
    }

    s_hud.n  = 0;
    s_glow.n = 0;
    s_fill.n = 0;
    s_portDraw.on = 0;
    s_portLive    = -1;
    s_p3dKey      = -1;
    s_cur    = &s_glow;
    Ah_BuildFlares();
    Ah_BuildMissiles();
    s_cur = &s_hud;
    s_jx = s_jy = 0.0f;
    if (s_dead || s_hurtT > 0.0f)
    {
        const float amp = s_dead ? 3.0f : 6.0f * s_hurtAmt * s_hurtT / AH_HURT_TIME;
        s_jx = (Ah_Rand() * 2.0f - 1.0f) * amp;
        s_jy = (Ah_Rand() * 2.0f - 1.0f) * amp * 0.5f;
    }
    if (s_dead && s_deadT >= AH_DEAD_FAIL)
        Ah_BuildDead((float)SDL_GetTicks() / 1000.0f);
    else if (g_PcConfig.flightHud == 2)
        Ah_BuildHudClassic(vpW, vpH);
    else
        Ah_BuildHud();
    s_jx = s_jy = 0.0f;

    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
    glGetIntegerv(GL_BLEND_SRC_RGB, &prevSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &prevDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &prevSrcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &prevDstA);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &prevEqRgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &prevEqA);
    prevBlend = glIsEnabled(GL_BLEND);
    prevDepth = glIsEnabled(GL_DEPTH_TEST);
    prevCull  = glIsEnabled(GL_CULL_FACE);

    glUseProgram(s_prog);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, AH_VERT_FLOATS * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, AH_VERT_FLOATS * sizeof(float), (void*)(2 * sizeof(float)));
    glEnable(GL_BLEND);
    /* PsyCross can leave GL_FUNC_REVERSE_SUBTRACT behind after an ABR=2 prim. */
    glBlendEquation(GL_FUNC_ADD);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    if (s_fill.n > 0)
    {
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUniform2f(s_locOfs, 0.0f, 0.0f);
        glUniform1f(s_locShadow, 0.0f);
        Ah_Submit(&s_fill);
    }

    Ah_PortraitDraw((float)SDL_GetTicks() / 1000.0f);

    if (s_glow.n > 0)
    {
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        glUniform2f(s_locOfs, 0.0f, 0.0f);
        glUniform1f(s_locShadow, 0.0f);
        Ah_Submit(&s_glow);
    }

    if (s_hud.n > 0)
    {
        /* A dark copy one pixel down-right first: green strokes alone vanish
         * against the white fog. */
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glBufferData(GL_ARRAY_BUFFER, sizeof(s_hudV), NULL, GL_STREAM_DRAW);
        glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)s_hud.n * AH_VERT_FLOATS * sizeof(float), s_hud.v);
        {
            const float px = (vpH / 480.0f < 1.0f) ? 1.0f : vpH / 480.0f;
            glUniform2f(s_locOfs, 2.0f * px / vpW, -2.0f * px / vpH);
        }
        glUniform1f(s_locShadow, 1.0f);
        glDrawArrays(GL_TRIANGLES, 0, s_hud.n);
        glUniform2f(s_locOfs, 0.0f, 0.0f);
        glUniform1f(s_locShadow, 0.0f);
        glDrawArrays(GL_TRIANGLES, 0, s_hud.n);
    }

    glBindVertexArray((GLuint)prevVao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevBuf);
    glUseProgram((GLuint)prevProg);
    glBlendFuncSeparate((GLenum)prevSrcRgb, (GLenum)prevDstRgb, (GLenum)prevSrcA, (GLenum)prevDstA);
    glBlendEquationSeparate((GLenum)prevEqRgb, (GLenum)prevEqA);
    if (!prevBlend) glDisable(GL_BLEND);
    if (prevDepth)  glEnable(GL_DEPTH_TEST);
    if (prevCull)   glEnable(GL_CULL_FACE);
}
