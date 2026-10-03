#include <assert.h>
#include <math.h>

#include "pc_flight_missile.h"

#define PI 3.14159265f

static int Near(float a, float b, float eps)
{
    return fabsf(a - b) <= eps;
}

static AfVec3 V(float x, float y, float z)
{
    AfVec3 v = { x, y, z };
    return v;
}

static float Angle(AfVec3 a, AfVec3 b)
{
    float c = a.x * b.x + a.y * b.y + a.z * b.z;
    if (c > 1.0f) c = 1.0f;
    if (c < -1.0f) c = -1.0f;
    return acosf(c);
}

static void TestSteer(void)
{
    AfVec3 fwd = V(0, 0, 1), right = V(1, 0, 0), back = V(0, 0, -1), out;

    out = Af_Steer(fwd, Af_Dir(V(0, 0, 0), V(0.1f, 0, 1)), 1.0f);
    assert(Angle(out, Af_Dir(V(0, 0, 0), V(0.1f, 0, 1))) < 1e-4f);

    out = Af_Steer(fwd, right, 0.2f);
    assert(Near(Angle(fwd, out), 0.2f, 1e-4f));
    assert(Near(sqrtf(out.x * out.x + out.y * out.y + out.z * out.z), 1.0f, 1e-4f));
    assert(out.x > 0.0f);

    out = Af_Steer(fwd, back, 0.3f);
    assert(!isnan(out.x) && !isnan(out.y) && !isnan(out.z));
    assert(Near(Angle(fwd, out), 0.3f, 1e-4f));
}

static void TestHitStraight(void)
{
    AfMissile m;
    int i, r = AF_STEP_FLYING;

    Af_MissileInit(&m, 0, 2, -1, V(0, -1, 0), V(0, 0, 1), 10.0f, 2.0f, 4.0f);
    for (i = 0; i < 30 && r == AF_STEP_FLYING; i++)
        r = Af_MissileStep(&m, V(0, -1, 5), 0.0f, 0.5f, 1.0f / 30.0f);
    assert(r == AF_STEP_HIT);
    assert(i <= 16);
    assert(!m.alive);
}

static void TestNoTunneling(void)
{
    AfMissile m;

    Af_MissileInit(&m, 1, -1, 3, V(0, -1, 0), V(0, 0, 1), 100.0f, 0.0f, 4.0f);
    assert(Af_MissileStep(&m, V(0, -1, 3), 0.0f, 0.5f, 0.1f) == AF_STEP_HIT);
}

static int Chase(float turnRate)
{
    AfMissile m;
    AfVec3    t = V(0, -1, 6);
    int       i, r = AF_STEP_FLYING;
    const float dt = 1.0f / 60.0f;

    Af_MissileInit(&m, 0, 1, -1, V(0, -1, 0), V(0, 0, 1), 7.0f, turnRate, 4.0f);
    for (i = 0; i < 400 && r == AF_STEP_FLYING; i++)
    {
        t.x += 5.0f * dt; /* a sidestep at a run */
        r = Af_MissileStep(&m, t, 0.0f, 0.5f, dt);
    }
    return r;
}

static void TestDodge(void)
{
    assert(Chase(0.35f) == AF_STEP_EXPIRED); /* slow turner: a run sideways beats it */
    assert(Chase(10.0f) == AF_STEP_HIT);     /* agile and faster than Harry: it catches him */
}

static void TestGroundAndExpiry(void)
{
    AfMissile m;
    int i, r = AF_STEP_FLYING;

    Af_MissileInit(&m, 0, 1, -1, V(0, -1, 0), V(0, 1, 0), 10.0f, 0.0f, 4.0f);
    for (i = 0; i < 30 && r == AF_STEP_FLYING; i++)
        r = Af_MissileStep(&m, V(0, 5, 0), 0.0f, -1.0f, 1.0f / 30.0f);
    assert(r == AF_STEP_GROUND);

    Af_MissileInit(&m, 0, 1, -1, V(0, -1, 0), V(0, 0, 1), 10.0f, 0.0f, 0.5f);
    r = AF_STEP_FLYING;
    for (i = 0; i < 60 && r == AF_STEP_FLYING; i++)
        r = Af_MissileStep(&m, V(0, -1, 1000), 0.0f, 0.5f, 1.0f / 30.0f);
    assert(r == AF_STEP_EXPIRED);
    assert(i >= 15 && i <= 16);
}

static void TestDecoy(void)
{
    AfVec3 flares[3] = { { 9, 0, 0 }, { 3, 0, 0 }, { 0, 0, 4 } };

    assert(Af_PickDecoy(V(0, 0, 0), flares, 3, 8.0f) == 1);
    assert(Af_PickDecoy(V(0, 0, 0), flares, 3, 2.0f) == -1);
    assert(Af_PickDecoy(V(0, 0, 0), flares, 0, 8.0f) == -1);
}

static void TestLaunchRule(void)
{
    assert(Af_EnemyMayLaunch(AF_ENEMY_LAUNCH_DELAY, 0.0f, 0, 0, 0));
    assert(!Af_EnemyMayLaunch(AF_ENEMY_LAUNCH_DELAY - 0.01f, 0.0f, 0, 0, 0));
    assert(!Af_EnemyMayLaunch(AF_ENEMY_LAUNCH_DELAY, 0.5f, 0, 0, 0));
    assert(!Af_EnemyMayLaunch(AF_ENEMY_LAUNCH_DELAY, 0.0f, 1, 0, 0));
    assert(!Af_EnemyMayLaunch(AF_ENEMY_LAUNCH_DELAY, 0.0f, 0, AF_ENEMY_MAX_INFLIGHT, 0));
    assert(!Af_EnemyMayLaunch(AF_ENEMY_LAUNCH_DELAY, 0.0f, 0, 0, 1));
}

static void TestSmoke(void)
{
    static AfSmoke s;
    int i, alive = 0;

    for (i = 0; i < AF_SMOKE_MAX + 1; i++)
        Af_SmokeEmit(&s, V((float)i, 0, 0));
    assert(s.next == 1);
    assert(s.p[0].pos.x == (float)AF_SMOKE_MAX);

    Af_SmokeStep(&s, 1.0f);
    assert(Near(s.p[0].age, 1.0f, 1e-5f));
    assert(Near(s.p[0].pos.y, -AF_PUFF_RISE, 1e-5f));

    Af_SmokeStep(&s, AF_PUFF_LIFE);
    for (i = 0; i < AF_SMOKE_MAX; i++)
        alive += s.p[i].alive;
    assert(alive == 0);

    assert(Near(Af_PuffSize(0.0f), AF_PUFF_SIZE0, 1e-5f));
    assert(Near(Af_PuffSize(AF_PUFF_LIFE), AF_PUFF_SIZE1, 1e-5f));
    assert(Near(Af_PuffAlpha(0.0f), 1.0f, 1e-5f));
    assert(Near(Af_PuffAlpha(AF_PUFF_LIFE), 0.0f, 1e-5f));
}

int main(void)
{
    (void)PI;
    TestSteer();
    TestHitStraight();
    TestNoTunneling();
    TestDodge();
    TestGroundAndExpiry();
    TestDecoy();
    TestLaunchRule();
    TestSmoke();
    return 0;
}
