# Flight Gameplay (modalità arcade) — piano di implementazione

> **Per chi esegue:** SUB-SKILL RICHIESTA: superpowers:subagent-driven-development (consigliata) o superpowers:executing-plans, un task alla volta. I passi usano le checkbox (`- [ ]`).

**Obiettivo:** dare effetti di gioco veri all'HUD stile caccia: i flare proteggono Harry, i mostri lanciano missili a ricerca, Harry lancia i suoi con Cerchio a LOCK ON, con scie di fumo nel mondo. Tutto dietro `flight_gameplay` (spento di default).

**Architettura:** un modulo puro senza dipendenze dal gioco (`pc_flight_missile.c`: guida, volo, esche, fumo, regola di lancio) coperto da test ctest; un modulo di collegamento al gioco (`pc_flight_arcade.c`: lanci, danni, input, primitive del fumo nella OT del mondo); `pc_flight_hud.c` espone il suo stato (jam, lock, seeker, flare) e disegna missili, avvisi e contatore MSL. Il danno passa per i campi che leggono i gestori originali (`damage.amount`, `damage.position`, `attackReceived`), così le animazioni di colpo e morte sono quelle PSX.

**Stack:** C (C99), CMake + ctest, PsyCross (GTE/GPU PSX emulati), msys2 per la build.

**Spec:** `pc_port/docs/Flight_HUD_Ideas.md`, sezione "Idee di gameplay" (livelli 1–3, "Missili arcade", "Scie dei missili"). Escluso: livello 4 e "Livelli nuovi" (arena TrenchBroom, piano a parte).

## Vincoli globali

- Opzione `flight_gameplay`: 0 = spento (default), 1 = acceso. Ha effetto solo con `flight_hud` ≠ 0. Con 0 il gioco deve restare identico all'originale PSX: nessun hook può cambiare comportamento quando `Pc_FlightArcade_Active()` è 0.
- Boss esclusi da tutto (né lanciano missili né sono fermati dai flare): Split Head, Floatstinger, Twinfeeler, Bloodsucker, Incubus, Monster Cybil (`Ah_IsBoss`).
- Missili di Harry: tasto = binding **light** del giocatore (Cerchio nei layout di default), solo con seeker chiuso (LOCK ON, arma da fuoco in mira). Con LOCK ON il tasto non accende/spegne la torcia.
- Danno: missile di un mostro = danno dell'attacco normale di quel mostro (`D_800AD4C8[attacco].field_4`), reazione di Harry sempre "colpo al busto" (attacco 43), mai una presa. Missile di Harry = 2 × danno del fucile (attacco `WEAPON_ATTACK(EquippedWeaponId_HuntingRifle, AttackInputType_Tap)`).
- Suoni: solo effetti già nel gioco (`func_8005DC1C`, `func_8008B664`), niente audio nuovo.
- Regole di codice del repo: `SH_DBG(...)` per i log, mai `fprintf(stderr, ...)`; commenti solo per il "perché" non ovvio; niente astrazioni oltre il necessario.
- Branch: `feat/flight-gameplay` da `pc-port`, PR sul fork `mcauzzi`; poi merge `pc-port` → `android-port` (lì un pulsante touch MSL è un follow-up, non in questo piano).
- Limite accettato: i missili non collidono con i muri (solo pavimento, bersaglio e durata). Annotarlo nella doc.

## Focus della review

1. **Torcia a LOCK ON**: con seeker chiuso, Cerchio lancia e la torcia NON cambia stato, anche nel frame in cui il lock si chiude o si apre (stato "latched", Task 4 passo 3).
2. **`flight_gameplay = 0` o `flight_hud = 0`**: nessun missile, nessuno scudo, Cerchio accende la torcia come sempre (Task 2 passo 6, Task 4 passo 6).
3. **Missile in volo al cambio mappa / continue dopo la morte**: sparisce, non colpisce Harry nella stanza nuova (Task 3 passo 5).
4. **Danno durante il fade-in dopo un caricamento o con `g_Player_DisableDamage`**: un missile non colpisce Harry (Task 3 passo 3).
5. **Missile veloce e frame lunghi** (dt fino a 0,1 s): non attraversa il bersaglio senza colpirlo (test di tunneling, Task 1).

---

## Struttura dei file

| File | Azione | Responsabilità |
|---|---|---|
| `pc_port/include/pc_flight_missile.h` | Crea | Tipi e funzioni pure: vettori, missile, fumo, regola di lancio |
| `pc_port/src/pc_flight_missile.c` | Crea | Implementazione pura (solo `<math.h>`, `<string.h>`) |
| `pc_port/tests/pc_flight_missile_test.c` | Crea | Test ctest del modulo puro |
| `pc_port/include/pc_flight_arcade.h` | Crea | API di gioco della modalità arcade |
| `pc_port/src/pc_flight_arcade.c` | Crea | Lanci, volo, danni, input, fumo nella OT |
| `pc_port/src/pc_flight_hud.c` | Modifica | Accessori di stato, chiamata all'update arcade, disegno missili/MSL |
| `pc_port/include/pc_flight_hud.h` | Modifica | Prototipi degli accessori |
| `src/bodyprog/bodyprog_combat_8008A058.c` | Modifica | Scudo dei flare in `func_8008A0E4` e `func_8008B714` |
| `src/bodyprog/events/game_sys_states.c` | Modifica | La torcia non scatta quando l'arcade prende il tasto light |
| `pc_port/include/pc_config.h`, `pc_port/src/pc_config.c`, `pc_port/config.cfg` | Modifica | Opzione `flight_gameplay` |
| `src/screens/options/options.c`, `pc_port/src/pc_quick_options.c` | Modifica | Riga dell'opzione (pagina Controls, menu rapido HUD) |
| `pc_port/CMakeLists.txt` | Modifica | Test `pc_flight_missile_test` |
| `pc_port/tools/flight_hud_preview/harness.c` | Modifica | Include i due moduli nuovi, scena `missile.png` |
| `pc_port/docs/Flight_HUD_Ideas.md` | Modifica | Doc e stato |

`pc_port/src/*.c` è preso da `file(GLOB_RECURSE PC_PORT_SOURCES ...)`: i file nuovi entrano nella build senza toccare CMake (serve solo ri-eseguire `cmake ..`).

## Comandi

```bash
# Build gioco
"C:/msys64/usr/bin/bash.exe" -lc 'cd /k/Repositories/silent-hill-decomp-panda/pc_port/build && cmake .. && cmake --build . 2>&1 | tail -40'

# Test (build separata con BUILD_TESTING, solo il target del test)
"C:/msys64/usr/bin/bash.exe" -lc 'cd /k/Repositories/silent-hill-decomp-panda/pc_port && cmake -S . -B build_test -DBUILD_TESTING=ON >/dev/null && cmake --build build_test --target pc_flight_missile_test 2>&1 | tail -20 && ctest --test-dir build_test -R pc_flight_missile --output-on-failure'
```

Log del gioco: `pc_port/build/SilentHill.log`. Il gioco lo avvia l'utente: i passi "prova in gioco" chiedono all'utente di provare e riportare.

---

### Task 0: Branch

- [ ] **Passo 1:** creare il branch

```bash
cd /k/Repositories/silent-hill-decomp-panda
git fetch origin pc-port
git checkout -b feat/flight-gameplay origin/pc-port
```

---

### Task 1: Modulo puro dei missili + test

**File:**
- Crea: `pc_port/include/pc_flight_missile.h`, `pc_port/src/pc_flight_missile.c`, `pc_port/tests/pc_flight_missile_test.c`
- Modifica: `pc_port/CMakeLists.txt` (blocco `if(BUILD_TESTING)`, dopo `add_test(NAME pc_big_tmd_test ...)`)

**Interfacce:**
- Consuma: niente.
- Produce (usato dai Task 3–6): tutto `pc_flight_missile.h` qui sotto, con questi nomi e tipi esatti.

- [ ] **Passo 1: header**

`pc_port/include/pc_flight_missile.h`:

```c
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
```

- [ ] **Passo 2: test che falliscono**

`pc_port/tests/pc_flight_missile_test.c`:

```c
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
```

Aggiungere in `pc_port/CMakeLists.txt`, dentro `if(BUILD_TESTING)`, dopo `add_test(NAME pc_big_tmd_test ...)`:

```cmake
    # Missile flight for flight_gameplay (pc_flight_missile.c): pure maths, no game headers.
    add_executable(pc_flight_missile_test
        tests/pc_flight_missile_test.c
        src/pc_flight_missile.c)
    target_include_directories(pc_flight_missile_test PRIVATE include)
    if(NOT MSVC)
        target_link_libraries(pc_flight_missile_test PRIVATE m)
    endif()
    add_test(NAME pc_flight_missile_test COMMAND pc_flight_missile_test)
```

- [ ] **Passo 3: eseguire i test, devono fallire**

Comando "Test" sopra. Atteso: errore di link (`undefined reference to Af_Steer` ecc.), perché `pc_flight_missile.c` non esiste ancora. Creare prima un file vuoto `pc_port/src/pc_flight_missile.c` se CMake rifiuta la sorgente mancante.

- [ ] **Passo 4: implementazione**

`pc_port/src/pc_flight_missile.c`:

```c
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

static float Af_SegPointDist(AfVec3 a, AfVec3 b, AfVec3 p)
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
    if (hitRadius > 0.0f && Af_SegPointDist(prev, m->pos, aim) <= hitRadius)
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
```

Nota sul test del fumo: `Af_SmokeStep(&s, 1.0f)` sposta `pos.y` di `-AF_PUFF_RISE` (1 s di salita) e lascia vivo ogni sbuffo (1,0 < 1,6 s); il secondo step porta l'età oltre `AF_PUFF_LIFE` e li uccide tutti.

- [ ] **Passo 5: test di nuovo, devono passare**

Comando "Test". Atteso: `100% tests passed, 0 tests failed out of 1`.
Se fallisce `TestDodge` sul caso `Chase(10.0f)`, non toccare il test per farlo passare: verificare prima `Af_Steer` (il ramo `acosf(c) <= maxTurn`) e la condizione di hit.

- [ ] **Passo 6: commit**

```bash
git add pc_port/include/pc_flight_missile.h pc_port/src/pc_flight_missile.c pc_port/tests/pc_flight_missile_test.c pc_port/CMakeLists.txt
git commit -m "flight_gameplay: pure missile flight, decoys and smoke, with tests"
```

---

### Task 2: Opzione `flight_gameplay` e flare che proteggono Harry (livello 1)

**File:**
- Modifica: `pc_port/include/pc_config.h` (dopo `flightHudCallsigns`, riga ~233), `pc_port/src/pc_config.c` (default riga ~95, parse dopo `flight_hud_callsigns` riga ~1041), `pc_port/config.cfg` (dopo il blocco `flight_hud_callsigns`, riga ~446)
- Modifica: `src/screens/options/options.c` (`PCOPT_C`, prima di `Prev_Page`), `pc_port/src/pc_quick_options.c` (`s_pageHud`, dopo `"flight_hud"`)
- Modifica: `pc_port/src/pc_flight_hud.c`, `pc_port/include/pc_flight_hud.h`
- Crea: `pc_port/include/pc_flight_arcade.h`, `pc_port/src/pc_flight_arcade.c` (scheletro: attivo + scudo + reset/update vuoti)
- Modifica: `src/bodyprog/bodyprog_combat_8008A058.c` (`func_8008A0E4` accanto al blocco `g_DebugNoTarget`, `func_8008B714` in testa)

**Interfacce:**
- Consuma: niente dai task precedenti.
- Produce:
  - `int Pc_FlightHud_JamActive(void)` — 1 mentre `s_jamT > 0`.
  - `int Pc_FlightHud_LockState(int slot)` — 0 nessuno, 1 tracking, 2 lock.
  - `int Pc_FlightHud_SeekerLockedSlot(void)` — slot dell'NPC con seeker chiuso, altrimenti -1.
  - `int Pc_FlightHud_FlarePositions(float* xyz, int max)` — flare vivi, 3 float ciascuno.
  - `int Pc_FlightHud_IsBoss(int charaId)`.
  - `int Pc_FlightArcade_Active(void)`, `int Pc_FlightArcade_ShieldsHarryFrom(const struct _SubCharacter* attacker)`, `void Pc_FlightArcade_Update(float dt)`, `void Pc_FlightArcade_Reset(void)`.

- [ ] **Passo 1: config**

`pc_config.h`, dopo la riga di `flightHudCallsigns`:

```c
    int flightGameplay;        /* 1 = the flight HUD changes the game: flares shield Harry, monsters and Harry fire missiles (config key: flight_gameplay, needs flight_hud) */
```

`pc_config.c`, nei default dopo `.flightHudCallsigns = 0,`:

```c
    .flightGameplay         = 0,
```

e nel parse dopo il ramo `flight_hud_callsigns`:

```c
        else if (strcmp(key, "flight_gameplay") == 0)
        {
            g_PcConfig.flightGameplay = (atoi(value) != 0);
        }
```

`config.cfg`, dopo il blocco di `flight_hud_callsigns`:

```
# Flight HUD gameplay (needs flight_hud on). 0 = off (default): the HUD is
# only a show and the game is the PSX one. 1 = flares stop monster hits for a
# few seconds, monsters that keep their lock fire homing missiles, and with
# the seeker locked the light button (Circle) fires one of Harry's missiles.
flight_gameplay = 0
```

`options.c`, in `PCOPT_C` prima di `Prev_Page` (la pagina ha 10 righe, il tetto è 11):

```c
    { "Flight_Gameplay",   &g_PcConfig.flightGameplay,    "flight_gameplay",        VAL_ONOFF, 2, LBL_ONOFF, NULL, 1, PCK_INT },
```

`pc_quick_options.c`, in `s_pageHud` dopo la riga `"flight_hud"`:

```c
    { ROW_OPT,   "flight_gameplay",      0, NULL },
```

- [ ] **Passo 2: accessori nell'HUD**

`pc_flight_hud.h`, prima di `#ifdef __cplusplus }`:

```c
/* State flight_gameplay reads. */
int Pc_FlightHud_JamActive(void);
int Pc_FlightHud_LockState(int slot);
int Pc_FlightHud_SeekerLockedSlot(void);
int Pc_FlightHud_FlarePositions(float* xyz, int max);
int Pc_FlightHud_IsBoss(int charaId);
```

`pc_flight_hud.c`, subito dopo `Ah_FlareSim` (serve `s_flares`) — e `Ah_IsBoss` è definito più sotto, quindi `Pc_FlightHud_IsBoss` va subito dopo `Ah_IsBoss` (riga ~632):

```c
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
```

```c
int Pc_FlightHud_IsBoss(int charaId)
{
    return Ah_IsBoss(charaId);
}
```

`Pc_FlightHud_SeekerLockedSlot` usa `s_seekSlot`/`s_seekT` definiti prima (righe 151–152): va bene la posizione dopo `Ah_FlareSim`.

- [ ] **Passo 3: scheletro arcade**

`pc_port/include/pc_flight_arcade.h`:

```c
/* SPDX-License-Identifier: GPL-3.0-or-later */
/* What the flight HUD does to the game (config key: flight_gameplay). */
#ifndef PC_FLIGHT_ARCADE_H
#define PC_FLIGHT_ARCADE_H

#include "pc_flight_missile.h"

#ifdef __cplusplus
extern "C" {
#endif

struct _SubCharacter;

int   Pc_FlightArcade_Active(void);
int   Pc_FlightArcade_ShieldsHarryFrom(const struct _SubCharacter* attacker);
int   Pc_FlightArcade_ClaimsLightButton(void);
void  Pc_FlightArcade_Update(float dt);
void  Pc_FlightArcade_Reset(void);

int   Pc_FlightArcade_Missiles(const AfMissile** out);
int   Pc_FlightArcade_Inbound(void);
float Pc_FlightArcade_InboundDist(void);
int   Pc_FlightArcade_Stock(void);
float Pc_FlightArcade_LaunchMsgT(void);
float Pc_FlightArcade_NoMslT(void);

#ifdef __cplusplus
}
#endif

#endif /* PC_FLIGHT_ARCADE_H */
```

`pc_port/src/pc_flight_arcade.c` (in questo task solo ciò che serve al livello 1; i Task 3–6 lo completano):

```c
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

#include <string.h>

#include "sh_log.h"
#include "pc_config.h"
#include "pc_flight_hud.h"
#include "pc_flight_missile.h"
#include "pc_flight_arcade.h"

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
}

void Pc_FlightArcade_Update(float dt)
{
    (void)dt;
    if (!Pc_FlightArcade_Active())
        Pc_FlightArcade_Reset();
}
```

Gli altri prototipi dell'header (`ClaimsLightButton`, `Missiles`, ...) arrivano nei Task 3–5: finché nessuno li chiama la build non si rompe.

`pc_flight_hud.c`: `#include "pc_flight_arcade.h"` dopo `#include "pc_flight_hud.h"`, poi in `Pc_FlightHud_Update`:
- nel ramo `if (!Pc_FlightHud_Enabled())`, dopo `s_flareReq = 0;`: `Pc_FlightArcade_Reset();`
- dopo `Ah_LockScan(dt);`: `Pc_FlightArcade_Update(dt);`

- [ ] **Passo 4: scudo nei due punti di colpo**

`src/bodyprog/bodyprog_combat_8008A058.c`, in `func_8008A0E4`, dentro il blocco `#ifdef SH_PC_PORT` subito dopo quello di `g_DebugNoTarget` (stessa motivazione: è l'unico punto da cui passano gli attacchi dei mostri):

```c
    /* flight_gameplay: a flare salvo blinds the monsters' seekers, and their
     * swings with it. */
    {
        extern int Pc_FlightArcade_ShieldsHarryFrom(const s_SubCharacter* attacker);
        if (chara2 == &g_SysWork.playerWork.player && Pc_FlightArcade_ShieldsHarryFrom(chara))
        {
            return NO_VALUE;
        }
    }
```

In `func_8008B714`, prima di `weaponAttack = (u8)attacker->field_44.field_2;`:

```c
#ifdef SH_PC_PORT
    /* The Air Screamer reaches Harry through func_8008A3E0 without ever
     * calling func_8008A0E4, so the flare shield is checked here too. */
    {
        extern int Pc_FlightArcade_ShieldsHarryFrom(const s_SubCharacter* attacker);
        if (target == &g_SysWork.playerWork.player && Pc_FlightArcade_ShieldsHarryFrom(attacker))
        {
            return 0;
        }
    }
#endif
```

(`func_8008B714` restituisce `sp10`, che per Harry vale -1: 0 significa "nessun colpo" per entrambi i chiamanti, righe ~821 e ~2353.)

- [ ] **Passo 5: build**

Comando "Build gioco". Atteso: nessun errore, nessun warning nuovo nei file toccati.

- [ ] **Passo 6: prova in gioco (utente)**

Chiedere all'utente, con `flight_hud = 1`:
1. `flight_gameplay = 0`: Air Screamer al bar e un Groaner in città colpiscono Harry come sempre anche subito dopo i flare (L3+R3). Atteso: identico a prima.
2. `flight_gameplay = 1`: lanciare i flare quando un mostro sta per colpire. Atteso: per 3 s i colpi non fanno danno né animazione di colpo; dopo 3 s tornano.
3. Una boss fight (Split Head): i flare non cambiano nulla.

- [ ] **Passo 7: commit**

```bash
git add pc_port/include/pc_config.h pc_port/src/pc_config.c pc_port/config.cfg src/screens/options/options.c pc_port/src/pc_quick_options.c pc_port/src/pc_flight_hud.c pc_port/include/pc_flight_hud.h pc_port/include/pc_flight_arcade.h pc_port/src/pc_flight_arcade.c src/bodyprog/bodyprog_combat_8008A058.c
git commit -m "flight_gameplay: option, and flares stop monster hits while they jam"
```

---

### Task 3: Missili dei mostri (livello 2)

**File:**
- Modifica: `pc_port/src/pc_flight_arcade.c`
- Modifica: `pc_port/src/pc_flight_hud.c` (`Ah_ZoneTick`: reset al cambio mappa; `Ah_LockScan`: il missile in arrivo è il terzo livello d'allarme)

**Interfacce:**
- Consuma: Task 1 (`Af_*`), Task 2 (`Pc_FlightHud_LockState`, `Pc_FlightHud_FlarePositions`, `Pc_FlightHud_IsBoss`).
- Produce: `Pc_FlightArcade_Missiles`, `Pc_FlightArcade_Inbound`, `Pc_FlightArcade_InboundDist` (usati dal Task 5); `s_smoke` e `Ar_Trail` (usati dal Task 6).

- [ ] **Passo 1: include, costanti e stato**

In `pc_flight_arcade.c` sostituire il blocco include con:

```c
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
```

(Se `func_8008B664` non è dichiarata in `bodyprog/bodyprog.h`, aggiungere `void func_8008B664(VECTOR3* pos, u32 caseVar);` accanto a `func_8005DC1C`.)

Sotto gli include:

```c
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
```


- [ ] **Passo 2: attacco normale per mostro**

```c
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
```

Fonti: `air_screamer.c:13204` (40), `groaner.c:788`, `creeper.c:583`, `hanged_scratcher.c:541`, `larval_stalker.c:467`, `romper.c:1082`, commento `[PLRDMG]` in `bodyprog_combat_8008A058.c` (infermiera 57/58), `stalker.c:1599` (default).

- [ ] **Passo 3: colpo su Harry, esplosione, scia**

```c
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
```

`damage.position.vy` non va toccato: per Harry è usato come angolo (`unknown23.c:372`, `player_control.c` ~10300).

- [ ] **Passo 4: lancio e volo**

```c
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
        int        step;

        if (!m->alive || m->fromHarry)
            continue;

        {
            const int d = Af_PickDecoy(m->pos, flares, nf, AR_DECOY_RANGE);
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
```

(`MAX` è la macro del progetto, già usata in `groaner.c:183`. In Y-giù il pavimento più basso è il valore più grande.)

- [ ] **Passo 5: reset, update, accessori**

Sostituire `Pc_FlightArcade_Reset` e `Pc_FlightArcade_Update` dello scheletro con:

```c
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
```

In `pc_flight_hud.c`:
- `Ah_ZoneTick`, dentro `if (map != s_prevMap)` come prima istruzione: `Pc_FlightArcade_Reset();` (copre cambio stanza, caricamento e continue dopo la morte).
- `Ah_LockScan`, ultima riga: sostituire `s_alert = s_anyLock;` con

```c
    /* A missile already flying keeps the alarm up after the lock that fired
     * it is gone. */
    if (Pc_FlightArcade_Inbound())
        s_danger = 1;
    s_alert = s_anyLock || s_danger;
```

Così MISSILE ALERT, EVADE, bordi rossi e sirena seguono il missile senza altro codice (`Ah_Tones` e `Ah_DangerEdge` leggono già `s_danger`). L'EVADE disegnato dentro `if (s_alert ...)` resta corretto perché ora `s_alert` è vero.

- [ ] **Passo 6: build**

Comando "Build gioco". Atteso: nessun errore.

- [ ] **Passo 7: prova in gioco (utente)**

Con `flight_gameplay = 1`:
1. Restare davanti a un Groaner finché fa lock (MISSILE ALERT). Atteso: dopo ~1,2 s di lock parte un missile (suono d'impatto come "sparo", log `[ARCADE] slot N ... fires`), sirena EVADE; se colpisce, Harry fa l'animazione di colpo al busto e perde salute (log `[ARCADE] missile ... hits Harry`).
2. Correre di lato appena parte. Atteso: il missile si può schivare (esplode a terra o scade dopo 4 s).
3. Missile in volo → flare. Atteso: il missile va verso i flare ed esplode lì, niente danno; la sirena si ferma.
4. Missile in volo → uscire dalla stanza. Atteso: nella stanza nuova niente missile né allarme.
5. Boss: nessun missile.
6. Dopo un caricamento, nessun colpo di missile durante il fade-in.

- [ ] **Passo 8: commit**

```bash
git add pc_port/src/pc_flight_arcade.c pc_port/src/pc_flight_hud.c
git commit -m "flight_gameplay: monsters holding a lock fire homing missiles; flares decoy them"
```

---

### Task 4: Missili di Harry con Cerchio (livello 3)

**File:**
- Modifica: `pc_port/src/pc_flight_arcade.c`
- Modifica: `src/bodyprog/events/game_sys_states.c` (toggle della torcia, riga ~491)

**Interfacce:**
- Consuma: Task 1, Task 2 (`Pc_FlightHud_SeekerLockedSlot`), Task 3 (`Ar_Center`, `Ar_FreeSlot`, `Ar_Trail`, `Ar_Blast`, `Ar_Q12Vec`).
- Produce: `Pc_FlightArcade_ClaimsLightButton`, `Pc_FlightArcade_Stock`, `Pc_FlightArcade_LaunchMsgT`, `Pc_FlightArcade_NoMslT`.

- [ ] **Passo 1: costanti e stato**

```c
#define AR_HARRY_SPEED     16.0f
#define AR_HARRY_TURN      4.0f   /* rad/s */
#define AR_HARRY_LIFE      3.0f
#define AR_HARRY_MAX       2
#define AR_HARRY_RECHARGE  12.0f  /* s per missile */
#define AR_HARRY_DMG_MULT  2      /* a missile hits like two rifle rounds */
#define AR_HIT_NPC         0.8f   /* m */

static int   s_mslStock = AR_HARRY_MAX;
static float s_mslRechargeT;
static float s_launchMsgT, s_noMslT;
static int   s_claim;

extern int g_PcConsoleInputActive;
```

e `#include "pc_quick_options.h"` (per `g_PcQuickOptionsActive`) fra gli include.

- [ ] **Passo 2: colpo su un mostro e volo dei missili di Harry**

```c
static void Ar_HitNpc(const AfMissile* m)
{
    s_SubCharacter* npc = &g_SysWork.npcs[m->target];
    const s32       wa  = WEAPON_ATTACK(EquippedWeaponId_HuntingRifle, AttackInputType_Tap);
    q19_12          dmg;

    if (npc->health <= Q12(0.0f))
        return;
    dmg = FP_TO(D_800AD4C8[wa].field_4, Q12_SHIFT) * AR_HARRY_DMG_MULT;
    dmg = Pc_Rando_ScaleWeaponDamage(dmg, 0);
    npc->damage.amount      += dmg;
    npc->damage.position.vx += (s32)(m->dir.x * AR_KNOCK * 4096.0f);
    npc->damage.position.vz += (s32)(m->dir.z * AR_KNOCK * 4096.0f);
    Chara_AttackReceivedSet(npc, wa);
    SH_DBG("[ARCADE] Harry's missile hits slot %d for %d", m->target, (int)dmg);
}

static void Ar_FlyHarry(float dt)
{
    const s_SubCharacter* pl = &g_SysWork.playerWork.player;
    int                   i;

    for (i = 0; i < AR_MISSILES_MAX; i++)
    {
        AfMissile*            m = &s_msl[i];
        const s_SubCharacter* npc;
        AfVec3                aim;
        float                 radius = AR_HIT_NPC;
        int                   step;

        if (!m->alive || !m->fromHarry)
            continue;

        npc = &g_SysWork.npcs[m->target];
        if (npc->health > Q12(0.0f))
        {
            aim = Ar_Center(npc);
        }
        else
        {
            aim.x  = m->pos.x + m->dir.x * 10.0f;
            aim.y  = m->pos.y + m->dir.y * 10.0f;
            aim.z  = m->pos.z + m->dir.z * 10.0f;
            radius = -1.0f;
        }

        Ar_Trail(m, dt);
        step = Af_MissileStep(m, aim, Ar_Q12f(MAX(pl->position.vy, npc->position.vy)) + 0.1f, radius, dt);
        if (step == AF_STEP_HIT)
            Ar_HitNpc(m);
        if (step != AF_STEP_FLYING)
            Ar_Blast(m->pos);
    }
}
```

- [ ] **Passo 3: lancio con il tasto light, latch del "claim"**

```c
static void Ar_HarryLaunch(int claimed)
{
    const s_SubCharacter* pl = &g_SysWork.playerWork.player;
    const u16             light = g_GameWorkPtr->config.controllerConfig.light;
    AfMissile*            m;
    AfVec3                from, to;
    int                   slot;

    if (!claimed || !(g_Controller0->clickedBtnFlags & light) || g_PcConsoleInputActive || g_PcQuickOptionsActive)
        return;

    slot = Pc_FlightHud_SeekerLockedSlot();
    if (slot < 0)
        return;
    if (s_mslStock <= 0 || (m = Ar_FreeSlot()) == NULL)
    {
        s_noMslT = 1.2f;
        SD_Call(Sfx_MenuError);
        return;
    }

    s_mslStock--;
    from = Ar_Center(pl);
    to   = Ar_Center(&g_SysWork.npcs[slot]);
    Af_MissileInit(m, 1, -1, slot, from, Af_Dir(from, to), AR_HARRY_SPEED, AR_HARRY_TURN, AR_HARRY_LIFE);
    s_launchMsgT = 1.0f;
    func_8005DC1C(Sfx_Unk1286, &pl->position, Q8(0.75f), 0);
    SH_DBG("[ARCADE] Harry fires at slot %d, %d left", slot, s_mslStock);
}

int Pc_FlightArcade_ClaimsLightButton(void)
{
    return Pc_FlightArcade_Active() && s_claim;
}
```

Aggiornare `Pc_FlightArcade_Update`:

```c
void Pc_FlightArcade_Update(float dt)
{
    /* SysState_Gameplay_Update gates the flashlight on s_claim earlier in this
     * same frame, so the launch uses that value and only then recomputes it:
     * one click is either a missile or the light, never both, never neither. */
    const int claimed = s_claim;

    if (!Pc_FlightArcade_Active())
    {
        Pc_FlightArcade_Reset();
        return;
    }

    if (s_mslStock < AR_HARRY_MAX)
    {
        s_mslRechargeT += dt;
        if (s_mslRechargeT >= AR_HARRY_RECHARGE)
        {
            s_mslRechargeT = 0.0f;
            s_mslStock++;
        }
    }
    else
    {
        s_mslRechargeT = 0.0f;
    }
    if (s_launchMsgT > 0.0f) s_launchMsgT -= dt;
    if (s_noMslT > 0.0f)     s_noMslT -= dt;

    Ar_HarryLaunch(claimed);
    Ar_EnemyLaunches(dt);
    Ar_Fly(dt);
    Ar_FlyHarry(dt);
    Af_SmokeStep(&s_smoke, dt);

    s_claim = Pc_FlightHud_SeekerLockedSlot() >= 0;
}
```

In `Pc_FlightArcade_Reset` aggiungere `s_claim = 0; s_launchMsgT = s_noMslT = 0.0f;` (lo stock non si azzera al cambio mappa, come i flare).

Accessori:

```c
int Pc_FlightArcade_Stock(void)        { return s_mslStock; }
float Pc_FlightArcade_LaunchMsgT(void) { return s_launchMsgT; }
float Pc_FlightArcade_NoMslT(void)     { return s_noMslT; }
```

- [ ] **Passo 4: la torcia non scatta col claim**

`src/bodyprog/events/game_sys_states.c`, in `SysState_Gameplay_Update` (riga ~491):

```c
    if (g_Controller0->clickedBtnFlags & g_GameWorkPtr->config.controllerConfig.light &&
        g_SysWork.field_2388.field_154.effectsInfo_0.field_0.s_field_0.field_0 & (1 << 1)
#ifdef SH_PC_PORT
        && !Pc_FlightArcade_ClaimsLightButton()
#endif
        )
    {
        Game_FlashlightToggle();
    }
```

con, sopra la funzione o nel blocco `#ifdef SH_PC_PORT` più vicino: `extern int Pc_FlightArcade_ClaimsLightButton(void);`.

Verificato: `SysState_Gameplay_Update` gira dentro `GameState_InGame_Update` (dispatch riga ~176) **prima** di `Pc_FlightHud_Update` (riga ~281) nello stesso frame: per questo il latch del passo 3 funziona.

- [ ] **Passo 5: build**

Comando "Build gioco".

- [ ] **Passo 6: prova in gioco (utente)**

Con `flight_gameplay = 1`, pistola in mano:
1. Mirare un mostro finché compare LOCK ON, premere Cerchio (V). Atteso: parte un missile, la torcia non cambia stato, log `[ARCADE] Harry fires`; all'impatto il mostro reagisce come a un colpo di fucile, compaiono HIT o DESTROYED e il punteggio.
2. Premere Cerchio senza LOCK ON (o senza mirare). Atteso: la torcia si accende/spegne come sempre.
3. Lanciare 3 volte di fila. Atteso: il terzo dà il suono d'errore (nessun missile); dopo 12 s ne torna uno.
4. `flight_gameplay = 0`: Cerchio con LOCK ON accende/spegne la torcia, nessun missile.
5. Premere Cerchio esattamente mentre il rombo del seeker si chiude, più volte. Atteso: ogni pressione dà o un missile o la torcia, mai entrambi.

- [ ] **Passo 7: commit**

```bash
git add pc_port/src/pc_flight_arcade.c src/bodyprog/events/game_sys_states.c
git commit -m "flight_gameplay: Circle at LOCK ON fires Harry's homing missiles"
```

---

### Task 5: HUD dei missili

**File:**
- Modifica: `pc_port/src/pc_flight_hud.c`
- Modifica: `pc_port/tools/flight_hud_preview/harness.c`

**Interfacce:**
- Consuma: `Pc_FlightArcade_Missiles`, `Pc_FlightArcade_Active`, `Pc_FlightArcade_Stock`, `Pc_FlightArcade_LaunchMsgT`, `Pc_FlightArcade_NoMslT` (Task 3–4).
- Produce: niente per altri task.

- [ ] **Passo 1: bagliore sulla testa dei missili (batch additivo)**

Dopo `Ah_BuildFlares`:

```c
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
```

In `Pc_FlightHud_Draw`, dopo `Ah_BuildFlares();` (ancora con `s_cur = &s_glow`): `Ah_BuildMissiles();`.

- [ ] **Passo 2: rombo sul missile in arrivo**

```c
/* Blinks faster as the missile closes: 0.5 s at 12 m, 0.08 s at contact. */
static void Ah_MissileMarks(float nowS)
{
    const AfMissile* m;
    const int        n = Pc_FlightArcade_Missiles(&m);
    const AfVec3     me = { Ah_Q12f(g_SysWork.playerWork.player.position.vx),
                            Ah_Q12f(g_SysWork.playerWork.player.position.vy),
                            Ah_Q12f(g_SysWork.playerWork.player.position.vz) };
    int              i;

    for (i = 0; i < n; i++)
    {
        float hx, hy, depth, k, period;
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
```

Chiamarla in `Ah_BuildHud` e in `Ah_BuildHudClassic` subito prima di `Ah_Events(...)`.

- [ ] **Passo 3: contatore MSL e messaggi**

Modern, ramo non-touch (righe ~2849–2868): quando `Pc_FlightArcade_Active()`, inserire una riga MSL a y=128 e spostare DMG a 144 e la sagoma a 164; altrimenti layout invariato:

```c
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
            }

            Ah_UseMain();
            Ah_Text("DMG", colL, dmgY, size, 0);
            snprintf(buf, sizeof(buf), "%d%%", (int)(dmg + 0.5f));
            Ah_Text(buf, colR, dmgY, size, 2);

            Ah_HealthColor(hp, nowS);
            Ah_Silhouette(colR - 45.0f, dmgY + 20.0f, 72.0f);
        }
```

(sostituisce le righe originali di DMG e `Ah_Silhouette`).

Modern, ramo touch: se `Pc_FlightArcade_Active()`, la stringa diventa `"DMG %d%%  %s %s  FLR %d  MSL %d"` con `Pc_FlightArcade_Stock()` in coda; la barra di ricarica dei flare resta calcolata su `"FLR 4"` ma va spostata a sinistra della larghezza di `"  MSL 2"`: `Ah_RechargeBar(10.0f + w * 0.5f - fw - Ah_TextWidth("  MSL 2", 8.0f), 224.0f, fw, 4.0f);`.

Classic: in `Ah_BuildHudClassic`, accanto a `Ah_FlareLine(Ah_TextWidth("FLR 4", 8.0f) * 0.5f, 194.0f, 8.0f);`:

```c
        if (Pc_FlightArcade_Active())
        {
            char msl[16];
            snprintf(msl, sizeof(msl), "MSL %d", Pc_FlightArcade_Stock());
            if (Pc_FlightArcade_LaunchMsgT() > 0.0f)
                Ah_UseHi();
            else
                Ah_UseMain();
            Ah_Text(msl, Ah_TextWidth("FLR 4", 8.0f) * 0.5f, 178.0f, 8.0f, 2);
        }
```

Messaggi, in entrambi gli stili dopo il blocco `FLARE` / `NO FLARES` (Modern y=86, Classic y=56):

```c
    if (Pc_FlightArcade_NoMslT() > 0.0f)
    {
        Ah_UseMain();
        Ah_Text("NO MISSILES", 0.0f, 86.0f, 10.0f, 1);
    }
```

Tutti i glifi usati (lettere maiuscole, cifre, spazio, `%`) esistono già in `s_glyph`.

- [ ] **Passo 4: anteprima senza gioco**

`pc_port/tools/flight_hud_preview/harness.c`: accanto all'include di `pc_flight_hud.c` aggiungere

```c
#include "../../src/pc_flight_missile.c"
#include "../../src/pc_flight_arcade.c"
```

(lo stesso TU vede gli `static` di `pc_flight_arcade.c`). Aggiungere una scena prima della fine di `main`, sul modello delle altre (stesso modo di posare Harry e i nemici e di scrivere il PNG):

```c
    /* missile.png: one inbound monster missile, one of Harry's, MSL counter. */
    g_PcConfig.flightGameplay = 1;
    Af_MissileInit(&s_msl[0], 0, 0, -1, (AfVec3){ px + 2.0f, py - 1.2f, pz + 5.0f }, (AfVec3){ 0, 0, -1 }, 7.0f, 1.2f, 4.0f);
    Af_MissileInit(&s_msl[1], 1, -1, 1, (AfVec3){ px - 1.0f, py - 1.0f, pz + 3.0f }, (AfVec3){ 0, 0, 1 }, 16.0f, 4.0f, 3.0f);
    s_mslStock = 1;
    /* render and save as "missile" with the same helper the other scenes use */
    g_PcConfig.flightGameplay = 0;
    memset(s_msl, 0, sizeof(s_msl));
```

dove `px, py, pz` sono la posizione di Harry che la scena usa (in metri); adattare al nome reale delle variabili del harness e alla funzione che salva le scene.

Eseguire l'anteprima (sezione "Anteprima senza il gioco" della doc). Atteso: `missile.png` mostra un bagliore arancio con rombo rosso, uno verde, `MSL 1` sotto `FLR`, e le altre immagini identiche a prima.

- [ ] **Passo 5: build e prova in gioco (utente)**

Comando "Build gioco". Poi chiedere all'utente: missili visibili da lontano (bagliore), rombo rosso che lampeggia più veloce man mano che il missile si avvicina, MSL che scende e risale, NO MISSILES a stock vuoto; in Classic e Modern; con `flight_gameplay = 0` il pannello in basso a destra è identico a prima.

- [ ] **Passo 6: commit**

```bash
git add pc_port/src/pc_flight_hud.c pc_port/tools/flight_hud_preview/harness.c
git commit -m "flight_hud: missile glow, inbound marker and MSL counter"
```

---

### Task 6: Scie di fumo nella OT del mondo

**File:**
- Modifica: `pc_port/src/pc_flight_arcade.c`

**Interfacce:**
- Consuma: `s_smoke` e `Ar_Trail` (Task 3), `Af_PuffSize`, `Af_PuffAlpha` (Task 1), `Vw_WorldScreenMatrixAtPositionGet` (`bodyprog/view/vw_calc.h`), `g_OrderingTable0`, `g_ActiveBufferIdx`, `GsOUT_PACKET_P`.
- Produce: niente.

Il fumo è un quad semitrasparente per sbuffo, messo nella OT del mondo alla sua profondità: muri e personaggi davanti lo coprono, e la nebbia lo spegne con il valore di depth-cue `p` che la GTE restituisce. Additivo (abr 1) perché il modo 50/50 non può sbiadire a zero; con colori bassi resta un fumo chiaro, leggibile nel buio di Silent Hill. `Pc_FlightHud_Update` gira nel loop di gioco dopo `updateWorldObjects`, quando la OT del frame è già aperta: è lo stesso momento in cui il gioco aggiunge le sue primitive (`map_effects.c`, fiamma dell'accendino).

- [ ] **Passo 1: disegno**

Include aggiuntivi in `pc_flight_arcade.c`:

```c
#include "bodyprog/view/vw_calc.h"
#include "bodyprog/screen/screen_data.h"
#include <libgte.h>
#include <libgpu.h>
#include <libgs.h>

extern long ReadGeomScreen(void);
```

Funzione:

```c
#define AR_PUFF_SHADE 72 /* additive: low, so it reads as smoke, not light */

static void Ar_SmokeDraw(void)
{
    GsOT*      ot    = &g_OrderingTable0[g_ActiveBufferIdx];
    const long otLen = 1L << ot->length;
    const long h     = ReadGeomScreen();
    int        i;

    for (i = 0; i < AF_SMOKE_MAX; i++)
    {
        const AfPuff* p = &s_smoke.p[i];
        MATRIX        mat;
        SVECTOR       zero = { 0, 0, 0, 0 };
        long          sxy, depthCue, flag, otz, idx, half, shade;
        short         sx, sy;
        POLY_F4*      poly;
        DR_TPAGE*     tp;

        if (!p->alive)
            continue;

        Vw_WorldScreenMatrixAtPositionGet(&mat, (q19_12)(p->pos.x * 4096.0f), (q19_12)(p->pos.y * 4096.0f),
                                          (q19_12)(p->pos.z * 4096.0f));
        SetRotMatrix(&mat);
        SetTransMatrix(&mat);
        otz = RotTransPers(&zero, &sxy, &depthCue, &flag);
        if (otz <= 0)
            continue;
        idx = (otz >> 1) - 2;
        if (idx < 1 || idx >= otLen)
            continue;

        /* RotTransPers gives OTZ = SZ / 4; a puff of half size s metres is
         * s * 256 GTE units, so h * s * 256 / SZ pixels. */
        half = (long)(Af_PuffSize(p->age) * 256.0f * (float)h / (float)(otz * 4));
        if (half < 1)  half = 1;
        if (half > 96) half = 96;
        shade = (long)(Af_PuffAlpha(p->age) * AR_PUFF_SHADE * (float)(4096 - depthCue) / 4096.0f);
        if (shade <= 0)
            continue;

        sx = (short)(sxy & 0xFFFF);
        sy = (short)(sxy >> 16);

        poly = (POLY_F4*)GsOUT_PACKET_P;
        setPolyF4(poly);
        setSemiTrans(poly, 1);
        setRGB0(poly, shade, shade, shade);
        setXY4(poly, sx - half, sy - half, sx + half, sy - half, sx - half, sy + half, sx + half, sy + half);
        AddPrim(&ot->org[idx], poly);

        /* AddPrim puts each prim at the head of the slot's list, so the tpage
         * added after the poly runs before it. */
        tp = (DR_TPAGE*)(poly + 1);
        setDrawTpage(tp, 0, 1, getTPage(0, 1, 0, 0));
        AddPrim(&ot->org[idx], tp);
        GsOUT_PACKET_P = (PACKET*)(tp + 1);
    }
}
```

Note per l'implementatore:
- Confrontare con `func_8003E740` in `src/bodyprog/gfx/map_effects.c` (righe ~127–273): stesso uso di `GsOUT_PACKET_P`, `RotTransPers`, `AddPrim` su `g_OrderingTable0[g_ActiveBufferIdx].org[...]`. Se lì l'indice OT è calcolato in modo diverso da `(otz >> 1) - 2`, usare lo stesso calcolo della fiamma.
- `RotTransPers` vuole `long*` per sxy: controllare il prototipo in PsyCross (`libgte.h`) e, se è `DVECTOR*`-like, adattare la lettura di `sx`/`sy`.
- Se `GsOT` in PsyCross non ha `length`, usare `ORDERING_TABLE_SIZE` (`screen_data.h`).

In `Pc_FlightArcade_Update`, dopo `Af_SmokeStep(&s_smoke, dt);`: `Ar_SmokeDraw();`.

- [ ] **Passo 2: build**

Comando "Build gioco".

- [ ] **Passo 3: prova in gioco (utente)**

1. Missile di un mostro e di Harry: scia di fumo chiaro che si allarga e sbiadisce in ~1,6 s.
2. La scia dietro un muro o dietro un personaggio non si vede (è nella scena, non sopra).
3. In nebbia fitta la scia lontana sparisce come il resto della scena.
4. Esplosione: piccolo sbuffo nel punto d'impatto.
5. Nessun crash o sfarfallio con 3 missili nemici + 2 di Harry in aria (OT/packet buffer).

- [ ] **Passo 4: commit**

```bash
git add pc_port/src/pc_flight_arcade.c
git commit -m "flight_gameplay: missile smoke trails in the world ordering table"
```

---

### Task 7: Documentazione e giro di prova completo

**File:**
- Modifica: `pc_port/docs/Flight_HUD_Ideas.md`

- [ ] **Passo 1: doc**

In `Flight_HUD_Ideas.md`:
- "Opzioni": riga `flight_gameplay` — 0 spento (default), 1 effetti di gioco; richiede `flight_hud`. Nelle Opzioni pagina Controls ("Flight_Gameplay") e nel menu rapido, pagina HUD.
- Nuova sezione "Modalità arcade (`flight_gameplay`)" sotto "Cosa vedi in gioco": flare = 3 s senza colpi dai mostri (boss esclusi); missili dei mostri dopo 1,2 s di lock, cooldown 6 s, max 3 in aria, schivabili correndo di lato, deviati dai flare; Cerchio (tasto torcia) a LOCK ON lancia un missile di Harry (2 in stock, +1 ogni 12 s), senza LOCK ON Cerchio resta la torcia; MSL accanto a FLR; NO MISSILES; rombo rosso sul missile in arrivo; scie di fumo; limite noto: i missili attraversano i muri.
- "Idee di gameplay": livelli 1–3 e "Scie dei missili" segnati `[~]` con il commit.
- "Stato": righe 19 (flare con effetto), 20 (missili dei mostri), 21 (missili di Harry), 22 (scie) a `[~]`.
- "Da provare in gioco": le voci dei passi "prova in gioco" dei Task 2–6.
- "Dove sta il codice": `pc_flight_missile.c` (puro, test `pc_flight_missile_test`), `pc_flight_arcade.c` (lanci, danni, input, fumo nella OT), hook in `func_8008A0E4` / `func_8008B714` e in `SysState_Gameplay_Update`.
- Follow-up per `android-port`: pulsante touch MSL accanto a `TB_FLARE`.

- [ ] **Passo 2: verifica finale**

1. Comando "Test": passa.
2. Comando "Build gioco": nessun errore.
3. Chiedere all'utente un giro breve con `flight_gameplay = 0` (bar → città): nessuna differenza dal comportamento attuale.

- [ ] **Passo 3: commit e PR**

```bash
git add pc_port/docs/Flight_HUD_Ideas.md
git commit -m "docs: flight_gameplay arcade mode"
git push -u origin feat/flight-gameplay
gh pr create --repo mcauzzi/silent-hill-decomp --base pc-port --title "Flight HUD gameplay: flares, homing missiles, smoke trails" --body "..."
```

(Push e PR solo dopo conferma dell'utente; il corpo della PR riassume i Task 1–6 e la lista "Da provare in gioco".)
