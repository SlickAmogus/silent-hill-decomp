/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Missile flight for flight_gameplay, kept free of game headers so the tests
 * can link it alone. Metres and seconds, game axes (+Y is down). */
#ifndef PC_FLIGHT_MISSILE_H
#define PC_FLIGHT_MISSILE_H

#ifdef __cplusplus
extern "C" {
#endif

#define AF_SMOKE_MAX          192
#define AF_PUFF_LIFE          1.6f  /* s */
#define AF_PUFF_SIZE0         0.15f /* m, half size at birth */
#define AF_PUFF_SIZE1         0.6f  /* m, half size at death */
#define AF_PUFF_RISE          0.25f /* m/s */
#define AF_ENEMY_LAUNCH_DELAY 1.2f  /* s a lock must hold before the monster fires */
#define AF_ENEMY_MAX_INFLIGHT 3
#define AF_GUN_RATE           12.0f /* rounds/s */
#define AF_GUN_HEAT_TIME      3.0f  /* s of fire to overheat */
#define AF_GUN_COOL_TIME      2.0f  /* s to cool from full */

enum
{
    AF_STEP_FLYING = 0,
    AF_STEP_HIT,
    AF_STEP_EXPIRED,
    AF_STEP_GROUND
};

typedef struct
{
    float x, y, z;
} AfVec3;

typedef struct
{
    int    alive;
    int    fromHarry; /* 1 Harry's, 0 a monster's */
    int    shooter;   /* npc slot that fired it, -1 for Harry */
    int    target;    /* npc slot for Harry's missiles, -1 for a monster's (always Harry) */
    int    decoyed;   /* a flare took it: it can no longer hit Harry */
    AfVec3 pos;
    AfVec3 dir;       /* unit */
    float  speed;     /* m/s */
    float  turnRate;  /* rad/s */
    float  life;      /* s left */
    float  puffT;     /* s since the last smoke puff */
} AfMissile;

typedef struct
{
    int    alive;
    AfVec3 pos;
    float  age;
} AfPuff;

typedef struct
{
    int    alive;
    AfVec3 pos;
    AfVec3 dir;  /* unit */
    float  life; /* s left */
} AfRound;

typedef struct
{
    float heat;       /* 0..1 */
    int   overheated; /* locked out until heat is back to 0 */
    float shotT;      /* s banked toward the next round */
    int   firing;     /* trigger held last tick */
} AfGun;

typedef struct
{
    AfPuff p[AF_SMOKE_MAX];
    int    next;
} AfSmoke;

float  Af_Dist(AfVec3 a, AfVec3 b);
AfVec3 Af_Dir(AfVec3 from, AfVec3 to);
AfVec3 Af_Steer(AfVec3 dir, AfVec3 want, float maxTurn);
void   Af_MissileInit(AfMissile* m, int fromHarry, int shooter, int target, AfVec3 pos, AfVec3 dir,
                      float speed, float turnRate, float life);
/* hitRadius <= 0: the missile cannot hit, it only flies, expires or grounds. */
int    Af_MissileStep(AfMissile* m, AfVec3 aim, float floorY, float hitRadius, float dt);
float  Af_SegDist(AfVec3 a, AfVec3 b, AfVec3 p);
/* Rounds to fire this tick; the first press fires at once. */
int    Af_GunTick(AfGun* g, int trigger, float dt);
int    Af_PickDecoy(AfVec3 pos, const AfVec3* flares, int n, float range);
int    Af_EnemyMayLaunch(float lockHeldT, float cooldownT, int ownInFlight, int totalInFlight, int isBoss);
void   Af_SmokeEmit(AfSmoke* s, AfVec3 pos);
void   Af_SmokeStep(AfSmoke* s, float dt);
float  Af_PuffSize(float age);
float  Af_PuffAlpha(float age);

#ifdef __cplusplus
}
#endif

#endif /* PC_FLIGHT_MISSILE_H */
