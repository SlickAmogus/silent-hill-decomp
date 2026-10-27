/*
 * split_npc_xbox.c - monsters against two players.
 *
 * Enemy AI reads "the player" straight out of g_SysWork (hundreds of sites in
 * src/maps/characters), and every damage and grab path tests only that slot. So
 * instead of teaching the AI about a second player, each monster's update runs
 * with the player it is after swapped into the slot: Player 2's part of the
 * player state the AI touches (Split_SwapNpcSet) goes in before the update and
 * comes back out after func_8008A3AC, which finishes the monster's swings. A hit
 * or a grab then lands on that player's own state, and the player's own update
 * reacts to it later exactly as for a single player.
 *
 * The target is the nearer player, with the online branch's hysteresis (switch
 * only when the other is clearly closer) so a monster between the two does not
 * flicker, except that a monster keeps its target while it is in the middle of
 * an attack, holding a grab, or holding that player's "a monster has committed"
 * latch (charaGroupFlags[3]), and turns on whoever just hit it.
 *
 * Story characters, bosses and the alley (map0_s00, whose scripted death is the
 * story) stay on the slot player, which is Player 1 outside a Player 2 scene.
 */
#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/player.h"
#include "bodyprog/events/npc_main.h"
#include "split_xbox.h"
#include "split_internal_xbox.h"
#include "sh_log.h"

#include <string.h>

static s8  s_target[NPC_COUNT_MAX];    /* player this NPC is after */
static s8  s_lastHitBy[NPC_COUNT_MAX]; /* player whose attack landed on it last, -1 none */
static s8  s_latchOwner[2];            /* NPC holding that player's commit latch, -1 none */
static s8  s_swapped;                  /* the current update runs with the other player's set in the slot */
static s8  s_curTarget;
static s8  s_savedTargetNpc;
static u16 s_latchBefore;

void SplitNpc_Reset(void)
{
    int i;

    for (i = 0; i < NPC_COUNT_MAX; i++)
    {
        s_target[i]    = 0;
        s_lastHitBy[i] = -1;
    }
    s_latchOwner[0] = s_latchOwner[1] = -1;
    s_swapped = 0;
}

/* The monsters that hunt. Bosses and scripted fights stay on the slot player:
 * their scripts stage one player, and Split Head / Monster Cybil / the Incubus
 * key boss logic on the player they hold. */
static int Sn_Hunter(e_CharaId id)
{
    switch (id)
    {
        case Chara_AirScreamer:
        case Chara_NightFlutter:
        case Chara_Groaner:
        case Chara_Wormhead:
        case Chara_LarvalStalker:
        case Chara_Stalker:
        case Chara_GreyChild:
        case Chara_Mumbler:
        case Chara_HangedScratcher:
        case Chara_Creeper:
        case Chara_Romper:
        case Chara_PuppetNurse:
        case Chara_DummyNurse:
        case Chara_PuppetDoctor:
        case Chara_DummyDoctor:
            return 1;
        default:
            return 0;
    }
}

/* Squared XZ distance in whole world units, the metric npc_main.c uses. */
static s32 Sn_Dist2(const VECTOR3* a, const VECTOR3* b)
{
    s32 dx = Q12_TO_Q6(a->vx) - Q12_TO_Q6(b->vx);
    s32 dz = Q12_TO_Q6(a->vz) - Q12_TO_Q6(b->vz);
    return Q12_SQUARE_PRECISE(dx) + Q12_SQUARE_PRECISE(dz);
}

static int Sn_Locked(int k, const s_SubCharacter* npc, int t)
{
    const s_SubCharacter* tc = Split_PlayerChara(t);

    if (npc->field_44.field_0 > 0)
        return 1; /* a swing is in progress */
    if (Split_PlayerNpcIdx(t, 0) == k || Split_PlayerNpcIdx(t, 1) == k)
        return 1; /* holding it */
    if (tc->field_40 == k && tc->attackReceived != NO_VALUE)
        return 1; /* a hit on it is still pending */
    if (s_latchOwner[t] == k)
        return 1;
    return 0;
}

static int Sn_PickTarget(int k, const s_SubCharacter* npc)
{
    int cur = s_target[k] & 1;
    int ok0 = Split_PlayerTargetable(0);
    int ok1 = Split_PlayerTargetable(1);
    s32 dCur, dOther;

    if (!ok1)
        return 0;
    if (!ok0)
        return 1;
    if (Sn_Locked(k, npc, cur))
        return cur;
    if (npc->damage.amount > 0 && s_lastHitBy[k] >= 0)
        return s_lastHitBy[k];

    dCur   = Sn_Dist2(&npc->position, &Split_PlayerChara(cur)->position);
    dOther = Sn_Dist2(&npc->position, &Split_PlayerChara(cur ^ 1)->position);
    if ((s64)dOther * 100 < (s64)dCur * 49)
        return cur ^ 1;
    return cur;
}

/* Around one NPC's AI update (npc_main.c). */
void SplitNpc_BeginUpdate(int k, s_SubCharacter* npc)
{
    int owner, t;

    s_swapped = 0;
    if (!g_XbSplitActive || k < 0 || k >= NPC_COUNT_MAX)
        return;

    owner = Split_SlotOwner();
    t     = owner;
    if (Sn_Hunter(npc->model.charaId) && g_SysWork.sysState == SysState_Gameplay &&
        g_SavegamePtr->mapIdx != MapIdx_MAP0_S00 && Split_P2InWorld())
    {
        t = Sn_PickTarget(k, npc);
    }
    if (t != s_target[k])
    {
        s_target[k] = (s8)t;
        if (s_latchOwner[t ^ 1] == k)
        {
            /* Never switch while holding the other player's latch (Sn_Locked);
             * this only happens when that player went down. Free them. */
            *Split_PlayerLatch(t ^ 1) &= ~(CharaGroupFlag_0 | CharaGroupFlag_1);
            s_latchOwner[t ^ 1] = -1;
        }
    }
    s_curTarget = (s8)t;

    if (t != owner)
    {
        Split_SwapNpcSet();
        s_swapped = 1;
    }

    /* Corpse finalisation and the Air Screamer's dodge compare against "the"
     * targeted NPC; with two players it is the one either of them has. */
    s_savedTargetNpc = g_SysWork.targetNpcIdx;
    if (Split_PlayerTargetNpc(0) == k || Split_PlayerTargetNpc(1) == k)
        g_SysWork.targetNpcIdx = (s8)k;

    s_latchBefore = g_SysWork.charaGroupFlags[3];
}

void SplitNpc_EndUpdate(int k, s_SubCharacter* npc)
{
    int t;
    u16 latch;

    if (!g_XbSplitActive || k < 0 || k >= NPC_COUNT_MAX)
        return;

    t     = s_curTarget;
    latch = g_SysWork.charaGroupFlags[3];
    if ((latch & ~s_latchBefore) & (CharaGroupFlag_0 | CharaGroupFlag_1))
        s_latchOwner[t] = (s8)k;
    else if (!(latch & (CharaGroupFlag_0 | CharaGroupFlag_1)) && s_latchOwner[t] == k)
        s_latchOwner[t] = -1;

    g_SysWork.targetNpcIdx = s_savedTargetNpc;

    if (s_swapped)
    {
        Split_SwapNpcSet();
        s_swapped = 0;
    }

    /* The latch's owner died or left: nothing will ever release it. */
    {
        int p;
        for (p = 0; p < 2; p++)
        {
            int o = s_latchOwner[p];
            if (o >= 0 && (g_SysWork.npcs[o].model.charaId == Chara_None || g_SysWork.npcs[o].health <= Q12(0.0f)))
            {
                *Split_PlayerLatch(p) &= ~(CharaGroupFlag_0 | CharaGroupFlag_1);
                s_latchOwner[p] = -1;
            }
        }
    }
}

/* func_8008B714 landed an attack on npcs[k] from the slot player. */
void SplitNpc_NoteHit(int k)
{
    if (g_XbSplitActive && k >= 0 && k < NPC_COUNT_MAX)
        s_lastHitBy[k] = (s8)Split_WorkOwner();
}

void SplitNpc_Freed(int k)
{
    int p;

    if (k < 0 || k >= NPC_COUNT_MAX)
        return;
    s_target[k]    = 0;
    s_lastHitBy[k] = -1;
    for (p = 0; p < 2; p++)
    {
        if (s_latchOwner[p] == k)
        {
            if (g_XbSplitActive)
                *Split_PlayerLatch(p) &= ~(CharaGroupFlag_0 | CharaGroupFlag_1);
            s_latchOwner[p] = -1;
        }
    }
}

/* ---- distance rules (npc_main.c) ---- */

/* Squared distance from `pos` to the nearer player still in the world, in the
 * units npc_main.c's temp_t3 uses; `d1` is the distance it already has to the
 * slot player. */
s32 SplitNpc_NearestPlayerDist2(const VECTOR3* pos, s32 d1)
{
    s32 d2;

    if (!g_XbSplitActive || !Split_P2InWorld())
        return d1;
    d2 = Sn_Dist2(pos, &Split_SideChara()->position);
    return d2 < d1 ? d2 : d1;
}

/* Squared XZ distance to the nearer of the two cameras (Camera_Distance2dGet's
 * metric): a monster stays visible while either view is close to it. */
s32 SplitNpc_CameraDist2(const VECTOR3* pos, s32 d1)
{
    const VECTOR3* cam2;
    q25_6          dx, dz;
    s32            d2;

    if (!g_XbSplitTwoViews || (cam2 = SplitRender_ViewPos(1)) == NULL)
        return d1;
    dx = Q12_TO_Q6(cam2->vx - pos->vx);
    dz = Q12_TO_Q6(cam2->vz - pos->vz);
    d2 = Q12_MULT_PRECISE(dx, dx) + Q12_MULT_PRECISE(dz, dz);
    return d2 < d1 ? d2 : d1;
}

/* The per-frame spawn ring with two players: within 22 units of at least one of
 * them and, while playing (cond), more than 20 from both, so nothing pops into
 * either view. */
int SplitNpc_SpawnRingOk(const VECTOR3* pos, int cond)
{
    const VECTOR3* p1 = &g_SysWork.playerWork.player.position;
    const VECTOR3* p2 = &Split_SideChara()->position;
    int            near1 = !Math_Distance2dCheck(p1, pos, Q12(22.0f));
    int            near2 = !Math_Distance2dCheck(p2, pos, Q12(22.0f));
    int            far1  = Math_Distance2dCheck(p1, pos, Q12(20.0f));
    int            far2  = Math_Distance2dCheck(p2, pos, Q12(20.0f));

    if (!Split_P2InWorld())
    {
        near2 = 0;
        far2  = 1;
    }
    if (!(near1 || near2))
        return 0;
    return !cond || (far1 && far2);
}

/* The radio's interior same-cell test passes when the monster shares a cell
 * with either player. */
int SplitNpc_InPlayerCell(const VECTOR3* npcPos)
{
    const VECTOR3* p2;
    s32            pc, nc;

    if (!g_XbSplitActive || !Split_P2InWorld())
        return 0;
    p2 = &Split_SideChara()->position;
    pc = (p2->vx + (CHUNK_CELL_SIZE * 4)) / CHUNK_CELL_SIZE;
    nc = (npcPos->vx + (CHUNK_CELL_SIZE * 4)) / CHUNK_CELL_SIZE;
    if (pc != nc)
        return 0;
    pc = (p2->vz + (CHUNK_CELL_SIZE * 4)) / CHUNK_CELL_SIZE;
    nc = (npcPos->vz + (CHUNK_CELL_SIZE * 4)) / CHUNK_CELL_SIZE;
    return pc == nc;
}
