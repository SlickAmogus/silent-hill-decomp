/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc_flight_missile.h"

#include <math.h>
#include <string.h>

static AfVec3 Af_Add(AfVec3 a, AfVec3 b)  { AfVec3 v = { a.x + b.x, a.y + b.y, a.z + b.z }; return v; }
static AfVec3 Af_Sub(AfVec3 a, AfVec3 b)  { AfVec3 v = { a.x - b.x, a.y - b.y, a.z - b.z }; return v; }
static AfVec3 Af_Scale(AfVec3 a, float k) { AfVec3 v = { a.x * k, a.y * k, a.z * k }; return v; }
static float  Af_Dot(AfVec3 a, AfVec3 b)  { return a.x * b.x + a.y * b.y + a.z * b.z; }
static float  Af_Len(AfVec3 a)            { return sqrtf(Af_Dot(a, a)); }

static AfVec3 Af_Cross(AfVec3 a, AfVec3 b)
{
    AfVec3 v = { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
    return v;
}

static AfVec3 Af_Norm(AfVec3 a)
{
    const float l = Af_Len(a);
    return l > 1e-6f ? Af_Scale(a, 1.0f / l) : a;
}

float Af_Dist(AfVec3 a, AfVec3 b)
{
    return Af_Len(Af_Sub(b, a));
}

AfVec3 Af_Dir(AfVec3 from, AfVec3 to)
{
    return Af_Norm(Af_Sub(to, from));
}

AfVec3 Af_Steer(AfVec3 dir, AfVec3 want, float maxTurn)
{
    float  c = Af_Dot(dir, want);
    AfVec3 perp;

    if (c > 1.0f)  c = 1.0f;
    if (c < -1.0f) c = -1.0f;
    if (acosf(c) <= maxTurn)
        return want;

    perp = Af_Sub(want, Af_Scale(dir, c));
    if (Af_Len(perp) < 1e-5f)
    {
        AfVec3 up = { 0.0f, 1.0f, 0.0f }, side = { 1.0f, 0.0f, 0.0f };
        perp = Af_Cross(dir, fabsf(dir.y) < 0.9f ? up : side);
    }
    perp = Af_Norm(perp);
    return Af_Norm(Af_Add(Af_Scale(dir, cosf(maxTurn)), Af_Scale(perp, sinf(maxTurn))));
}

void Af_MissileInit(AfMissile* m, int fromHarry, int shooter, int target, AfVec3 pos, AfVec3 dir,
                    float speed, float turnRate, float life)
{
    memset(m, 0, sizeof(*m));
    m->alive     = 1;
    m->fromHarry = fromHarry;
    m->shooter   = shooter;
    m->target    = target;
    m->pos       = pos;
    m->dir       = Af_Norm(dir);
    m->speed     = speed;
    m->turnRate  = turnRate;
    m->life      = life;
}

float Af_SegDist(AfVec3 a, AfVec3 b, AfVec3 p)
{
    const AfVec3 ab = Af_Sub(b, a);
    const float  l2 = Af_Dot(ab, ab);
    float        t  = l2 > 1e-9f ? Af_Dot(Af_Sub(p, a), ab) / l2 : 0.0f;

    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return Af_Dist(Af_Add(a, Af_Scale(ab, t)), p);
}

int Af_MissileStep(AfMissile* m, AfVec3 aim, float floorY, float hitRadius, float dt)
{
    AfVec3 prev;

    m->life -= dt;
    if (m->life <= 0.0f)
    {
        m->alive = 0;
        return AF_STEP_EXPIRED;
    }

    if (Af_Dist(m->pos, aim) > 1e-4f)
        m->dir = Af_Steer(m->dir, Af_Dir(m->pos, aim), m->turnRate * dt);
    prev   = m->pos;
    m->pos = Af_Add(m->pos, Af_Scale(m->dir, m->speed * dt));

    /* The swept segment, not the end point: a fast missile on a long frame
     * would otherwise step straight through its target. */
    if (hitRadius > 0.0f && Af_SegDist(prev, m->pos, aim) <= hitRadius)
    {
        m->alive = 0;
        return AF_STEP_HIT;
    }
    if (m->pos.y > floorY)
    {
        m->alive = 0;
        return AF_STEP_GROUND;
    }
    return AF_STEP_FLYING;
}

int Af_GunTick(AfGun* g, int trigger, float dt)
{
    int n;

    if (g->overheated || !trigger)
    {
        g->heat -= dt / AF_GUN_COOL_TIME;
        if (g->heat <= 0.0f)
        {
            g->heat       = 0.0f;
            g->overheated = 0;
        }
        g->firing = 0;
        return 0;
    }

    if (!g->firing)
    {
        g->firing = 1;
        g->shotT  = 1.0f / AF_GUN_RATE;
        g->shotT -= dt;
    }

    g->heat += dt / AF_GUN_HEAT_TIME;
    if (g->heat >= 1.0f)
    {
        g->heat       = 1.0f;
        g->overheated = 1;
        return 0;
    }
    g->shotT += dt;
    n         = (int)(g->shotT * AF_GUN_RATE);
    g->shotT -= (float)n / AF_GUN_RATE;
    return n;
}

int Af_PickDecoy(AfVec3 pos, const AfVec3* flares, int n, float range)
{
    int   i, best = -1;
    float bestD = range;

    for (i = 0; i < n; i++)
    {
        const float d = Af_Dist(pos, flares[i]);
        if (d <= bestD)
        {
            bestD = d;
            best  = i;
        }
    }
    return best;
}

int Af_EnemyMayLaunch(float lockHeldT, float cooldownT, int ownInFlight, int totalInFlight, int isBoss)
{
    return !isBoss && lockHeldT >= AF_ENEMY_LAUNCH_DELAY && cooldownT <= 0.0f && !ownInFlight &&
           totalInFlight < AF_ENEMY_MAX_INFLIGHT;
}

void Af_SmokeEmit(AfSmoke* s, AfVec3 pos)
{
    AfPuff* p = &s->p[s->next];

    p->alive = 1;
    p->pos   = pos;
    p->age   = 0.0f;
    s->next  = (s->next + 1) % AF_SMOKE_MAX;
}

void Af_SmokeStep(AfSmoke* s, float dt)
{
    int i;

    for (i = 0; i < AF_SMOKE_MAX; i++)
    {
        AfPuff* p = &s->p[i];
        if (!p->alive)
            continue;
        p->age += dt;
        if (p->age >= AF_PUFF_LIFE)
        {
            p->alive = 0;
            continue;
        }
        p->pos.y -= AF_PUFF_RISE * dt;
    }
}

float Af_PuffSize(float age)
{
    float k = age / AF_PUFF_LIFE;
    if (k > 1.0f) k = 1.0f;
    return AF_PUFF_SIZE0 + (AF_PUFF_SIZE1 - AF_PUFF_SIZE0) * k;
}

float Af_PuffAlpha(float age)
{
    float k = 1.0f - age / AF_PUFF_LIFE;
    return k < 0.0f ? 0.0f : k;
}
