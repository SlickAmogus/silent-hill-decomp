#include "game.h"
#ifdef SH_PC_PORT
#include "sh_log.h"
#include <stdio.h>
#endif

#include <psyq/libetc.h>
#include <psyq/libpad.h>
#include <psyq/strings.h>

#include "bodyprog/bodyprog.h"
#include "bodyprog/demo.h"
#include "bodyprog/events/bodyprog_data_800A99B4.h"
#include "bodyprog/events/npc_main.h"
#include "bodyprog/events/radio.h"
#include "bodyprog/math/math.h"
#include "bodyprog/player.h"
#include "bodyprog/screen/screen_data.h"
#include "bodyprog/sound_system.h"
#include "main/fsqueue.h"

#ifdef SH_PC_PORT
static s32 Camera_Distance2dGet(const VECTOR3* pos);
#endif

void Savegame_EnemyStateUpdate(s_SubCharacter* chara) // 0x80037DC4
{
    if (g_SavegamePtr->gameDifficulty_260 <= GameDifficulty_Normal || Rng_RandQ12() >= Q12_ANGLE(108.0f))
    {
        g_SavegamePtr->ovlEnemyStates[g_SavegamePtr->mapOverlayId_A4] &= ~(1 << chara->field_40);
    }
}

void Chara_DamagedFlagUpdate(s_SubCharacter* chara) // 0x80037E40
{
    if (chara->damage.amount_C > Q12(0.0f))
    {
        chara->flags |= CharaFlag_Damaged;
    }
    else
    {
        chara->flags &= ~CharaFlag_Damaged;
    }
}

void func_80037E78(s_SubCharacter* chara) // 0x80037E78
{
    s8  idx;
    s32 cond;

    // TODO: Strange `chara->headingAngle` access.
    if (chara->health <= Q12(0.0f) && (*(s32*)&chara->headingAngle & 0x600000) == 0x200000)
    {
        idx = chara->attackReceived;
        if (idx < 39) // TODO: What weapon attack?
        {
            cond = D_800AD4C8[idx].field_10 == 3;
            func_800914C4(cond, func_8009146C(cond) + 1);
        }

        chara->flags |= CharaFlag_Dead;
    }
}

void Game_NpcRoomInitSpawn(bool cond) // 0x80037F24
{
    s_Collision     coll;
    s32             groupCharaId0;
    s32             groupCharaId1;
    s32             npcIdx;
    s32             i;
    s32*            ovlEnemiesStatePtr;
    s_SpawnInfo*    curCharaSpawn;
    s_SubCharacter* chara;
    VECTOR3*        pos;

    npcIdx             = 0;
    curCharaSpawn      = g_MapOverlayHeader.charaSpawns_24C[0];
    ovlEnemiesStatePtr = &g_SavegamePtr->ovlEnemyStates[g_SavegamePtr->mapOverlayId_A4];

#ifdef SH_PC_PORT
    /* Force-clear UnkSysFlag_4 every frame on non-tutorial maps. Vanilla
     * sets this flag in map2_s00.c:1948 unless EventFlag_146 / WaterWorks
     * cutscene event is set, gating ALL enemy spawning. On a fresh PC
     * playthrough we never reach that cutscene via the normal path so the
     * flag stays set forever, leaving streets enemy-less. Clearing here
     * (right before the spawn gate that reads it) guarantees the gate
     * sees a clean state regardless of when map init re-sets it. */
    if (g_SavegamePtr->mapOverlayId_A4 != MapIdx_MAP0_S00 &&
        g_SavegamePtr->mapOverlayId_A4 != MapIdx_MAP0_S01)
    {
        g_SysWork.flags_22A4 &= ~UnkSysFlag_4;
    }
#endif

    if (cond == false)
    {
        func_80037154();

        if (g_MapOverlayHeader.npcSpawnEvent_48 != NULL)
        {
            g_MapOverlayHeader.npcSpawnEvent_48();
        }
    }

    groupCharaId0 = g_MapOverlayHeader.charaGroupIds_248[0];
    groupCharaId1 = g_MapOverlayHeader.charaGroupIds_248[1];

#ifdef SH_PC_PORT
    /* Spawn diagnostic state. _spawnNearLogged[] holds the last logged
     * near/far state per slot (1=far, 2=near). On map change we reset it
     * so the new map's spawns log fresh on first encounter, and we keep
     * a periodic "closest spawn distance" tick so we can see if the
     * player is approaching ANY spawn at all over time. */
    static u8  _spawnNearLogged[64] = { 0 };
    static s8  _spawnLastMapId      = -1;
    static u32 _spawnTickCounter    = 0;
    if (_spawnLastMapId != g_SavegamePtr->mapOverlayId_A4) {
        memset(_spawnNearLogged, 0, sizeof(_spawnNearLogged));
        _spawnLastMapId = g_SavegamePtr->mapOverlayId_A4;
        _spawnTickCounter = 0;
        SH_DBG("[SPAWN-GATE] map changed → mapId=%d, resetting spawn-state cache",
               (int)_spawnLastMapId);
    }
    /* Tick-throttled "closest spawn" log every ~5s so we can observe player
     * approach. Computed during the loop below — capture nearest distance. */
    s32 _closestDist  = 0x7FFFFFFF;
    s32 _closestSlot  = -1;
    s32 _closestX     = 0;
    s32 _closestZ     = 0;
    s8  _closestFlags = 0;
    int _shouldTickLog = (++_spawnTickCounter % 300 == 0); /* ~5s @60fps */
#endif

    for (i = 0; i < 32 && g_VBlanks < 4; i++, curCharaSpawn++)
    {
        if (g_SysWork.npcFlags == ((1 << g_SysWork.npcFlagsId) - 1)) // TODO: Macro for this check?
        {
#ifdef SH_PC_PORT
            /* Hit the concurrent-NPC cap. Throttled log so we know if
             * this is the bottleneck. */
            static u32 _lastCapLog = 0;
            if (_spawnTickCounter - _lastCapLog > 300) {
                SH_DBG("[SPAWN-CAP] npcFlags=0x%x flagsId=%d full (mask=0x%x) — slot=%d unreachable; raise NPC_COUNT_MAX or map flagsId override",
                       (unsigned)g_SysWork.npcFlags, (int)g_SysWork.npcFlagsId,
                       (unsigned)((1 << g_SysWork.npcFlagsId) - 1), i);
                _lastCapLog = _spawnTickCounter;
            }
#endif
            break;
        }

#ifdef SH_PC_PORT
        /* CRITICAL: s_SpawnInfo is 12 bytes on PSX but 16 bytes on MinGW
         * x86-64. The s32:4 bitfield (gameDifficultyMin_7_0) forces gcc to
         * allocate a new s32 storage unit at offset 8, pushing positionZ_8
         * to offset 12. STATIC_ASSERT_SIZEOF is a no-op on PC so this size
         * change went silent. The old `pos = (VECTOR3*)curCharaSpawn` cast
         * made pos->vz read the bitfield slot (≈0 for Easy) instead of
         * positionZ_8 — every distance check saw Z=0, firing spawns at
         * coordinates totally unrelated to the actual spawn point. Build
         * a proper VECTOR3 with the correctly-typed fields and use that. */
        VECTOR3 spawnPos = { curCharaSpawn->positionX_0, 0, curCharaSpawn->positionZ_8 };
        pos = &spawnPos;
#else
        pos = (VECTOR3*)curCharaSpawn;
#endif

#ifdef SH_PC_PORT
        /* Per-spawn diagnostic — log non-empty slots when conditions change
         * (especially when player gets close enough that distance gate
         * could pass). Logs once per (slot, near/far transition) to avoid
         * spam while still capturing the moment a spawn would activate. */
        if (curCharaSpawn->flags_6 != 0) {
            VECTOR3* pp = &g_SysWork.playerWork.player.position;
            int gate7 = !Math_Distance2dCheck(pp, pos, Q12(22.0f));
            /* Track closest non-empty slot for the periodic tick log. */
            s32 dx = pp->vx - curCharaSpawn->positionX_0;
            s32 dz = pp->vz - curCharaSpawn->positionZ_8;
            /* Q12 squared-distance — keep it as squared to avoid sqrt cost. */
            s32 distSq = (s32)(((s64)dx * dx + (s64)dz * dz) >> 12);
            if (distSq < _closestDist) {
                _closestDist  = distSq;
                _closestSlot  = i;
                _closestX     = curCharaSpawn->positionX_0;
                _closestZ     = curCharaSpawn->positionZ_8;
                _closestFlags = curCharaSpawn->flags_6;
            }
            /* Only re-log on transitions: far→near (gate7 went 0→1) or
             * if first time this slot ever evaluated. */
            u8 prevState = _spawnNearLogged[i];
            u8 curState = (gate7 ? 2 : 1); /* 1=far, 2=near */
            if (prevState != curState) {
                int gate1 = !(g_SysWork.flags_22A4 & UnkSysFlag_4);
                int gate2 = HAS_FLAG(ovlEnemiesStatePtr, i) ? 1 : 0;
                int gate3 = !HAS_FLAG(g_SysWork.field_228C, i) ? 1 : 0;
                int gate5 = (g_SavegamePtr->gameDifficulty_260 >= curCharaSpawn->gameDifficultyMin_7_0);
                int gate6 = func_8008F914(curCharaSpawn->positionX_0, curCharaSpawn->positionZ_8) ? 1 : 0;
                int gate8 = (!cond || Math_Distance2dCheck(pp, pos, Q12(20.0f)));
                SH_DBG("[SPAWN-GATE] slot=%d %s spawn=(%d,%d) player=(%d,%d) flags=%d diff=%d/%d cond=%d g1=%d g2=%d g3=%d g5=%d g6=%d g7=%d g8=%d",
                       i, gate7 ? "NEAR" : "far",
                       (int)curCharaSpawn->positionX_0, (int)curCharaSpawn->positionZ_8,
                       (int)pp->vx, (int)pp->vz,
                       (int)curCharaSpawn->flags_6,
                       (int)g_SavegamePtr->gameDifficulty_260, (int)curCharaSpawn->gameDifficultyMin_7_0,
                       (int)cond,
                       gate1, gate2, gate3, gate5, gate6, gate7, gate8);
                _spawnNearLogged[i] = curState;
            }
        }
#endif

#ifdef SH_PC_PORT
        /* Mirror the spawn condition exactly so we can see WHICH gate is
         * the actual blocker. Diagnoses the case where SPAWN-GATE shows
         * all gates passing (g1..g8 = 1) but no NPC_SPAWN follows — the
         * difference must be a re-evaluation race, an aliasing issue,
         * or the npcFlags-full break above the loop. Logs once per slot
         * per second when the slot looks spawnable. */
        if (curCharaSpawn->flags_6 != 0) {
            int dbg_g1 = !(g_SysWork.flags_22A4 & UnkSysFlag_4);
            int dbg_g2 = HAS_FLAG(ovlEnemiesStatePtr, i) ? 1 : 0;
            int dbg_g3 = !HAS_FLAG(g_SysWork.field_228C, i) ? 1 : 0;
            int dbg_g5 = (g_SavegamePtr->gameDifficulty_260 >= curCharaSpawn->gameDifficultyMin_7_0);
            int dbg_g6 = func_8008F914(curCharaSpawn->positionX_0, curCharaSpawn->positionZ_8) ? 1 : 0;
            int dbg_g7 = !Math_Distance2dCheck(&g_SysWork.playerWork.player.position, pos, Q12(22.0f));
            int dbg_g8 = (!cond || Math_Distance2dCheck(&g_SysWork.playerWork.player.position, pos, Q12(20.0f)));
            int allPass = dbg_g1 && dbg_g2 && dbg_g3 && dbg_g5 && dbg_g6 && dbg_g7 && dbg_g8;
            int npcFlagsFull = (g_SysWork.npcFlags == ((1 << g_SysWork.npcFlagsId) - 1));
            if (allPass) {
                static u32 _allPassTick[64] = { 0 };
                if (_allPassTick[i] == 0 || (_spawnTickCounter - _allPassTick[i]) > 60) {
                    SH_DBG("[SPAWN-FIRE?] slot=%d allPass! npcFlags=0x%x flagsId=%d full=%d vblanks=%d ABOUT TO TRY SPAWN",
                           i, (unsigned)g_SysWork.npcFlags, (int)g_SysWork.npcFlagsId,
                           npcFlagsFull, (int)g_VBlanks);
                    _allPassTick[i] = _spawnTickCounter;
                }
            }
        }
#endif

#ifdef SH_PC_PORT
        /* Per-slot post-spawn cooldown. Without it the spawn loop and the
         * Game_NpcUpdate despawn check (line ~439, despawn at >40u) form
         * an oscillator on PC: spawn fires while player is <22u, despawn
         * fires same frame because of how player position evaluates against
         * the npc->position chain on PC, NPC slot is freed, next frame
         * spawns again. Repeats thousands of times → eventually corrupts
         * downstream state and crashes after Player_UpperBodyUpdate.
         *
         * Fix: once a slot has spawned, hold off re-spawning it for 60
         * ticks (~1s @60fps). Enough to break the same-frame oscillator
         * but short enough to preserve vanilla PSX spawn density — the
         * original 600-tick value was suppressing town enemies way more
         * than the original game. Despawn still works to clear the slot;
         * the cooldown just prevents the immediate respawn race. */
        static u32 _slotSpawnCooldown[64] = { 0 };
        if (_slotSpawnCooldown[i] > 0) {
            _slotSpawnCooldown[i]--;
        }
        /* Log when cooldown is blocking a slot that otherwise wants to spawn. */
        if (curCharaSpawn->flags_6 != 0 && _slotSpawnCooldown[i] > 0 &&
            !HAS_FLAG(g_SysWork.field_228C, i)) {
            /* Quick mirror of the distance gate to know if cooldown is the
             * actual blocker (player IS in range but cooldown gates). */
            int near22 = !Math_Distance2dCheck(&g_SysWork.playerWork.player.position, pos, Q12(22.0f));
            if (near22) {
                static u32 _cdLog[64] = { 0 };
                if (_cdLog[i] == 0 || (_spawnTickCounter - _cdLog[i]) > 60) {
                    SH_DBG("[SPAWN-COOLDOWN] slot=%d in range (player <22u) but cooldown=%u ticks remaining",
                           i, (unsigned)_slotSpawnCooldown[i]);
                    _cdLog[i] = _spawnTickCounter;
                }
            }
        }
#endif

        if (!(g_SysWork.flags_22A4 & UnkSysFlag_4) &&
            HAS_FLAG(ovlEnemiesStatePtr, i) && !HAS_FLAG(g_SysWork.field_228C, i) &&
            curCharaSpawn->flags_6 != 0 &&
            g_SavegamePtr->gameDifficulty_260 >= curCharaSpawn->gameDifficultyMin_7_0 &&
            func_8008F914(curCharaSpawn->positionX_0, curCharaSpawn->positionZ_8) &&
            !Math_Distance2dCheck(&g_SysWork.playerWork.player.position, pos, Q12(22.0f)) &&
            (!cond || Math_Distance2dCheck(&g_SysWork.playerWork.player.position, pos, Q12(20.0f)))
#ifdef SH_PC_PORT
            && _slotSpawnCooldown[i] == 0
#endif
            )
        {
#ifdef SH_PC_PORT
            SH_DBG("[SPAWN-FIRE!] slot=%d gates passed → entering spawn block, npcIdx will be assigned", i);
            _slotSpawnCooldown[i] = 60;  /* ~1s @60fps -- minimal oscillator guard */
#endif
            while (HAS_FLAG(&g_SysWork.npcFlags, npcIdx))
            {
                npcIdx++;
            }

            bzero(&g_SysWork.npcs[npcIdx], sizeof(s_SubCharacter));

            if (curCharaSpawn->charaId_4 > Chara_None)
            {
                g_SysWork.npcs[npcIdx].model.charaId = curCharaSpawn->charaId_4;
            }
            else
            {
                g_SysWork.npcs[npcIdx].model.charaId = (i < 16) ? groupCharaId0 : groupCharaId1;
            }

            g_SysWork.npcs[npcIdx].field_40           = i;
            g_SysWork.npcs[npcIdx].model.controlState = 0;
            g_SysWork.npcs[npcIdx].model.stateStep    = curCharaSpawn->flags_6;
            g_SysWork.npcs[npcIdx].position.vx        = curCharaSpawn->positionX_0;
            g_SysWork.npcs[npcIdx].position.vz        = curCharaSpawn->positionZ_8;

            Collision_Get(&coll, curCharaSpawn->positionX_0, curCharaSpawn->positionZ_8);

            g_SysWork.npcs[npcIdx].position.vy = coll.groundHeight_0;
            g_SysWork.npcs[npcIdx].rotation.vy = Q8_TO_Q12(curCharaSpawn->rotationY_5);

            SET_FLAG(&g_SysWork.npcFlags, npcIdx);
            SET_FLAG(g_SysWork.field_228C, i);

            chara                    = &g_SysWork.npcs[npcIdx];
            chara->model.anim.flags |= AnimFlag_Visible;
#ifdef SH_PC_PORT
            SH_DBG("[NPC_SPAWN] slot=%d charaId=%d spawnIdx=%d pos=(%d,%d,%d) rotY=%d",
                   (int)npcIdx, (int)chara->model.charaId, (int)i,
                   (int)chara->position.vx, (int)chara->position.vy,
                   (int)chara->position.vz, (int)chara->rotation.vy);
#endif
        }
    }

#ifdef SH_PC_PORT
    /* Periodic tick log: every ~5 seconds, dump player position and the
     * closest non-empty spawn slot. Lets us trace whether the player is
     * actually approaching ANY spawn while wandering, even when no slot
     * crosses the 22u trigger. Helps diagnose "streets are empty" — if
     * closestDist stays > 22 forever, the player just hasn't walked
     * close enough yet (or is blocked from doing so). */
    if (_shouldTickLog && _closestSlot >= 0) {
        VECTOR3* pp = &g_SysWork.playerWork.player.position;
        /* _closestDist is squared in Q12 already; rough sqrt for log
         * readability — log it as squared too so we don't pull in
         * SquareRoot12 from here. */
        SH_DBG("[SPAWN-TICK] mapId=%d player=(%d,%d) closestSlot=%d at=(%d,%d) flags=%d distSq_q12=%d (radius_q12=%d)",
               (int)g_SavegamePtr->mapOverlayId_A4,
               (int)pp->vx, (int)pp->vz,
               (int)_closestSlot, (int)_closestX, (int)_closestZ,
               (int)_closestFlags, (int)_closestDist,
               (int)(Q12(22) * Q12(22) >> 12));
    }
#endif
}

void Game_NpcUpdate(void) // 0x80038354
{
    typedef struct
    {
        s8      bitIdx_0;
        u8      unk_1[3];
        s32     field_4;
        VECTOR3 field_8;
    } s_func_800382EC_0;

    s_func_800382EC_0  field_0[3];
    u32                field_40;
    s32                posZShift6;
    s32                posXShift6;
    s32                temp_t1;
    s32                m;
    u8                 var_a2_2;
    s32                j;
    s32                var_s3;
    s32                k;
    s32                var_t5;
    s32                var_v0_4;
    s32                var_v1_3;
    s32                temp_s0_2;
    s32                temp_s0_4;
    s8                 temp_s1;
    s32                temp_v0_4;
    s32                var_v0_5;
    u32                temp_t3;
    u8                 temp_a2;
    u32                new_var;
    s32                l;
    s32                animDataInfoIdx;
    s32                temp2;
    GsCOORDINATE2*     boneCoords;
    s_SubCharacter*    npc;
    s_func_800382EC_0* temp_s0_3;

    // GCC extension funcs.
    s32 func_800382B0(s32 arg0)
    {
        s32 i;

        for (i = 0; i < 2; i++)
        {
            if (arg0 == field_0[i].bitIdx_0)
            {
                return i;
            }
        }

        return NO_VALUE;
    }

    s32 func_800382EC()
    {
        s32 i;

        for (i = 0; i < 2; i++)
        {
            if (field_0[i].bitIdx_0 == NO_VALUE)
            {
                break;
            }

            if ((field_40 & (1 << field_0[i].bitIdx_0)) == 0)
            {
                field_40 |= (1 << field_0[i].bitIdx_0);
                return i;
            }
        }

        return NO_VALUE;
    }

    posXShift6 = Q12_TO_Q6(g_SysWork.playerWork.player.position.vx);
    posZShift6 = Q12_TO_Q6(g_SysWork.playerWork.player.position.vz);

    Demo_DemoRandSeedBackup();
    Demo_DemoRandSeedRestore();

    for (j = 0; j < ARRAY_SIZE(field_0); j++)
    {
        field_0[j].bitIdx_0   = NO_VALUE;
        field_0[j].field_4    = Q12(0.25f);
        field_0[j].field_8.vy = 0;
    }

    for (k = 0, npc = g_SysWork.npcs; k < ARRAY_SIZE(g_SysWork.npcs); k++, npc++)
    {
        if (npc->model.charaId != Chara_None && npc->model.charaId != Chara_Padlock)
        {
            if (npc->model.charaId <= Chara_MonsterCybil)
            {
                temp_t3 = Q12_SQUARE_PRECISE(Q12_TO_Q6(npc->position.vx) - posXShift6) +
                          Q12_SQUARE_PRECISE(Q12_TO_Q6(npc->position.vz) - posZShift6);
                var_t5 = 0;

                if (g_MapOverlayHeader.mapInfo->flags_6 & MapFlag_Interior)
                {
                    var_t5 = (g_MapOverlayHeader.mapInfo->flags_6 & (MapFlag_OneActiveChunk | MapFlag_TwoActiveChunks)) > 0;
                }

#ifdef SH_PC_PORT
                /* Once-per-second per-NPC tracking trace. Logs why an alive
                 * NPC is or isn't being inserted into field_0[] (the radio's
                 * NPC tracker). Helps diagnose silent radio: if temp_t3
                 * stays > 1024 the NPC is out of radio range; if health
                 * stays <=0 the NPC never got Init'd; etc. */
                {
                    static u32 _trkTick[6] = { 0 };
                    static u32 _trkCounter = 0;
                    if (k == 0) _trkCounter++;
                    if (k < 6 && (_trkCounter - _trkTick[k]) > 60) {
                        SH_DBG("[NPC-TRACK] npc=%d charaId=%d health=%d flags=0x%x pos=(%d,%d) temp_t3=%d field_4[0]=%d health_gate=%d insertRange=%d despawnRange=%d",
                               (int)k, (int)npc->model.charaId,
                               (int)npc->health, (unsigned)npc->flags,
                               (int)npc->position.vx, (int)npc->position.vz,
                               (int)temp_t3, (int)field_0[0].field_4,
                               (int)(npc->health > Q12(0.0f)),
                               (int)(temp_t3 < field_0[0].field_4),
                               (int)(temp_t3 < SQUARE(40)));
                        _trkTick[k] = _trkCounter;
                    }
                }
#endif

                for (j = 0; j < 3; j++)
                {
                    if (npc->health <= Q12(0.0f) || npc->flags & CharaFlag_Unk9 || temp_t3 >= field_0[j].field_4)
                    {
                        continue;
                    }

                    if (var_t5 != 0)
                    {
                        s32 playerCell = (g_SysWork.playerWork.player.position.vx + (CHUNK_CELL_SIZE * 4)) / CHUNK_CELL_SIZE;
                        s32 npcCell    = (npc->position.vx                        + (CHUNK_CELL_SIZE * 4)) / CHUNK_CELL_SIZE;
                        if (npcCell != playerCell)
                        {
                            continue;
                        }

                        // TODO: Unique vars for these.
                        playerCell = (g_SysWork.playerWork.player.position.vz + (CHUNK_CELL_SIZE * 4)) / CHUNK_CELL_SIZE;
                        npcCell    = (npc->position.vz                        + (CHUNK_CELL_SIZE * 4)) / CHUNK_CELL_SIZE;
                        if (npcCell != playerCell)
                        {
                            continue;
                        }
                    }

                    for (m = 2; j < m; m--)
                    {
                        field_0[m].bitIdx_0   = field_0[m - 1].bitIdx_0;
                        field_0[m].field_4    = field_0[m - 1].field_4;
                        field_0[m].field_8.vx = field_0[m - 1].field_8.vx;
                        field_0[m].field_8.vz = field_0[m - 1].field_8.vz;
                    }

                    temp_t1 = (uintptr_t)npc - (uintptr_t)g_SysWork.npcs;
                    temp2   = ((((temp_t1 * 0x7E8) - (temp_t1 * 0xFD)) * 4) + temp_t1) * -0x3FFFF;

#ifdef SH_PC_PORT
                    /* The MIPS-compiler reciprocal-multiply above computes
                     * `temp_t1 / sizeof(s_SubCharacter)` to recover the NPC
                     * array index k. The constants (0x7E8, 0xFD, -0x3FFFF)
                     * are baked for PSX struct sizes; on PC s_SubCharacter
                     * is larger so the formula gives garbage. Just use k
                     * directly — it IS the array index. */
                    field_0[j].bitIdx_0   = (s8)k;
                    SH_DBG("[NPC-INSERT] field_0[%d] = npc[%d] charaId=%d temp_t3=%d (radio should fire)",
                           (int)j, (int)k, (int)npc->model.charaId, (int)temp_t3);
#else
                    field_0[j].bitIdx_0   = temp2 >> 3;
#endif
                    field_0[j].field_4    = temp_t3;
                    field_0[j].field_8.vx = npc->position.vx;
                    field_0[j].field_8.vz = npc->position.vz;
                    break;
                }

                new_var = temp_t3;

                if (new_var > ((var_t5 == 0 && npc->health < Q12(0.0f)) ? SQUARE(24) : SQUARE(40)))
                {
#ifdef SH_PC_PORT
                    SH_DBG("[NPC-DESPAWN] npcSlot=%d charaId=%d spawnSlot=%d distSq=%d (radius=%d) deadFastTrack=%d",
                           (int)k, (int)npc->model.charaId, (int)npc->field_40,
                           (int)new_var,
                           (var_t5 == 0 && npc->health < Q12(0.0f)) ? 576 : 1600,
                           (int)(npc->health < Q12(0.0f)));
#endif
                    npc->model.charaId = Chara_None;
                    SysWork_NpcFlagClear(k);
                    CLEAR_FLAG(g_SysWork.field_228C, npc->field_40);
                    continue;
                }

                if ((g_SysWork.field_2388.field_154.effectsInfo_0.field_0.s_field_0.field_0 & 0x2 && temp_t3 > SQUARE(15)) ||
                    (!(g_SysWork.field_2388.field_154.effectsInfo_0.field_0.s_field_0.field_0 & 0x2) &&
                     Camera_Distance2dGet(&npc->position) > SQUARE(15)))
                {
                    npc->model.anim.flags &= ~AnimFlag_Visible;
                }
                else
                {

                    npc->model.anim.flags |= AnimFlag_Visible;
                }
            }

            npc->model.anim.flags |= AnimFlag_Unlocked;

            animDataInfoIdx = g_CharaAnimInfoIdxs[npc->model.charaId];
#ifdef SH_PC_PORT
            /* On PC only Cheryl's NPC AI is safe to run. All other NPCs
             * (Cybil, monsters, grey children) have AI that crashes due to
             * unsupported subsystems (collision, PSX-specific state).
             * Also skip if anim data not loaded yet (idx==0xFF) or update
             * function pointer is NULL (sanitized out by map overlay loader). */
            {
                bool animLoaded  = ((s8)animDataInfoIdx != (s8)0xFF);
                bool hasUpdateFn = (npc->model.charaId < (e_CharacterId)ARRAY_SIZE(g_MapOverlayHeader.charaUpdateFuncs_194) &&
                                    g_MapOverlayHeader.charaUpdateFuncs_194[npc->model.charaId] != NULL);
                /* NPCs whose AI we fully run.  Cheryl + GreyChild were the
                 * baseline working set; Cybil + AirScreamer added because
                 * render-only path never produced a visible model — they need
                 * AI updates to drive the model state. Groaner (dog) added
                 * for map2_s00 streets; LarvalStalker for map2_s00/s01 small
                 * grey-children. When new NPCs crash, narrow this list rather
                 * than going back to a blanket skip. */
                bool isFullAiNpc = (npc->model.charaId == Chara_Cheryl ||
                                    npc->model.charaId == Chara_GreyChild ||
                                    npc->model.charaId == Chara_Cybil ||
                                    npc->model.charaId == Chara_AirScreamer ||
                                    npc->model.charaId == Chara_Groaner ||
                                    npc->model.charaId == Chara_LarvalStalker ||
                                    npc->model.charaId == Chara_Bloodsucker ||
                                    npc->model.charaId == Chara_BloodyLisa ||
                                    npc->model.charaId == Chara_Alessa ||
                                    npc->model.charaId == Chara_GhostChildAlessa ||
                                    npc->model.charaId == Chara_Lisa ||
                                    npc->model.charaId == Chara_Kaufmann ||
                                    npc->model.charaId == Chara_Dahlia ||
                                    npc->model.charaId == Chara_PuppetNurse ||
                                    npc->model.charaId == Chara_HangedScratcher ||
                                    npc->model.charaId == Chara_Creeper ||
                                    npc->model.charaId == Chara_SplitHead ||
                                    npc->model.charaId == Chara_Romper);
                /* No render-only set — kept as opt-out for any future NPC that
                 * really only needs the model and not the full AI dispatch. */
                bool isRenderOnlyNpc = false;

                /* Force AnimFlag_Visible for charaId > Chara_MonsterCybil --
                 * the distance-check block above only runs for ids <= 24,
                 * so Cybil (26) and AirScreamer would otherwise never get
                 * the flag set and would skip the func_8003DA9C render call. */
                if (isFullAiNpc && npc->model.charaId > Chara_MonsterCybil) {
                    npc->model.anim.flags |= AnimFlag_Visible;
                }

                if (!animLoaded || !isFullAiNpc)
                {
                    if (isFullAiNpc && !animLoaded)
                    {
                        /* Anim data not loaded yet (Chara_Spawn just happened this
                         * frame, async ANM read still pending). Do NOT kill the
                         * NPC — the slot would get wiped and game code expecting
                         * npcs[slot] to hold this chara (e.g. map0_s01 BIRD
                         * fly-by) would dereference an empty slot and crash.
                         * Just skip AI this tick and wait for load to complete. */
                        static u32 _animWaitLogged = 0;
                        if (!(_animWaitLogged & (1u << (npc->model.charaId & 31)))) {
                            SH_DBG("[NPC_AI] charaId=%d anim not loaded yet (idx=%d) — waiting",
                                   npc->model.charaId, (int)(s8)animDataInfoIdx);
                            _animWaitLogged |= (1u << (npc->model.charaId & 31));
                        }
                    }
                    else if (isRenderOnlyNpc)
                    {
                        /* Keep render-only NPCs alive even while ANM is still loading. */
                        if (animLoaded && (npc->model.anim.flags & AnimFlag_Visible)) {
                            SH_DBG("[NPC_RENDER] charaId=%d animIdx=%d npcBoneCoords=%p",
                                   npc->model.charaId, animDataInfoIdx,
                                   (void*)g_CharaTypeAnimInfo[animDataInfoIdx].npcBoneCoords);
                            func_8003DA9C(npc->model.charaId,
                                          g_CharaTypeAnimInfo[animDataInfoIdx].npcBoneCoords,
                                          1, npc->timer_C6,
                                          (s8)npc->model.paletteIdx);
                            SH_DBG("[NPC_RENDER] done charaId=%d", npc->model.charaId);
                        }
                    }
                    else
                    {
                        /* Fully unsafe NPC — remove so it doesn't keep firing. */
                        npc->model.charaId = Chara_None;
                    }
                    continue;
                }
                if (!hasUpdateFn)
                {
                    /* Map overlay's charaUpdateFunc was NULL (likely sanitized
                     * out by map_overlay_loader for an un-decompiled stub).
                     * Don't kill the NPC — keep it alive so the model can
                     * render even without AI driving it. */
                    static u32 _noUpdateFnLogged = 0;
                    if (!(_noUpdateFnLogged & (1u << (npc->model.charaId & 31)))) {
                        SH_DBG("[NPC_AI] charaId=%d has NULL charaUpdateFunc — skipping AI, keeping for render",
                               npc->model.charaId);
                        _noUpdateFnLogged |= (1u << (npc->model.charaId & 31));
                    }
                    if (animLoaded && (npc->model.anim.flags & AnimFlag_Visible)) {
                        func_8003DA9C(npc->model.charaId,
                                      g_CharaTypeAnimInfo[animDataInfoIdx].npcBoneCoords,
                                      1, npc->timer_C6,
                                      (s8)npc->model.paletteIdx);
                    }
                    continue;
                }
            }
            /* Reset stateStep only on the first frame after spawn so
             * Model_AnimStatusSet can fire once.  Don't reset every frame
             * or anim status transitions (blend→playback) get stuck. */
            if (npc->model.charaId == Chara_Cheryl)
            {
                static bool _cherylInitDone = false;
                if (!_cherylInitDone) {
                    npc->model.stateStep = 0;
                    _cherylInitDone = true;
                }
            }
            /* Same spawn-init pattern for Cybil/AirScreamer: reset stateStep
             * once on first AI tick so Model_AnimStatusSet fires and the NPC
             * actually enters its state machine. Without this the NPC appears
             * loaded but never animates.  Per-slot guard keyed on charaId so
             * a second spawn after the first dies re-inits.
             *
             * NOT applied to GreyChild/Stalker: their AI uses stateStep as
             * an init-switch selector (stateStep_5 / _6 / _7 etc map to
             * different StalkerControl_X states).  map0_s00_2.c rewrites
             * controlState=Uninitialized + stateStep=6 after the corpse
             * cutscene to make them aggressive; resetting stateStep to 0
             * here would break that handoff and leave them stuck in the
             * Init->switch-no-match->Init loop forever. */
            else if (npc->model.charaId == Chara_Cybil ||
                     npc->model.charaId == Chara_AirScreamer)
            {
                /* Per-slot latch — fire ONCE per spawn, not every frame
                 * the NPC happens to be at controlState==None.
                 *
                 * Original code stomped stateStep=0 every frame
                 * controlState was 0, which broke the cutscene→combat
                 * handoff: Air Screamer's intro sets controlState=None
                 * + stateStep=7 to transition into Control_46 (combat
                 * dive); the next NpcUpdate would then immediately
                 * stomp stateStep back to 0, killing the handoff and
                 * leaving the AS in StandIdle forever. Same family
                 * also affects Cybil combat in later levels.
                 *
                 * Latch resets when the slot is cleared (charaId →
                 * Chara_None on death/despawn) so a respawn re-arms. */
                static u8 _spawnInitDone[3]   = { 0, 0, 0 };
                static u8 _lastInitCharaId[3] = { 0xFF, 0xFF, 0xFF };
                if (k < 3) {
                    /* Re-arm latch if the slot's charaId changed
                     * (despawn/respawn cycle, including a new NPC
                     * occupying the same slot). */
                    if (npc->model.charaId != _lastInitCharaId[k]) {
                        _spawnInitDone[k] = 0;
                        _lastInitCharaId[k] = npc->model.charaId;
                    }
                    if (!_spawnInitDone[k] && npc->model.controlState == 0) {
                        npc->model.stateStep = 0;
                        _spawnInitDone[k] = 1;
                        SH_DBG("[NPC_AI] one-shot spawn-init charaId=%d slot=%d stateStep=0",
                               npc->model.charaId, (int)k);
                    }
                }
            }
#endif
            boneCoords      = g_CharaTypeAnimInfo[animDataInfoIdx].npcBoneCoords;

            Chara_Flag8Clear(npc);
            Chara_DamagedFlagUpdate(npc);
            func_8003BD48(npc);

#ifdef SH_PC_PORT
            /* Guard against NULL animFile for any NPC: the playback function
             * always dereferences animHdr for bone data, so NULL crashes.
             * Cheryl logs details; other NPCs (e.g. grey children) just wait
             * until Chara_ProcessLoads() completes their ANM read. */
            if (g_CharaTypeAnimInfo[animDataInfoIdx].animFile1_8 == NULL) {
                if (npc->model.charaId == Chara_Cheryl) {
                    SH_DBG("[NPC_AI] Cheryl: animDataInfoIdx=%d animFile1_8=NULL boneCoords=%p — skipping",
                            animDataInfoIdx, (void*)boneCoords);
                } else {
                    SH_DBG("[NPC_AI] charaId=%d animDataInfoIdx=%d animFile1_8=NULL — waiting for load",
                            npc->model.charaId, animDataInfoIdx);
                }
                continue;
            }
#endif
#ifdef SH_PC_PORT
            SH_DBG("[NPC] ai-enter charaId=%d status=%d kf=%d",
                    npc->model.charaId, npc->model.anim.status,
                    npc->model.anim.keyframeIdx);
#endif
            g_MapOverlayHeader.charaUpdateFuncs_194[npc->model.charaId](npc, g_CharaTypeAnimInfo[animDataInfoIdx].animFile1_8, boneCoords);
#ifdef SH_PC_PORT
            SH_DBG("[NPC] ai-done charaId=%d status=%d", npc->model.charaId, npc->model.anim.status);
#endif

            func_8003BE28();
#ifdef SH_PC_PORT
            SH_DBG("[NPC] post-BE28 charaId=%d", npc->model.charaId);
#endif
            func_80037E78(npc);
#ifdef SH_PC_PORT
            SH_DBG("[NPC] post-7E78 charaId=%d", npc->model.charaId);
#endif
            func_8008A3AC(npc);
#ifdef SH_PC_PORT
            SH_DBG("[NPC] post-A3AC charaId=%d", npc->model.charaId);
#endif

            if (npc->model.anim.flags & AnimFlag_Visible)
            {
                func_8003DA9C(npc->model.charaId, boneCoords, 1, npc->timer_C6, (s8)npc->model.paletteIdx);
            }
        }
    }

    for (k = 2; k >= 0; k--)
    {
        if (field_0[k].bitIdx_0 != NO_VALUE)
        {
            break;
        }
    }

    g_RadioPitchState = k + 1;

    if (!(g_SavegamePtr->itemToggleFlags_AC & ItemToggleFlag_RadioOn))
    {
        return;
    }

    field_40 = 0;

    for (l = 0; l < ARRAY_SIZE(D_800BCDA8); l++)
    {
        temp_s0_2 = D_800BCDA8[l].field_1;
        if (temp_s0_2 == NO_VALUE)
        {
            var_v0_4 = NO_VALUE;
        }
        else
        {
            var_v0_4 = func_800382B0(temp_s0_2);
        }

        if (var_v0_4 >= 0)
        {
            D_800BCDA8[l].field_2 = var_v0_4;
            field_40             |= 1 << temp_s0_2;
        }
        else
        {
            D_800BCDA8[l].field_1 = NO_VALUE;
        }
    }

    for (l = 0; l < ARRAY_SIZE(D_800BCDA8); l++)
    {
        temp_s1 = D_800BCDA8[l].field_1;
        if (temp_s1 == NO_VALUE)
        {
            temp_v0_4 = func_800382EC();
            if (temp_v0_4 != temp_s1)
            {
                var_v0_5 = field_0[temp_v0_4].bitIdx_0;
            }
            else
            {
                var_v0_5 = NO_VALUE;
            }

            D_800BCDA8[l].field_2 = temp_v0_4;
            D_800BCDA8[l].field_1 = var_v0_5;
        }
    }

    for (l = 0; l < ARRAY_SIZE(D_800BCDA8); l++)
    {
#ifdef SH_PC_PORT
        /* One-shot per-slot keyon diagnostic so we can verify the radio
         * voice actually starts when an enemy first enters range. */
        static s8 _radioKeyonLogged[2] = { 0, 0 };
        if (l < 2 && !_radioKeyonLogged[l] &&
            D_800BCDA8[l].field_0 == NO_VALUE && D_800BCDA8[l].field_1 >= 0) {
            SH_DBG("[RADIO_KEYON] slot=%d firing SD_Call(0x%04X) field_0=%d field_1=%d field_2=%d",
                   (int)l, (unsigned)(Sfx_RadioInterferenceLoop + l),
                   (int)D_800BCDA8[l].field_0, (int)D_800BCDA8[l].field_1,
                   (int)D_800BCDA8[l].field_2);
            _radioKeyonLogged[l] = 1;
        }
        /* Throttled state-snapshot — every ~1s log the actual D_800BCDA8 values
         * so we can confirm whether field_0 is stuck at non-NO_VALUE. */
        {
            static u32 _radStateTickCnt = 0;
            if (l == 0 && (++_radStateTickCnt % 60) == 0) {
                SH_DBG("[RADIO_STATE] tick=%u slot0=(f0=%d,f1=%d,f2=%d) slot1=(f0=%d,f1=%d,f2=%d)",
                       _radStateTickCnt,
                       (int)D_800BCDA8[0].field_0, (int)D_800BCDA8[0].field_1, (int)D_800BCDA8[0].field_2,
                       (int)D_800BCDA8[1].field_0, (int)D_800BCDA8[1].field_1, (int)D_800BCDA8[1].field_2);
            }
        }
#endif
        if (D_800BCDA8[l].field_0 == NO_VALUE)
        {
            if (D_800BCDA8[l].field_1 >= 0)
            {
                SD_Call((u16)(Sfx_RadioInterferenceLoop + l));
            }
        }
        else
        {
            var_s3 = 0;
            if (!(g_MapOverlayHeader.mapInfo->flags_6 & MapFlag_Interior) ||
                !(g_MapOverlayHeader.mapInfo->flags_6 & (MapFlag_OneActiveChunk | MapFlag_TwoActiveChunks)))
            {
                var_s3 = 1;
            }

            if (D_800BCDA8[l].field_1 >= 0)
            {
                temp_s0_3 = &field_0[D_800BCDA8[l].field_2];
                temp_s0_4 = Vc_StereoBalanceGet(&temp_s0_3->field_8);

                var_v1_3 = SquareRoot12(temp_s0_3->field_4 << Q12_SHIFT) >> 8;
                if (var_s3 != 0)
                {
                    var_v1_3 >>= 1;
                }

                var_a2_2 = CLAMP(var_v1_3, 0, 0xFF);

                Sd_SfxAttributesUpdate(Sfx_RadioInterferenceLoop + l, temp_s0_4, var_a2_2, 0);
            }
            else
            {
                Sd_SfxStop(Sfx_RadioInterferenceLoop + l);
            }
        }

        D_800BCDA8[l].field_0 = D_800BCDA8[l].field_1;
    }
}

bool Math_Distance2dCheck(const VECTOR3* from, const VECTOR3* to, q19_12 radius) // 0x80038A6C
{
    q19_12 deltaX;
    q19_12 deltaZ;
    q19_12 radiusSqr;
    q19_12 sum;

    // Check rough radius intersection on X axis.
    deltaX = from->vx - to->vx;
    if (radius < deltaX)
    {
        return true;
    }
    if (radius < -deltaX)
    {
        return true;
    }

    // Check rough radius intersection on Z axis.
    deltaZ = from->vz - to->vz;
    if (radius < deltaZ)
    {
        return true;
    }
    if (radius < -deltaZ)
    {
        return true;
    }

    // Check distance.
    sum       = Q12_MULT_PRECISE(deltaX, deltaX) + Q12_MULT_PRECISE(deltaZ, deltaZ);
    radiusSqr = Q12_MULT_PRECISE(radius, radius);
    return sum > radiusSqr;
}

/** @brief Computes the squared 2D distance on the XZ plane from the reference position to the camera.
 *
 * @param pos Reference position (Q19.12).
 * @return 2D distance to the camera. TODO: Does it stay in Q25.6?
 */
static s32 Camera_Distance2dGet(const VECTOR3* pos) // 0x80038B44
{
    VECTOR3 camPos; // Q19.12
    q25_6   deltaX;
    q25_6   deltaZ;

    vwGetViewPosition(&camPos);
    deltaX = Q12_TO_Q6(camPos.vx - pos->vx);
    deltaZ = Q12_TO_Q6(camPos.vz - pos->vz);
    return Q12_MULT_PRECISE(deltaX, deltaX) + Q12_MULT_PRECISE(deltaZ, deltaZ);
}
