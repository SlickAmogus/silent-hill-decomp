/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_flight_arcade.c - what the flight HUD does to the game (config key:
 * flight_gameplay, off by default, needs flight_hud). With it off nothing here
 * acts and the game is the PSX one.
 *
 *  - Flares: while a salvo jams the seekers, no hit from a non-boss monster
 *    lands on Harry. Checked in the game's two hit functions.
 *  - A non-boss monster that holds its lock for AF_ENEMY_LAUNCH_DELAY fires a
 *    homing missile at Harry; a flare near it takes it.
 *  - With the seeker locked, the light button (Circle by default) fires one
 *    of Harry's missiles at the seeker's target instead of the flashlight.
 *  - Hits are written into the fields the game's own damage handlers read,
 *    so hurt and death animations are the original ones.
 */
#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/player.h"
#include "bodyprog/sys/joy.h"
#include "bodyprog/sound/sfx_id_enum.h"
#include "bodyprog/sound/sound_system.h"
#include "bodyprog/screen/screen_fade.h"

#include <math.h>
#include <string.h>

#include "sh_log.h"
#include "pc_config.h"
#include "pc_rando.h"
#include "pc_flight_hud.h"
#include "pc_flight_missile.h"
#include "pc_flight_arcade.h"

void func_8005DC1C(e_SfxId sfxId, const VECTOR3* pos, q23_8 vol, s32 soundType);

#define AR_MISSILES_MAX    8
#define AR_FLARES_MAX      24     /* AH_PARTICLES_MAX in pc_flight_hud.c */
#define AR_ENEMY_SPEED     7.0f   /* m/s: Harry running sideways outturns it */
#define AR_ENEMY_TURN      1.2f   /* rad/s */
#define AR_ENEMY_LIFE      4.0f
#define AR_ENEMY_COOLDOWN  6.0f   /* s before the same monster fires again */
#define AR_HIT_HARRY       0.5f   /* m */
#define AR_DECOY_RANGE     8.0f   /* m from a flare that takes a missile */
#define AR_KNOCK           0.5f   /* m of push along the missile's path */
#define AR_PUFF_STEP       0.04f  /* s between smoke puffs */
#define AR_HARRY_HURT      43     /* a plain torso hit in Player_ReceiveDamage, never a grab */

static AfMissile s_msl[AR_MISSILES_MAX];
static AfSmoke   s_smoke;
static float     s_lockHeld[NPC_COUNT_MAX];
static float     s_cool[NPC_COUNT_MAX];

static float Ar_Q12f(s32 v)
{
    return (float)v / 4096.0f;
}

static AfVec3 Ar_Center(const s_SubCharacter* c)
{
    AfVec3 v;
    v.x = Ar_Q12f(c->position.vx + c->collision.shapeOffsets.box.vx);
    v.y = Ar_Q12f(c->position.vy + c->collision.box.offsetY);
    v.z = Ar_Q12f(c->position.vz + c->collision.shapeOffsets.box.vz);
    if (c->collision.box.offsetY == 0)
        v.y -= 1.0f;
    return v;
}

static VECTOR3 Ar_Q12Vec(AfVec3 p)
{
    VECTOR3 v;
    v.vx = (s32)(p.x * 4096.0f);
    v.vy = (s32)(p.y * 4096.0f);
    v.vz = (s32)(p.z * 4096.0f);
    return v;
}

static AfMissile* Ar_FreeSlot(void)
{
    int i;
    for (i = 0; i < AR_MISSILES_MAX; i++)
        if (!s_msl[i].alive)
            return &s_msl[i];
    return NULL;
}

/* The attack each monster lands in melee, so its missile hurts as much. */
static s32 Ar_EnemyAttack(int charaId)
{
    switch (charaId)
    {
        case Chara_AirScreamer:
        case Chara_NightFlutter:
            return 40;
        case Chara_Groaner:
            return WEAPON_ATTACK(EquippedWeaponId_HuntingRifle, AttackInputType_Hold);
        case Chara_Creeper:
            return WEAPON_ATTACK(EquippedWeaponId_HuntingRifle, AttackInputType_Multitap);
        case Chara_HangedScratcher:
            return WEAPON_ATTACK(EquippedWeaponId_Unk44, AttackInputType_Tap);
        case Chara_LarvalStalker:
            return WEAPON_ATTACK(EquippedWeaponId_Unk31, AttackInputType_Multitap);
        case Chara_Romper:
            return WEAPON_ATTACK(EquippedWeaponId_Shotgun, AttackInputType_Multitap);
        case Chara_PuppetNurse:
        case Chara_PuppetDoctor:
            return 57;
        default:
            return WEAPON_ATTACK(EquippedWeaponId_Unk49, AttackInputType_Tap);
    }
}

static void Ar_Blast(AfVec3 at)
{
    VECTOR3 v = Ar_Q12Vec(at);
    int     k;

    func_8008B664(&v, WEAPON_ATTACK(EquippedWeaponId_HuntingRifle, AttackInputType_Tap));
    for (k = 0; k < 6; k++)
        Af_SmokeEmit(&s_smoke, at);
}

static void Ar_HitHarry(const AfMissile* m)
{
    s_SubCharacter*       pl      = &g_SysWork.playerWork.player;
    const s_SubCharacter* shooter = &g_SysWork.npcs[m->shooter];
    q19_12                dmg;

    if (g_Player_DisableDamage || pl->health <= Q12(0.0f))
        return;
    /* Same post-load window func_8008A0E4 refuses hits in. */
    if ((g_Screen_FadeStatus & 0x7) >= ScreenFadeState_FadeInStart)
        return;

    dmg = FP_TO(D_800AD4C8[Ar_EnemyAttack(shooter->model.charaId)].field_4, Q12_SHIFT);
    dmg = Pc_Rando_ScaleWeaponDamage(dmg, 1);
    pl->damage.amount      += dmg;
    pl->damage.position.vx += (s32)(m->dir.x * AR_KNOCK * 4096.0f);
    pl->damage.position.vz += (s32)(m->dir.z * AR_KNOCK * 4096.0f);
    pl->field_40            = m->shooter;
    Chara_AttackReceivedSet(pl, AR_HARRY_HURT);
    SH_DBG("[ARCADE] missile from slot %d hits Harry for %d", m->shooter, (int)dmg);
}

static void Ar_Trail(AfMissile* m, float dt)
{
    m->puffT += dt;
    if (m->puffT >= AR_PUFF_STEP)
    {
        m->puffT = 0.0f;
        Af_SmokeEmit(&s_smoke, m->pos);
    }
}

static void Ar_EnemyLaunches(float dt)
{
    const AfVec3 chest = Ar_Center(&g_SysWork.playerWork.player);
    int          i, k, total = 0;

    for (k = 0; k < AR_MISSILES_MAX; k++)
        if (s_msl[k].alive && !s_msl[k].fromHarry)
            total++;

    for (i = 0; i < NPC_COUNT_MAX; i++)
    {
        s_SubCharacter* npc = &g_SysWork.npcs[i];
        AfMissile*      m;
        AfVec3          from;
        int             own = 0;

        if (s_cool[i] > 0.0f)
            s_cool[i] -= dt;
        if (Pc_FlightHud_LockState(i) != 2)
        {
            s_lockHeld[i] = 0.0f;
            continue;
        }
        s_lockHeld[i] += dt;

        for (k = 0; k < AR_MISSILES_MAX; k++)
            if (s_msl[k].alive && !s_msl[k].fromHarry && s_msl[k].shooter == i)
                own = 1;
        if (!Af_EnemyMayLaunch(s_lockHeld[i], s_cool[i], own, total, Pc_FlightHud_IsBoss(npc->model.charaId)))
            continue;
        if ((m = Ar_FreeSlot()) == NULL)
            return;

        from = Ar_Center(npc);
        Af_MissileInit(m, 0, i, -1, from, Af_Dir(from, chest), AR_ENEMY_SPEED, AR_ENEMY_TURN, AR_ENEMY_LIFE);
        s_cool[i] = AR_ENEMY_COOLDOWN;
        total++;
        func_8005DC1C(Sfx_Unk1286, &npc->position, Q8(0.75f), 0);
        SH_DBG("[ARCADE] slot %d (chara %d) fires", i, npc->model.charaId);
    }
}

static void Ar_Fly(float dt)
{
    const s_SubCharacter* pl    = &g_SysWork.playerWork.player;
    const AfVec3          chest = Ar_Center(pl);
    float                 xyz[AR_FLARES_MAX * 3];
    AfVec3                flares[AR_FLARES_MAX];
    const int             nf = Pc_FlightHud_FlarePositions(xyz, AR_FLARES_MAX);
    int                   i;

    for (i = 0; i < nf; i++)
    {
        flares[i].x = xyz[i * 3 + 0];
        flares[i].y = xyz[i * 3 + 1];
        flares[i].z = xyz[i * 3 + 2];
    }

    for (i = 0; i < AR_MISSILES_MAX; i++)
    {
        AfMissile* m = &s_msl[i];
        AfVec3     aim;
        float      radius, floorY;
        int        step, d;

        if (!m->alive || m->fromHarry)
            continue;

        d = Af_PickDecoy(m->pos, flares, nf, AR_DECOY_RANGE);
        if (d >= 0)
            m->decoyed = 1;
        if (m->decoyed)
        {
            aim.x  = m->pos.x + m->dir.x * 10.0f;
            aim.y  = m->pos.y + m->dir.y * 10.0f;
            aim.z  = m->pos.z + m->dir.z * 10.0f;
            radius = -1.0f;
            if (d >= 0)
            {
                aim    = flares[d];
                radius = AR_HIT_HARRY;
            }
        }
        else
        {
            aim    = chest;
            radius = AR_HIT_HARRY;
        }

        floorY = Ar_Q12f(MAX(pl->position.vy, g_SysWork.npcs[m->shooter].position.vy)) + 0.1f;
        Ar_Trail(m, dt);
        step = Af_MissileStep(m, aim, floorY, radius, dt);
        if (step == AF_STEP_HIT && !m->decoyed)
            Ar_HitHarry(m);
        if (step != AF_STEP_FLYING)
            Ar_Blast(m->pos);
    }
}

int Pc_FlightArcade_Active(void)
{
    return g_PcConfig.flightHud != 0 && g_PcConfig.flightGameplay != 0;
}

int Pc_FlightArcade_ShieldsHarryFrom(const s_SubCharacter* attacker)
{
    return Pc_FlightArcade_Active() && Pc_FlightHud_JamActive() &&
           attacker != &g_SysWork.playerWork.player && !Pc_FlightHud_IsBoss(attacker->model.charaId);
}

void Pc_FlightArcade_Reset(void)
{
    memset(s_msl, 0, sizeof(s_msl));
    memset(&s_smoke, 0, sizeof(s_smoke));
    memset(s_lockHeld, 0, sizeof(s_lockHeld));
    memset(s_cool, 0, sizeof(s_cool));
}

void Pc_FlightArcade_Update(float dt)
{
    if (!Pc_FlightArcade_Active())
    {
        Pc_FlightArcade_Reset();
        return;
    }

    Ar_EnemyLaunches(dt);
    Ar_Fly(dt);
    Af_SmokeStep(&s_smoke, dt);
}

int Pc_FlightArcade_Missiles(const AfMissile** out)
{
    *out = s_msl;
    return AR_MISSILES_MAX;
}

float Pc_FlightArcade_InboundDist(void)
{
    const AfVec3 chest = Ar_Center(&g_SysWork.playerWork.player);
    float        best  = -1.0f;
    int          i;

    for (i = 0; i < AR_MISSILES_MAX; i++)
    {
        const AfMissile* m = &s_msl[i];
        float            d;
        if (!m->alive || m->fromHarry || m->decoyed)
            continue;
        d = Af_Dist(m->pos, chest);
        if (best < 0.0f || d < best)
            best = d;
    }
    return best;
}

int Pc_FlightArcade_Inbound(void)
{
    return Pc_FlightArcade_InboundDist() >= 0.0f;
}
