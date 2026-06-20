#include "game.h"
#include "inline_no_dmpsx.h"

#include <psyq/gtemac.h>
#include <psyq/libapi.h>
#include <psyq/strings.h>

#include "bodyprog/bodyprog.h"
#include "bodyprog/collision/collision.h"
#ifdef SH_PC_PORT
#include "sh_log.h"
#endif
#include "bodyprog/math/math.h"
#include "bodyprog/item_screens.h"
#include "bodyprog/player.h"
#include "bodyprog/screen/screen_data.h"
#include "bodyprog/screen/screen_draw.h"
#include "bodyprog/sound/sound_system.h"
#include "main/rng.h"

#ifdef SH_PC_PORT
#include "dbg_overlay.h"
extern int g_CollVisEnabled; /* Collision visualizer toggle (dbg_overlay.c, ' key) */

/* [WALLSTOP] (#42 v2): the [WALL-HIT] probe logged proximity to wall faces, but
 * the actual movement block can come from THREE paths, and the deciding element
 * (field_34/38 swept clamp or field_44 angle block) is often set by a DIFFERENT
 * subcell than state.point holds when the pass ends. Stash the real blocking
 * element AT each block site, then log it once per pass for Harry. kind:
 * 1=swept wall, 2=static wall, 3=ROUND OBSTACLE (ptr_18, no split face -> invisible
 * to the [WALL-HIT] probe and the visualizer; prime invisible-wall suspect). */
struct s_WallStopDbg { int active, kind; s32 sub, ax, az, bx, bz, rad, dist, frac;
                       s32 fullx, fullz, cellw, cellh, offx, offz; };
static struct s_WallStopDbg g_WallStopDbg;
/* `state` is in scope at every call site (the 3 block fns all take s_CollisionState*).
 * fullx/z = the UN-truncated s32 chunk-local offset (positionFrom - chunk origin);
 * offx/z = the s16 DVECTOR_XZ that the collision actually used. fullx!=offx => the
 * s16 offset WRAPPED (long-chunk overflow). cellw/h = chunk extent (s32). */
#define WALLSTOP_SET(k, s, _ax, _az, _bx, _bz, _r, _d, _f) do { \
    const s_IpdCollisionData* _cd = state->point.ipdCollisionData; \
    g_WallStopDbg.active = 1; g_WallStopDbg.kind = (k); g_WallStopDbg.sub = (s); \
    g_WallStopDbg.ax = (_ax); g_WallStopDbg.az = (_az); g_WallStopDbg.bx = (_bx); \
    g_WallStopDbg.bz = (_bz); g_WallStopDbg.rad = (_r); g_WallStopDbg.dist = (_d); \
    g_WallStopDbg.frac = (_f); \
    g_WallStopDbg.offx = state->charaPositionFrom.offset.vx; \
    g_WallStopDbg.offz = state->charaPositionFrom.offset.vz; \
    g_WallStopDbg.fullx = _cd ? (state->charaState.positionFromX - _cd->positionX) : 0; \
    g_WallStopDbg.fullz = _cd ? (state->charaState.positionFromZ - _cd->positionZ) : 0; \
    g_WallStopDbg.cellw = _cd ? ((s32)_cd->subcellSize * _cd->subcellCountX) : 0; \
    g_WallStopDbg.cellh = _cd ? ((s32)_cd->subcellSize * _cd->subcellCountZ) : 0; \
    } while (0)

/* Draw the round obstacle (ptr_18) that actually blocked Harry as a RED box in
 * the collision visualizer — at its world position in WHATEVER chunk it lives in
 * (the cached cyan cylinders only cover Harry's current cell, so the swept-caught
 * one in an adjacent cell was invisible). World Q12 = (origin + offset) << 4. */
extern int g_CollVisEnabled;
extern void CollVis_CaptureHitCylinder(s32 cx, s32 cy, s32 cz, s32 r);
#define WALLSTOP_VIS_OBST() do { \
    const s_IpdCollisionData* _vcd = state->point.ipdCollisionData; \
    if (g_CollVisEnabled && _vcd) { \
        CollVis_CaptureHitCylinder((_vcd->positionX + state->point.field_6.vx) << 4, \
                                   (s32)state->point.field_6.vy << 4, \
                                   (_vcd->positionZ + state->point.field_6.vz) << 4, \
                                   state->point.field_C.field_0 << 4); \
    } } while (0)

/* Invisible-wall fix (#42): ptr_18 round obstacles (street poles/hydrants — point
 * colliders with no rendered model) hard-stop a SPRINTING Harry from far via the
 * swept test, in adjacent chunks, with nothing visible. 0 = their solid blocking
 * is OFF (ground-height use is kept; walls/buildings still block). Console `OBST`. */
int g_PcObstacleCollision = 0;
#endif

// Note - Will: I added a bunch of poorly written comments among the code
// to have a better follow up of a code comprehension, some may be
// misleading as I'm mostly based on code functionallity with barely any
// in-game test.
// Please delete them or rewrite them properly after properly recognizing
// the function purpose.

s_ActiveCollisionTriggers g_ActiveCollisionTriggers;

void Collision_Init(void) // 0x800697EC
{
    Collision_FlagsSet(CollisionTriggerFlag_0);
    g_ActiveCollisionTriggers.collisionTriggerCount = 0;
}

u16 Collision_FlagsGet(void) // 0x80069810
{
    return g_ActiveCollisionTriggers.flags;
}

void Collision_FlagsSet(u16 collFlags) // 0x80069820
{
    g_ActiveCollisionTriggers.flags = collFlags;
}

void Collision_FlagBitsSet(u16 collFlags) // 0x8006982C
{
    g_ActiveCollisionTriggers.flags |= collFlags;
}

void Collision_FlagBitsClear(s32 collFlags) // 0x80069844
{
    g_ActiveCollisionTriggers.flags = (g_ActiveCollisionTriggers.flags & ~collFlags) | CollisionTriggerFlag_0;
}

void Collision_NearbyTriggersGet(q19_12 posX, q19_12 posZ, s_CollisionTrigger* triggers) // 0x80069860
{
    #define AABB_BUFFER Q12(16.0f)

    s_CollisionTrigger* curTrigger;
    q19_12              minX;
    q19_12              maxX;
    q19_12              minZ;
    q19_12              maxZ;

    g_ActiveCollisionTriggers.collisionTriggerCount = 0;

    // Run through collision triggers.
    for (curTrigger = triggers; !curTrigger->isEndOfArray; curTrigger++)
    {
        // Define AABB bounds.
        minX = FP_TO(curTrigger->positionX, Q12_SHIFT);
        maxX = FP_TO(curTrigger->positionX + curTrigger->sizeX, Q12_SHIFT);
        minZ = FP_TO(curTrigger->positionZ, Q12_SHIFT);
        maxZ = FP_TO(curTrigger->positionZ + curTrigger->sizeZ, Q12_SHIFT);

        // Expand AABB.
        minX -= AABB_BUFFER;
        maxX += AABB_BUFFER;
        minZ -= AABB_BUFFER;
        maxZ += AABB_BUFFER;

        // Collect collided trigger.
        if (posX >= minX && maxX >= posX &&
            posZ >= minZ && maxZ >= posZ)
        {
            g_ActiveCollisionTriggers.collisionTriggers[g_ActiveCollisionTriggers.collisionTriggerCount] = curTrigger;
            g_ActiveCollisionTriggers.collisionTriggerCount++;
        }
    }

    #undef AABB_BUFFER
}

void Ipd_CollisionPtrsInit(s_IpdCollisionData* collData) // 0x8006993C
{
    collData->splitVertices = (u8*)collData->splitVertices + (u32)collData;
    collData->surfaces      = (u8*)collData->surfaces      + (u32)collData;
    collData->subcells      = (u8*)collData->subcells      + (u32)collData;
    collData->ptr_18        = (u8*)collData->ptr_18        + (u32)collData;
    collData->subcellRanges = (u8*)collData->subcellRanges + (u32)collData;
    collData->ptr_28        = (u8*)collData->ptr_28        + (u32)collData;
    collData->ptr_2C        = (u8*)collData->ptr_2C        + (u32)collData;
}

void Collision_SubcellChecksReset(s_IpdCollisionData* collData) // 0x80069994
{
    s32* curPtr;

    collData->subcellCheckCount++;
    if (collData->subcellCheckCount > (sizeof(collData->subcellCheckIdx) - 4))
    {
        collData->subcellCheckCount = 0;

        // TODO: Is this `memset`/`bzero`?
        for (curPtr = &collData->subcellCheckIdx[0]; curPtr < &collData->subcellCheckIdx[sizeof(collData->subcellCheckIdx)]; curPtr++)
        {
            *curPtr = 0;
        }
    }
}

void Collision_SubcellChecksCountUpdate(s_IpdCollisionData* collData) // 0x800699E4
{
    collData->subcellCheckCount++;
}

void Collision_SurfaceGet(s_CollisionSurface* surface, q19_12 posX, q19_12 posZ) // 0x800699F8
{
    s_CollisionCylinder cylinder;
    VECTOR3             pos;
    s_CollisionState    state;
    s_IpdCollisionData* collData;

#ifdef SH_PC_PORT
    /* Zero-init collision state: PSX stack contents are predictable, x86-64's
     * vary with binary layout, so uninitialized fields caused sporadic crashes. */
    memset(&state, 0, sizeof(state));
#endif

    pos.vx = Q12(0.0f);
    pos.vy = Q12(0.0f);
    pos.vz = Q12(0.0f);

    // Get collision data at world position.
    collData = Ipd_CollisionDataGet(posX, posZ);
    if (collData == NULL)
    {
        surface->groundHeight = Q12(8.0f);
        surface->tiltAngleZ   = Q12_ANGLE(0.0f);
        surface->tiltAngleX   = Q12_ANGLE(0.0f);
        surface->groundType   = GroundType_Default;
        return;
    }

    // Define cylinder.
    cylinder.position.vx = posX;
    cylinder.position.vy = Q12(0.0f);
    cylinder.position.vz = posZ;
    cylinder.bottom      = Q12(0.0f);
    cylinder.top         = Q12(0.0f);
    cylinder.radius      = Q12(0.0f);

    Collision_CollStateInit(&state, &pos, &cylinder, false);

    state.isCharaMoving = false;
    state.field_0_9     = false;
    state.field_0_10    = true;
    Collision_CharaCollisionHandle(&state, collData);

    if (state.heightDisabled == true)
    {
        surface->groundType   = GroundType_Default;
        surface->groundHeight = Q12(8.0f);
    }
    else
    {
        surface->groundType   = state.groundType;
        surface->groundHeight = Q8_TO_Q12(Ipd_GroundHeightGet(state.charaState.positionFromX, state.charaState.positionFromZ, &state));
    }

    surface->tiltAngleX = state.tiltAngleX;
    surface->tiltAngleZ = state.tiltAngleZ;
}

s32 Collision_WallDetect(s_CollisionResult* collResult, const VECTOR3* moveOffset, s_SubCharacter* chara) // 0x80069B24
{
    s32 stackPtr;
    s32 response;

    stackPtr = SetSp(PSX_SCRATCH_ADDR(0x3D8));
    response = Collision_WallResponse(collResult, moveOffset, chara, Collision_CharaCollisionSetup(collResult, moveOffset, chara));
    SetSp(stackPtr);
    return response;
}

s32 Collision_WallResponse(s_CollisionResult* collResult, const VECTOR3* moveOffset, s_SubCharacter* chara, s32 response) // 0x80069BA8
{
    #define POINT_COUNT          9
    #define ANGLE_STEP           Q12_ANGLE(370.0f / POINT_COUNT) // @bug? Maybe `360.0f` was intended.
    #define WALL_COUNT_THRESHOLD 3                               // Unknown purpose.
    #define WALL_HEIGHT          Q12(0.5f)

    s_CollisionSurface surface;
    e_CollisionType    collType;
    q19_12             groundHeight;
    q19_12             wallHeightBound;
    s32                i;
    s32                wallCount;
    s32                groundType; // `e_GroundType`

    if (response == NO_VALUE)
    {
        response = true;

        if (chara == &g_SysWork.playerWork && chara->health > Q12(0.0f))
        {
            Collision_GroundProbeRadial(collResult, &chara->position, chara->position.vy, chara->rotation.vy);
        }
    }

    switch (chara->model.charaId)
    {
        case Chara_Harry:
        case Chara_Groaner:
        case Chara_Wormhead:
        case Chara_LarvalStalker:
        case Chara_Stalker:
        case Chara_GreyChild:
        case Chara_Mumbler:
        case Chara_Creeper:
        case Chara_Romper:
        case Chara_PuppetNurse:
        case Chara_PuppetDoctor:
            wallHeightBound = chara->position.vy - WALL_HEIGHT;

            switch (collResult->surface.groundType)
            {
                case GroundType_None:
                    collType = CollisionType_Unk2;
                    break;

                default:
                    collType = (collResult->surface.groundHeight < wallHeightBound) ? CollisionType_Wall : CollisionType_None;
                    break;
            }

            wallCount = 0;
            if (collType == CollisionType_None)
            {
                break;
            }

            for (i = 0, groundType = GroundType_None; i < POINT_COUNT; i++)
            {
                Collision_SurfaceGet(&surface,
                                     chara->position.vx + Q12_MULT(Math_Sin(i * ANGLE_STEP), Q12(0.2f)),
                                     chara->position.vz + Q12_MULT(Math_Cos(i * ANGLE_STEP), Q12(0.2f)));

                switch (collType)
                {
                    case CollisionType_Wall:
                        if (surface.groundHeight < wallHeightBound)
                        {
                            wallCount++;
                        }
                        break;

                    case CollisionType_Unk2:
                        if (surface.groundType != GroundType_None)
                        {
                            groundType   = surface.groundType;
                            groundHeight = surface.groundHeight;
                        }
                        break;
                }
            }

            switch (collType)
            {
                case CollisionType_Wall:
                    if (wallCount < WALL_COUNT_THRESHOLD)
                    {
                        collResult->surface.groundHeight = chara->position.vy;
                    }
                    break;

                case CollisionType_Unk2:
                    if (groundType != GroundType_None)
                    {
                        collResult->surface.groundHeight = groundHeight;
                        collResult->surface.groundType   = GroundType_None;
                    }
                    break;
            }
            break;
    }

    return response;

    #undef POINT_COUNT
    #undef ANGLE_STEP
    #undef WALL_COUNT_THRESHOLD
    #undef WALL_HEIGHT
}

void Collision_GroundProbeRadial(s_CollisionResult* collResult, const VECTOR3* pos,
                                 q19_12 startGroundHeight, q19_12 startHeadingAngle) // 0x80069DF0
{
    #define POINT_COUNT 16
    #define ANGLE_STEP  Q12_ANGLE(360.0f / POINT_COUNT)

    q19_12             groundHeights[POINT_COUNT];
    s_CollisionSurface surface;
    q19_12             angle;
    s32                var_a0;
    q19_12             groundHeight;
    s32                var_s0;
    s32                i;
    q19_12             groundHeightMax;
    q19_12             groundHeightMin;
    s32                lowestGroundHeightIdx;

    groundHeightMin       = Q12(-30.0f);
    groundHeightMax       = Q12(30.0f);
    lowestGroundHeightIdx = 0;

    // Collect ground heights around position?
    for (i = 0; i < POINT_COUNT; i++)
    {
        Collision_SurfaceGet(&surface,
                             pos->vx + Math_Sin((startHeadingAngle & 0xF) + (i * ANGLE_STEP)),
                             pos->vz + Math_Cos((startHeadingAngle & 0xF) + (i * ANGLE_STEP)));
        groundHeights[i] = surface.groundHeight;

        if (groundHeightMin < surface.groundHeight)
        {
            groundHeightMin       = surface.groundHeight;
            lowestGroundHeightIdx = i;
        }

        if (surface.groundHeight < groundHeightMax)
        {
            groundHeightMax = surface.groundHeight;
        }
    }

    groundHeight = (groundHeightMin + groundHeightMax) >> 1; // `/ 2`.
    if (groundHeight < (startGroundHeight - INTERSECTION_BUFFER))
    {
        groundHeight = startGroundHeight - INTERSECTION_BUFFER;
    }

    for (i = lowestGroundHeightIdx + 1, var_a0 = lowestGroundHeightIdx;
         i < (lowestGroundHeightIdx + POINT_COUNT) && groundHeight < groundHeights[i & 0xF];
         i++)
    {
        var_a0 = i;
    }

    for (i = lowestGroundHeightIdx - 1, var_s0 = lowestGroundHeightIdx;
         i < (lowestGroundHeightIdx - POINT_COUNT) && groundHeight < groundHeights[i & 0xF];
         i--)
    {
        var_s0 = i;
    }

    angle = ((var_s0 + var_a0) << 8) >> 1;
    collResult->offset.vx = Q12_MULT_PRECISE(Math_Sin(angle), Q12(1.0f / 16.0f));
    collResult->offset.vz = Q12_MULT_PRECISE(Math_Cos(angle), Q12(1.0f / 16.0f));

    #undef POINT_COUNT
    #undef ANGLE_STEP
}

bool Collision_CharaCollisionSetup(s_CollisionResult* collResult, const VECTOR3* moveOffset, s_SubCharacter* chara) // 0x80069FFC
{
    s_CollisionCylinder cylinder;
    VECTOR3             offsetCpy;
    s32                 collDataIdx;
    s32                 charaCount;
    bool                cond;

    // Set collision cylinder position.
    cylinder.position.vx = chara->position.vx + chara->collision.shapeOffsets.cylinder.vx;
    cylinder.position.vy = chara->position.vy - Q12(0.02f);
    cylinder.position.vz = chara->position.vz + chara->collision.shapeOffsets.cylinder.vz;

    if (Ipd_CollisionDataGet(chara->position.vx, chara->position.vz) == NULL)
    {
        Collision_DefaultResultSet(collResult, Q12(0.0f), Q12(0.0f), Q12(0.0f), Q12(8.0f));
        return true;
    }

    // Set collision cylinder state.
    cylinder.top            = chara->collision.box.top;
    cylinder.bottom         = chara->collision.box.bottom;
    cylinder.radius         = chara->collision.cylinder.radius;
    cylinder.collisionState = chara->collision.state;

    offsetCpy = *moveOffset;

    switch (chara->model.charaId)
    {
        case Chara_Harry:
        case Chara_Groaner:
        case Chara_Wormhead:
        case Chara_Romper:
            cond = true;
            break;

        default:
            cond = false;
            break;
    }

#ifdef SH_PC_PORT
    /* MIPS evaluates args left-to-right so Ipd_ActiveChunksCollisionDataGet /
     * Collision_CollidableCharasGet run (setting collDataIdx/charaCount) before
     * those counts are read as later args. x86-64 evaluates right-to-left, so the
     * counts would be read uninitialized. Hoist the calls into locals first. */
    s_IpdCollisionData** _collDataPtrs = Ipd_ActiveChunksCollisionDataGet(&collDataIdx);
    s_SubCharacter**     _charas       = Collision_CollidableCharasGet(&charaCount, chara, true);
    return func_8006A4A8(collResult, &offsetCpy, &cylinder, cond,
                         _collDataPtrs, collDataIdx, NULL, 0,
                         _charas, charaCount);
#else
    return func_8006A4A8(collResult, &offsetCpy, &cylinder, cond,
                         Ipd_ActiveChunksCollisionDataGet(&collDataIdx), collDataIdx, NULL, 0,
                         Collision_CollidableCharasGet(&charaCount, chara, true), charaCount);
#endif
}

void Collision_DefaultResultSet(s_CollisionResult* collResult, q19_12 offsetX, q19_12 offsetY, q19_12 offsetZ, q19_12 groundHeight) // 0x8006A178
{
    collResult->offset.vx            = offsetX;
    collResult->offset.vy            = offsetY;
    collResult->offset.vz            = offsetZ;
    collResult->surface.tiltAngleZ   = Q12_ANGLE(0.0f);
    collResult->surface.tiltAngleX   = Q12_ANGLE(0.0f);
    collResult->surface.groundType   = GroundType_Default;
    collResult->ceilingHeight        = Q12(DEFAULT_CEILING_HEIGHT);
    collResult->surface.groundHeight = groundHeight;
}

s_SubCharacter** Collision_CollidableCharasGet(s32* collCharaCount, const s_SubCharacter* excludedChara, bool includePlayer) // 0x8006A1A4
{
    s_SubCharacter*         curChara;
    static s_SubCharacter*  collCharas[NPC_COUNT_MAX + 1]; /** Enough for all NPCs and player. */
    static s_SubCharacter** curCollChara;                  /** Array of active characters. */

    // Filter invalid case.
    if (excludedChara != NULL &&
        (excludedChara->model.charaId == Chara_None ||
         excludedChara->collision.state == CharaCollisionState_Ignore ||
        (excludedChara->collision.state == CharaCollisionState_Player && includePlayer == true)))
    {
        *collCharaCount = 0;
        return &collCharas;
    }

    *collCharaCount = 0;
    curCollChara    = &collCharas;

    // Collect collidable NPCs.
    for (curChara = &g_SysWork.npcs[0]; curChara < &g_SysWork.npcs[ARRAY_SIZE(g_SysWork.npcs)]; curChara++)
    {
        if (curChara->model.charaId != Chara_None)
        {
            if (curChara->collision.state != CharaCollisionState_Ignore &&
                (curChara->collision.state != CharaCollisionState_Player || includePlayer != true) &&
                curChara != excludedChara &&
                (includePlayer != true || excludedChara == NULL ||
                 excludedChara->collision.state != CharaCollisionState_4 ||
                 curChara->collision.state >= excludedChara->collision.state))
            {
                *collCharaCount += 1;
                *curCollChara    = curChara;
                curCollChara++;
                curChara->collision.field_E0 = 0;
            }
        }
    }

    // Collect collidable player.
    curChara = &g_SysWork.playerWork.player;
    if (curChara->model.charaId != Chara_None)
    {
        if (curChara->collision.state != CharaCollisionState_Ignore &&
            (curChara->collision.state != CharaCollisionState_Player || includePlayer != true) &&
            curChara != excludedChara &&
            (includePlayer != true || excludedChara == NULL ||
             excludedChara->collision.state != CharaCollisionState_4 ||
             curChara->collision.state >= excludedChara->collision.state))
        {
            *collCharaCount += 1;
            *curCollChara    = curChara;
            curCollChara++;
            curChara->collision.field_E0 = 0;
        }
    }

    return &collCharas;
}

s32 Collision_OffsetCheck(s_CollisionResult* collResult, VECTOR* offset, const s_CollisionCylinder* cylinder) // 0x8006A3B4
{
    s32 stackPtr;
    s32 var1;

    stackPtr = SetSp(PSX_SCRATCH_ADDR(0x3D8));
    var1 = func_8006A42C(collResult, offset, cylinder);
    SetSp(stackPtr);

    if (var1 == NO_VALUE)
    {
        var1 = 1;
    }

    return var1;
}

s32 func_8006A42C(s_CollisionResult* collResult, const VECTOR3* offset, const s_CollisionCylinder* cylinder) // 0x8006A42C
{
    VECTOR3 offsetCpy;
    s32     collDataIdx;

    offsetCpy = *offset;

#ifdef SH_PC_PORT
    /* See note in Collision_CharaCollisionSetup: hoist so collDataIdx is set
     * before it's read as a later arg (x86-64 right-to-left arg evaluation). */
    s_IpdCollisionData** _collDataPtrs = Ipd_ActiveChunksCollisionDataGet(&collDataIdx);
    return func_8006A4A8(collResult, &offsetCpy, cylinder, false,
                         _collDataPtrs, collDataIdx, NULL, 0, NULL, 0);
#else
    return func_8006A4A8(collResult, &offsetCpy, cylinder, false,
                         Ipd_ActiveChunksCollisionDataGet(&collDataIdx), collDataIdx, NULL, 0, NULL, 0);
#endif
}

bool func_8006A4A8(s_CollisionResult* collResult, VECTOR3* moveOffset, const s_CollisionCylinder* cylinder, bool arg3,
                   s_IpdCollisionData** collDataPtrs, s32 collDataIdx, s_func_8006CF18* arg6, s32 arg7,
                   s_SubCharacter** charas, s32 charaCount) // 0x8006A4A8
{
    s_CollisionState     state;
    VECTOR3              sp120; // Q19.12
    VECTOR3              moveOffset1;
    VECTOR3              moveOffsetCpy;
    s32                  var_a0;
    s32                  i;
    bool                 cond;
    q19_12               groundHeight;
    s32                  count;
    s_SubCharacter**     curChara;
    s_IpdCollisionData** curCollData;
    s_SubCharacter*      chara;

#ifdef SH_PC_PORT
    /* Zero-init collision state to prevent a heisenbug from uninitialized stack. */
    memset(&state, 0, sizeof(state));
#endif

    cond = false;

    if (cylinder->collisionState == CharaCollisionState_5)
    {
        Collision_DefaultResultSet(collResult, moveOffset->vx, moveOffset->vy, moveOffset->vz, cylinder->position.vy);
        return false;
    }

    Collision_TargetCharaCollidingSlowDown(moveOffset, cylinder, charas, charaCount);

    moveOffsetCpy = *moveOffset;
    collResult->ceilingHeight = Collision_CeilingHeightGet(&moveOffsetCpy, cylinder, cylinder->radius, cylinder->top);

    Collision_CollStateInit(&state, &moveOffsetCpy, cylinder, arg3);

    moveOffset1 = moveOffsetCpy;

    collResult->offset.vz = Q12(0.0f);
    collResult->offset.vx = Q12(0.0f);
    collResult->offset.vy = moveOffsetCpy.vy;

    while (true)
    {
        if (state.field_0_0)
        {
            state.isCharaMoving = state.charaState.distance != Q12(0.0f);
            state.field_0_9     = false;
            state.field_0_10    = false;
        }
        else
        {
            state.isCharaMoving = state.charaState.distance != Q12(0.0f);
            state.field_0_9     = true;
            state.field_0_10    = true;
        }

        // Run through collision data.
#ifdef SH_PC_PORT
        g_WallStopDbg.active = 0;
#endif
        for (curCollData = collDataPtrs; curCollData < &collDataPtrs[collDataIdx]; curCollData++)
        {
            Collision_CharaCollisionHandle(&state, *curCollData);
        }

        if (state.field_44.field_0.field_0 && state.field_44.field_0.field_2.vx == state.field_44.field_0.field_2.vy)
        {
            cond |= true;
        }

        func_8006CF18(&state, arg6, arg7);

        // Run through characters.
        for (curChara = charas; curChara < &charas[charaCount]; curChara++)
        {
            chara  = *curChara;
            // TODO: Wrong. `chara->collision.cylinder.radius` is `q9_12` while `state.charaState.radius`
            // is `q7_8`. Could be division?
            var_a0 = FP_FROM(chara->collision.cylinder.radius, Q4_SHIFT) + state.charaState.radius;

            if (chara->collision.state < (u32)state.charaState.collisionState)
            {
                var_a0 -= 15;
            }

            state.charaPositionFrom.field_0 = Q12_TO_Q8(chara->position.vx + chara->collision.shapeOffsets.cylinder.vx);
            state.charaPositionTo.field_0   = Q12_TO_Q8(chara->position.vz + chara->collision.shapeOffsets.cylinder.vz);

            state.field_A0.s_1.field_0        = Q12_TO_Q8(chara->collision.box.top    + chara->position.vy);
            state.field_A0.s_1.field_2        = Q12_TO_Q8(chara->collision.box.bottom + chara->position.vy);
            state.field_A0.s_1.field_4        = var_a0;
            state.field_A0.s_1.collisionState = chara->collision.state;
            state.field_A0.s_1.field_8        = &chara->collision.field_E0;

            if (state.field_0_0 == false)
            {
                chara->collision.field_E0 = 0;
            }

            func_8006CC9C(&state);
            func_8006CF18(&state, chara->collision.field_E4, chara->collision.field_E1_4);
        }

        func_8006D01C(&sp120, &moveOffset1, Collision_OffsetAlphaGet(&state), &state);

        collResult->offset.vx += sp120.vx;
        collResult->offset.vz += sp120.vz;

#ifdef SH_PC_PORT
        /* collState inspector: snapshot the resolved collision state for the
         * overlay's collState panel, gated to the player's collision pass (query
         * chara near Harry) so the panel tracks Harry, not NPCs. Field names track
         * the modular s_CollisionState: radius=charaState.radius, dist=point.field_20.
         * radiusCollDiffDist, faceIdx=point.subcellIdx, groundFlag=heightDisabled. */
        if (g_CollVisEnabled)
        {
            /* cylinder is the collision query (Harry's collision cylinder), always
             * valid; `chara` is the charas-loop local and is NULL/stale here. */
            s32 _pdx     = cylinder->position.vx - g_SysWork.playerWork.player.position.vx;
            s32 _pdz     = cylinder->position.vz - g_SysWork.playerWork.player.position.vz;
            s32 _dist    = state.point.field_20.radiusCollDiffDist;
            s32 _rad     = state.charaState.radius;
            int _contact = (_rad > 0 && _dist < _rad) || state.field_44.field_0.field_0;
            /* Harry oscillates in/out of contact every frame (wall response pushes
             * him out, he walks back in); latch a snapshot for ~0.5s so the panel
             * holds the colliding values instead of blinking to the idle face. */
            if (ABS(_pdx) < Q12(4.0f) && ABS(_pdz) < Q12(4.0f) &&
                (_contact || g_CollStateDbg.hold == 0))
            {
                g_CollStateDbg.valid      = 1;
                g_CollStateDbg.dist       = _dist;
                g_CollStateDbg.radius     = _rad;
                g_CollStateDbg.respActive = state.field_44.field_0.field_0;
                g_CollStateDbg.angMin     = state.field_44.field_0.field_2.vx;
                g_CollStateDbg.angMax     = state.field_44.field_0.field_2.vy;
                g_CollStateDbg.faceIdx    = state.point.subcellIdx;
                g_CollStateDbg.pushX      = collResult->offset.vx;
                g_CollStateDbg.pushZ      = collResult->offset.vz;
                g_CollStateDbg.groundFlag = state.heightDisabled;
                g_CollStateDbg.groundType = state.groundType;
                g_CollStateDbg.corner     = cond;
                if (_contact)
                {
                    g_CollStateDbg.hold = 30; /* ~0.5s at 60fps */
                }
            }
        }

        /* [WALLSTOP] read: a block actually clamped Harry's move this pass
         * (g_WallStopDbg set by one of the three block sites). Player-gated via
         * cylinder-near-Harry + throttled. Captures the REAL stop the [WALL-HIT]
         * proximity probe misses — especially kind=3 (round obstacle, otherwise
         * invisible). kind 1/2=wall: a/b = split-face XZ endpoints, dist = perp
         * distance, rad = body radius, frac = swept crossing (Q12, kind 1).
         * kind 3=obstacle: a = obstacle XZ, b = (obstY, obstRadius), rad = body+obst
         * reach, dist = center distance, frac = swept crossing. */
        {
            s32 _wpdx = cylinder->position.vx - g_SysWork.playerWork.player.position.vx;
            s32 _wpdz = cylinder->position.vz - g_SysWork.playerWork.player.position.vz;
            static s32 s_lastStopLog = -1000;
            if (g_WallStopDbg.active && (g_TickCount - s_lastStopLog) > 8 &&
                ABS(_wpdx) < Q12(4.0f) && ABS(_wpdz) < Q12(4.0f))
            {
                s_lastStopLog = g_TickCount;
                SH_DBG("[WALLSTOP] kind=%d sub=%d a=(%d,%d) b=(%d,%d) rad=%d dist=%d frac=%d sweepDist=%d spd=%d world=(%d,%d) head=%d",
                       g_WallStopDbg.kind, g_WallStopDbg.sub,
                       g_WallStopDbg.ax, g_WallStopDbg.az, g_WallStopDbg.bx, g_WallStopDbg.bz,
                       g_WallStopDbg.rad, g_WallStopDbg.dist, g_WallStopDbg.frac,
                       (int)state.charaState.distance,
                       (int)g_SysWork.playerWork.player.moveSpeed,
                       (int)g_SysWork.playerWork.player.position.vx, (int)g_SysWork.playerWork.player.position.vz,
                       (int)g_SysWork.playerWork.player.rotation.vy);
                SH_DBG("[WALLSTOP]   off=(%d,%d) full=(%d,%d) wrap=%d cellWH=(%d,%d) fall=%d hp=%d",
                       g_WallStopDbg.offx, g_WallStopDbg.offz, g_WallStopDbg.fullx, g_WallStopDbg.fullz,
                       (g_WallStopDbg.fullx != g_WallStopDbg.offx) || (g_WallStopDbg.fullz != g_WallStopDbg.offz),
                       g_WallStopDbg.cellw, g_WallStopDbg.cellh,
                       (int)g_SysWork.playerWork.player.fallSpeed,
                       (int)g_SysWork.playerWork.player.health);
            }
        }

        /* [WALL-HIT] (#42 invisible walls): on cylinder contact, name the
         * face — subcell index, its EVENT CHANNEL vs the active trigger
         * flags word (func_8006B318 gates each subcell on
         * flags >> (field_0_14*4 | field_2_14)), and both surfaces' ground
         * type + solid (disableHeight) bits. NOT visualizer-gated: user
         * reports must capture it (SilentHill(29).log had the bump but no
         * line because the first version lived inside g_CollVisEnabled). */
        {
            s32 _pdx     = cylinder->position.vx - g_SysWork.playerWork.player.position.vx;
            s32 _pdz     = cylinder->position.vz - g_SysWork.playerWork.player.position.vz;
            s32 _dist    = state.point.field_20.radiusCollDiffDist;
            s32 _rad     = state.charaState.radius;
            int _contact = (_rad > 0 && _dist < _rad) || state.field_44.field_0.field_0;

            {
                static s32 s_lastWallLog = -1000;
                if (_contact && (g_TickCount - s_lastWallLog) > 15 &&
                    ABS(_pdx) < Q12(4.0f) && ABS(_pdz) < Q12(4.0f) &&
                    state.point.ipdCollisionData != NULL &&
                    state.point.ipdCollisionData->subcells != NULL)
                {
                    const s_IpdCollisionData* cd = state.point.ipdCollisionData;
                    s32                       sci = state.point.subcellIdx;
                    const s_IpdCollSubcell*   sc  = &cd->subcells[sci];
                    s32 chan = (sc->field_0_14 * 4) | sc->field_2_14;
                    s32 s0   = state.point.field_C.cellSurfaces.surfaceIdx0;
                    s32 s1   = state.point.field_C.cellSurfaces.surfaceIdx1;

                    s_lastWallLog = g_TickCount;
                    SH_DBG("[WALL-HIT] subcell=%d chan=%d flags=0x%04X s0=%d(t%d d%d) s1=%d(t%d d%d) gt=%d dist=%d rad=%d",
                           sci, chan, (unsigned)g_ActiveCollisionTriggers.flags,
                           s0,
                           (s0 != UCHAR_MAX && cd->surfaces != NULL) ? (int)cd->surfaces[s0].groundType    : -1,
                           (s0 != UCHAR_MAX && cd->surfaces != NULL) ? (int)cd->surfaces[s0].disableHeight : -1,
                           s1,
                           (s1 != UCHAR_MAX && cd->surfaces != NULL) ? (int)cd->surfaces[s1].groundType    : -1,
                           (s1 != UCHAR_MAX && cd->surfaces != NULL) ? (int)cd->surfaces[s1].disableHeight : -1,
                           (int)state.groundType, _dist, _rad);
                    /* Speed-dependence triage: contact only happens at top run
                     * speed (walkable otherwise), so the deciding variables are
                     * VERTICAL — the cylinder's span vs the face's span at the
                     * moment of contact (Q8). If bottom sits below the face top
                     * only while moving fast, Harry's height is lagging the
                     * ground across the step and clipping the riser. */
                    SH_DBG("[WALL-HIT]   cylBot=%d cylTop=%d sv0y=%d sv1y=%d harryY=%d ground=%d fall=%d spd=%d",
                           (int)state.charaState.bottomPos, (int)state.charaState.topPos,
                           (int)state.point.splitVertex0.vy, (int)state.point.splitVertex1.vy,
                           (int)Q12_TO_Q8(g_SysWork.playerWork.player.position.vy),
                           (int)Q12_TO_Q8(g_SysWork.playerWork.player.properties.player.groundHeight),
                           (int)g_SysWork.playerWork.player.fallSpeed,
                           (int)g_SysWork.playerWork.player.moveSpeed);
                    /* Swept-vs-real disambiguation (#42): the response fires from
                     * the MOVING path (func_8006BE40/BF88), which tests Harry's
                     * per-frame movement LINE (charaState.distance) against the
                     * wall segment — not his body radius. Log the segment's world
                     * XZ endpoints, Harry's from-XZ, and his swept move vector so
                     * the sweep can be reconstructed: if the line genuinely crosses
                     * a real wall ahead the wall is invisible-but-real; if the
                     * sweep is abnormally long for the speed, the PC per-frame
                     * distance over-reaches (lower-body movement shim). */
                    SH_DBG("[WALL-HIT]   harryXZ=(%d,%d) moveXZ=(%d,%d) sweepDist=%d face0=(%d,%d) face1=(%d,%d)",
                           (int)state.charaPositionFrom.offset.vx, (int)state.charaPositionFrom.offset.vz,
                           (int)state.point.field_20.charaMoveOffset.vx, (int)state.point.field_20.charaMoveOffset.vz,
                           (int)state.charaState.distance,
                           (int)state.point.splitVertex0.vx, (int)state.point.splitVertex0.vz,
                           (int)state.point.splitVertex1.vx, (int)state.point.splitVertex1.vz);
                }
            }
        }
#endif /* SH_PC_PORT (collState inspector + [WALL-HIT]) */

        if (state.field_0_0)
        {
            break;
        }

        if (state.field_44.field_0.field_0 || state.field_44.field_30.field_0)
        {
            count = 8;
            if (state.field_44.field_8.field_0 < 9)
            {
                count = state.field_44.field_8.field_0;
            }

            for (i = 0; i < count; i++)
            {
                *state.field_44.field_10[i] += 1;
            }
        }
        else if (state.field_34)
        {
            *state.field_40 += 1;
        }
        else
        {
            break;
        }

        state.field_0_0 = true;
        func_8006D774(&state, &sp120, &moveOffset1);
    }

    if (state.heightDisabled == true)
    {
        groundHeight                   = Q12(8.0f);
        collResult->surface.groundType = GroundType_Default;
    }
    else
    {
        collResult->surface.groundType = state.groundType;
        groundHeight                   = Q8_TO_Q12(Ipd_GroundHeightGet(state.charaState.positionFromX + Q12_TO_Q8(sp120.vx),
                                                                       state.charaState.positionFromZ + Q12_TO_Q8(sp120.vz), &state));
    }

    collResult->surface.groundHeight = groundHeight;
    collResult->surface.tiltAngleX   = state.tiltAngleX;
    collResult->surface.tiltAngleZ   = state.tiltAngleZ;

    if (cond)
    {
        return NO_VALUE;
    }

    return state.field_0_0 != false;
}

void Collision_TargetCharaCollidingSlowDown(VECTOR3* offset, const s_CollisionCylinder* cylinder, s_SubCharacter** charas, s32 charaCount) // 0x8006A940
{
    q19_12          headingAngle;
    q19_12          cylinderOffsetZ;
    q19_12          cylinderOffsetX;
    q19_12          var_a0;
    s32             i;
    q19_12          offsetAlpha;
    q19_12          var_v0;
    s32             dist;
    q19_12          curCharaTop;
    q19_12          curCharaBottom;
    q19_12          otherCharaBottom;
    q19_12          otherCharaTop;
    s_SubCharacter* curChara;

    offsetAlpha  = Q12(1.0f);
    headingAngle = ratan2(offset->vx, offset->vz);

    // Run through characters to collide.
    for (i = 0; i < charaCount; i++)
    {
        curChara = charas[i];

        // Check if current character is collidable.
        if (curChara->collision.state == CharaCollisionState_Ignore ||
            curChara->collision.state == CharaCollisionState_Player ||
            curChara->collision.state >= cylinder->collisionState)
        {
            continue;
        }

        // Check if cylinders collide on vertical axis using box top and bottom.
        curCharaTop      = curChara->collision.box.top    + curChara->position.vy;
        curCharaBottom   = curChara->collision.box.bottom + curChara->position.vy;
        otherCharaTop    = cylinder->top                  + cylinder->position.vy;
        otherCharaBottom = cylinder->bottom               + cylinder->position.vy;
        if (curCharaTop    > otherCharaBottom ||
            curCharaBottom < otherCharaTop)
        {
            continue;
        }

        cylinderOffsetX = (curChara->position.vx + curChara->collision.shapeOffsets.cylinder.vx) - cylinder->position.vx;
        cylinderOffsetZ = (curChara->position.vz + curChara->collision.shapeOffsets.cylinder.vz) - cylinder->position.vz;
        
        // Check if cylinders collide on XZ plane.
        dist = Vc_VectorMagnitudeCalc(cylinderOffsetX, Q12(0.0f), cylinderOffsetZ);
        if (((curChara->collision.cylinder.radius + cylinder->radius) + INTERSECTION_BUFFER) < dist)
        {
            continue;
        }

        // TODO: Check what this is doing. Computes a slowdown alpha based on the angle at which the cylinders collided?
        var_a0 = Q12_MULT(Math_Cos(ratan2(cylinderOffsetX, cylinderOffsetZ) - headingAngle), Q12(1.5f));
        var_v0 = MAX(var_a0, Q12(0.0f));
        var_a0 = var_v0;
        if (curChara->model.charaId == Chara_HangedScratcher)
        {
            var_a0 = MIN(var_a0, Q12(0.6f));
        }
        else
        {
            var_a0 = MIN(var_a0, Q12(0.4f));
        }

        offsetAlpha -= var_a0;
    }

    // Adjust displacement offset.
    offsetAlpha = MAX(offsetAlpha, Q12(0.4f));
    offset->vx  = Q12_MULT(offsetAlpha, offset->vx);
    offset->vz  = Q12_MULT(offsetAlpha, offset->vz);
}

void Collision_CollStateInit(s_CollisionState* state, VECTOR3* moveOffset, const s_CollisionCylinder* cylinder, bool arg3) // 0x8006AB50
{
    state->field_0_0          = false;
    state->flags              = g_ActiveCollisionTriggers.flags;
    state->charaState.field_4 = arg3;

    Collision_MoveDirectionCalc(&state->charaState, moveOffset, cylinder);

    state->slopedGroundHeight = Q8(30.0f); // TODO: 30 metres might be a default constant. Also seen in `Collision_OffsetAlphaGet`.
    state->field_34 = 0;
    
    state->field_44.field_0.field_0  = 0;
    state->field_44.field_6          = 0;
    state->field_44.field_8.field_0  = 0;
    state->field_44.field_36         = 0;
    state->field_44.field_30.field_0 = 0;
    
    state->tiltAngleZ     = Q12_ANGLE(0.0f);
    state->tiltAngleX     = Q12_ANGLE(0.0f);
    state->heightDisabled = true;
    state->groundType     = GroundType_Default;
}

void Collision_MoveDirectionCalc(s_CollisionCharaState* charaState,
                                 const VECTOR3* moveOffset, const s_CollisionCylinder* cylinder) // 0x8006ABC0
{
    q3_12 headingAngle;

    charaState->offset.vx = Q12_TO_Q8(moveOffset->vx);
    charaState->offset.vy = Q12_TO_Q8(moveOffset->vy);
    charaState->offset.vz = Q12_TO_Q8(moveOffset->vz);

    charaState->distance = Math_Vector2MagCalc(charaState->offset.vx, charaState->offset.vz);
    if (charaState->distance != Q12(0.0f))
    {
        // @unused
        charaState->direction.vx = Q12(charaState->offset.vx) / charaState->distance;
        charaState->direction.vz = Q12(charaState->offset.vz) / charaState->distance;

        headingAngle             = ratan2(charaState->offset.vz, charaState->offset.vx);
        charaState->direction.vx = Math_Cos(headingAngle);
        charaState->direction.vz = Math_Sin(headingAngle);
    }
    else
    {
        charaState->direction.vx = Q12(1.0f);
        charaState->direction.vz = Q12(0.0f);
    }

    charaState->radius         = Q12_TO_Q8(cylinder->radius);
    charaState->positionFromX  = Q12_TO_Q8(cylinder->position.vx);
    charaState->positionFromZ  = Q12_TO_Q8(cylinder->position.vz);
    charaState->positionToX    = charaState->positionFromX + charaState->offset.vx;
    charaState->positionToZ    = charaState->positionFromZ + charaState->offset.vz;
    charaState->topPos         = Q12_TO_Q8(cylinder->top    + cylinder->position.vy);
    charaState->bottomPos      = Q12_TO_Q8(cylinder->bottom + cylinder->position.vy);
    charaState->collisionState = cylinder->collisionState;
}

void Collision_CharaCollisionHandle(s_CollisionState* state, s_IpdCollisionData* collData) // 0x8006AD44
{
    s32                    endIdx;
    s32                    startIdx;
    s32                    i;
    s32                    j;
    s_IpdCollSubcellRange* curSubcellRanges;

    // Check if IPD collision contains necessary information and the current subcell is accessible.
    if ((collData->surfaceCount == 0 && collData->subcellCount == 0 && collData->field_8_24 == 0) ||
        !Collision_SubcellInit(state, collData))
    {
        return;
    }

#ifdef SH_PC_PORT
    /* Guard against NULL subcellRanges or zero subcellCountX (cell width), which
     * would cause out-of-bounds access or an infinite loop in the grid walk below. */
    if (collData->subcellRanges == NULL || collData->subcellCountX == 0)
    {
        return;
    }
#endif

    if (!state->field_0_0)
    {
        Collision_SubcellChecksReset(collData);
    }

    startIdx = state->field_A0.s_0.closestXSubcellIdx;
    endIdx   = (state->field_A0.s_0.closestXSubcellIdx + state->field_A0.s_0.closeFarXSubcellIdxDiff) - 1;

#ifdef SH_PC_PORT
    /* Bounds-check the grid indices to prevent OOB access into subcellRanges
     * (subcellCountX * subcellCountZ entries). The flat index i*subcellCountX + j
     * must stay in [0, maxIdx). (PSX never overran because the data always fit;
     * on PC a partially-loaded/garbage chunk can violate this.) */
    {
        s32 firstIdx_i = state->field_A0.s_0.closestZSubcellIdx;
        s32 loopEnd_i  = firstIdx_i + state->field_A0.s_0.closeFarZSubcellIdxDiff;
        if (startIdx < 0 || endIdx < 0 ||
            startIdx >= collData->subcellCountX || endIdx >= collData->subcellCountX ||
            firstIdx_i < 0 || loopEnd_i > collData->subcellCountZ)
        {
            goto ad44_skip_grid;
        }
    }
#endif

    for (i = state->field_A0.s_0.closestZSubcellIdx;
         i < (state->field_A0.s_0.closestZSubcellIdx + state->field_A0.s_0.closeFarZSubcellIdxDiff);
         i++)
    {
        curSubcellRanges = &collData->subcellRanges[(i * collData->subcellCountX) + startIdx];

        for (j = startIdx; j <= endIdx; j++, curSubcellRanges++)
        {
            func_8006B1C8(state, collData, curSubcellRanges);
        }
    }

#ifdef SH_PC_PORT
ad44_skip_grid: ;
#endif

    if (state->field_0_0 == false)
    {
        Collision_SubcellChecksCountUpdate(collData);
    }

    // TODO: Something that hangles height and movement on slopes.
    if (state->field_0_10)
    {
        func_8006C838(state, collData);
        if (state->field_A0.s_0.subcellRange != NULL)
        {
            func_8006CA18(state, collData, state->field_A0.s_0.subcellRange);
        }
    }
}

bool Collision_SubcellInit(s_CollisionState* state, const s_IpdCollisionData* collData) // 0x8006AEAC
{
    s_CollisionState_A8* curUnk;

    if (!Collision_SubcellIdxGet(state, collData))
    {
        return false;
    }

    state->charaPositionFrom.offset.vx = state->charaState.positionFromX - collData->positionX;
    state->charaPositionFrom.offset.vz = state->charaState.positionFromZ - collData->positionZ;
    state->charaPositionTo.offset.vx   = state->charaState.positionToX   - collData->positionX;
    state->charaPositionTo.offset.vz   = state->charaState.positionToZ   - collData->positionZ;

#ifdef SH_PC_PORT
    /* Guard against divide-by-zero on the collision cell size (subcellSize). */
    if (collData->subcellSize == 0)
    {
        state->field_A0.s_0.subcellRange = NULL;
        return true;
    }
#endif

    if ((state->charaPositionFrom.offset.vx / collData->subcellSize) < 0 || (state->charaPositionFrom.offset.vx / collData->subcellSize) >= collData->subcellCountX ||
        ((state->charaPositionFrom.offset.vz / collData->subcellSize) < 0) || (state->charaPositionFrom.offset.vz / collData->subcellSize) >= collData->subcellCountZ)
    {
        state->field_A0.s_0.subcellRange = NULL;
    }
    else
    {
        state->field_A0.s_0.subcellRange = &collData->subcellRanges[((state->charaPositionFrom.offset.vz / collData->subcellSize) * collData->subcellCountX) +
                                                                        (state->charaPositionFrom.offset.vx / collData->subcellSize)];
        state->subcellIdx                = NO_VALUE;

        for (curUnk = &state->field_A0.s_0.field_8[0]; curUnk < &state->field_A0.s_0.field_8[4]; curUnk++)
        {
            curUnk->subChunkTransDir   = SubChunkTransitionDirection_None;
            curUnk->surfaceIdx         = NO_VALUE;
            curUnk->radiusCollDiffDist = INT_MAX;
        }
    }

    return true;
}

bool Collision_SubcellIdxGet(s_CollisionState* state, const s_IpdCollisionData* collData) // 0x8006B004
{
    q23_8 charaCollDiffZClosest;
    q23_8 charaCollDiffXClosest;
    q23_8 collCellXSize;
    q23_8 collCellZSize;
    q23_8 collCellXLimit;
    q23_8 collCellZLimit;
    q23_8 charaCollDiffZFarest;
    q23_8 charaCollDiffXFarest;
    
    collCellXSize  = collData->subcellSize * collData->subcellCountX;
    collCellXLimit = collCellXSize - 1;
    collCellZSize  = collData->subcellSize * collData->subcellCountZ;
    collCellZLimit = collCellZSize - 1;

    // Getting X plane differences.
    charaCollDiffXClosest = state->charaState.positionFromX - collData->positionX;
    charaCollDiffXFarest  = state->charaState.positionToX   - collData->positionX;
    if (charaCollDiffXFarest < charaCollDiffXClosest)
    {
        charaCollDiffXFarest  ^= charaCollDiffXClosest;
        charaCollDiffXClosest ^= charaCollDiffXFarest;
        charaCollDiffXFarest  ^= charaCollDiffXClosest;
    }
    charaCollDiffXClosest -= state->charaState.radius;
    charaCollDiffXFarest  += state->charaState.radius;


    // Getting Z plane differences.
    charaCollDiffZClosest = state->charaState.positionFromZ - collData->positionZ;
    charaCollDiffZFarest  = state->charaState.positionToZ   - collData->positionZ;
    if (charaCollDiffZFarest < charaCollDiffZClosest)
    {
        charaCollDiffZFarest  ^= charaCollDiffZClosest;
        charaCollDiffZClosest ^= charaCollDiffZFarest;
        charaCollDiffZFarest  ^= charaCollDiffZClosest;
    }
    charaCollDiffZClosest -= state->charaState.radius;
    charaCollDiffZFarest  += state->charaState.radius;

    if (collCellXSize < charaCollDiffXClosest ||
        collCellZSize < charaCollDiffZClosest ||
        charaCollDiffXFarest < 0 ||
        charaCollDiffZFarest < 0)
    {
        return false;
    }

    charaCollDiffXClosest = limitRange(charaCollDiffXClosest, 0, collCellXLimit);
    charaCollDiffZClosest = limitRange(charaCollDiffZClosest, 0, collCellZLimit);
    charaCollDiffXFarest  = limitRange(charaCollDiffXFarest, 0, collCellXLimit);
    charaCollDiffZFarest  = limitRange(charaCollDiffZFarest, 0, collCellZLimit);

    state->field_A0.s_0.closestXSubcellIdx      = charaCollDiffXClosest / collData->subcellSize;
    state->field_A0.s_0.closestZSubcellIdx      = charaCollDiffZClosest / collData->subcellSize;
    state->field_A0.s_0.closeFarXSubcellIdxDiff = ((charaCollDiffXFarest / collData->subcellSize) -
                                                   state->field_A0.s_0.closestXSubcellIdx) + 1;
    state->field_A0.s_0.closeFarZSubcellIdxDiff = ((charaCollDiffZFarest / collData->subcellSize) -
                                                   state->field_A0.s_0.closestZSubcellIdx) + 1;

    return true;
}

void func_8006B1C8(s_CollisionState* state, s_IpdCollisionData* collData, s_IpdCollSubcellRange* subcellRanges) // 0x8006B1C8
{
    s32 subcellCount;
    s32 i;
    u8  idx;

    for (i = subcellRanges[0].field_0; i < subcellRanges[1].field_0; i++)
    {
        idx = collData->ptr_28[i];
        
        // Checks if cell element haven't been analysed.
        if (collData->subcellCheckCount >= collData->subcellCheckIdx[idx])
        {
            collData->subcellCheckIdx[idx] = collData->subcellCheckCount + 1;
            subcellCount                   = collData->subcellCount;
            
            // Checks if sub cell index is inside the defined sub cell information.
            if (idx < subcellCount)
            {
                if (func_8006B318(state, collData, idx))
                {
                    if (state->field_0_10)
                    {
                        func_8006B6E8(state, subcellRanges);
                    }

                    if (state->isCharaMoving || state->field_0_9)
                    {
                        if (state->point.field_C.cellSurfaces.surfaceIdx1 == UCHAR_MAX)
                        {
                            func_8006B9C8(state);
                        }

                        if (state->point.field_C.cellSurfaces.surfaceIdx0 == UCHAR_MAX)
                        {
                            func_8006B8F8(&state->point);
                            func_8006B9C8(state);
                        }
                    }
                }
            }
            else if (func_8006C3D4(state, collData, idx))
            {
                func_8006C45C(state);
            }
        }
    }
}

bool func_8006B318(s_CollisionState* state, const s_IpdCollisionData* collData, s32 subcellIdx) // 0x8006B318
{
    q23_8             charaVertDiffZ;
    q23_8             charaVertDiffX;
    s32               temp_v0;
    s_IpdCollSurface* curSurface;
    s_IpdCollSubcell* subcell;

    subcell = &collData->subcells[subcellIdx];
    
    /** @note Alternative collision enabling?
     * As noted briefly in `e_CollisionTriggerFlags` the game contains two flags (three, but one is unused)
     * which specifically enable certain collisions under specific game events, see for example the school
     * boss valve puzzle which relays in this flag system in order to enable and disable the collision that
     * blocks the player from progressing until the puzzle is solved.
     *
     * Claude (on Kush/AI port repo) indicates this is used to enable certain collision cells so likely this is
     * where that system relays on.
     */
    if (!((state->flags >> ((subcell->field_0_14 * 4) | subcell->field_2_14)) & (1 << 0)))
    {
        return false;
    }

    state->point.ipdCollisionData = collData;
    state->point.subcellIdx       = subcellIdx;

    state->point.field_6.vx = subcell->field_0_0;
    state->point.field_6.vy = subcell->field_2_0;
    state->point.field_6.vz = subcell->field_4;

    state->point.splitVertex0 = collData->splitVertices[subcell->splitVertexIdx1];
    state->point.splitVertex1 = collData->splitVertices[subcell->splitVertexIdx0];

    state->point.field_C.cellSurfaces.surfaceIdx0 = subcell->surfaceIdx0;

    if (state->point.field_C.cellSurfaces.surfaceIdx0 != UCHAR_MAX)
    {
        curSurface                         = &collData->surfaces[state->point.field_C.cellSurfaces.surfaceIdx0];
        state->point.field_E               = curSurface->field_6_8;
        state->point.disableSurface0Height = curSurface->disableHeight;

        if (state->charaState.field_4 &&
            (curSurface->disableHeight == true || curSurface->groundType == GroundType_None))
        {
            state->point.splitVertex0.vy -= Q8(-DEFAULT_CEILING_HEIGHT);
            state->point.splitVertex1.vy -= Q8(-DEFAULT_CEILING_HEIGHT);
        }
    }

    state->point.field_C.cellSurfaces.surfaceIdx1 = subcell->surfaceIdx1;

    if (state->point.field_C.cellSurfaces.surfaceIdx1 != UCHAR_MAX)
    {
        curSurface                         = &collData->surfaces[state->point.field_C.cellSurfaces.surfaceIdx1];
        state->point.field_F               = curSurface->field_6_8;
        state->point.disableSurface1Height = curSurface->disableHeight;

        if (state->charaState.field_4 &&
            (curSurface->disableHeight == true || curSurface->groundType == GroundType_None))
        {
            state->point.splitVertex0.vy = Q8(DEFAULT_CEILING_HEIGHT);
            state->point.splitVertex1.vy = Q8(DEFAULT_CEILING_HEIGHT);
        }
    }

    /* Will - 1: Important for dealing with heights or cells transition, in any case of changing any value given to `field_8`
       then harry start freaking out when going through subcells that have different heights or subcells that have two
       with one having a different alture as he may suddenly appear way above of them, this is more something done to handle
       the transition between cells because onces the player is in the cell with different height to the previous one they
       can walk normally.
    */
    temp_v0 = PACKED_XZ16(subcell->field_0_0, subcell->field_2_0);
    gte_ldR11R12(temp_v0);
    gte_ldR13R21(temp_v0);
    
    temp_v0 = PACKED_XZ16(state->charaState.offset.vx, state->charaState.offset.vz);
    gte_ldvxy0(temp_v0);
    gte_ldvz0();
    gte_rtv0();
    gte_stMAC12(&state->point.field_20.charaMoveOffset);
    /* Will - 1: ^ These values are normally set to 0 when standing still and are only modified when moving.
    
       Will - 2: I think this a distance, the value when set isn't too big and also I think `charaState.offset` is the
       character movement offset, like the equivalent of doing `charaPositionFrom.offset.vx - charaPositionTo.offset.vx`
       that may explain why it's 0 when standing still.
    */
    
    temp_v0 = PACKED_XZ16(state->charaPositionFrom.offset.vx - state->point.splitVertex0.vx,
                          state->charaPositionFrom.offset.vz - state->point.splitVertex0.vz);
    gte_ldvxy0(temp_v0);
    gte_ldvz0();
    gte_rtv0();
    gte_stMAC12(&state->point.field_20.charaVertDiff);

    charaVertDiffX                             = state->point.field_20.charaVertDiff.vx;
    charaVertDiffZ                             = state->point.field_20.charaVertDiff.vz;
    state->point.field_20.charaMoveVertDiff.vx = state->point.field_20.charaMoveOffset.vx + charaVertDiffX;
    state->point.field_20.charaMoveVertDiff.vz = state->point.field_20.charaMoveOffset.vz + charaVertDiffZ;

    if (charaVertDiffX < Q8(0.0f))
    {
        state->point.field_20.subChunkTransDir   = SubChunkTransitionDirection_X;
        state->point.field_20.radiusCollDiffDist = Math_Vector2MagCalc(charaVertDiffX, charaVertDiffZ);
        state->point.field_20.collDiffDist       = -charaVertDiffX;
    }
    else if (state->point.field_6.vz < charaVertDiffX)
    {
        state->point.field_20.subChunkTransDir   = SubChunkTransitionDirection_X;
        state->point.field_20.radiusCollDiffDist = Math_Vector2MagCalc(charaVertDiffX - state->point.field_6.vz, charaVertDiffZ);
        state->point.field_20.collDiffDist       = charaVertDiffX - state->point.field_6.vz;
    }
    else
    {
        if (charaVertDiffZ < Q8(0.0f))
        {
            state->point.field_20.radiusCollDiffDist = -charaVertDiffZ;
        }
        else
        {
            state->point.field_20.radiusCollDiffDist = charaVertDiffZ;
        }

        state->point.field_20.subChunkTransDir = SubChunkTransitionDirection_Z;
    }

#ifdef SH_PC_PORT
    /* Collision visualizer (' key): capture this contacted wall face in world
     * space for the red overlay. A face is a genuine contact when the player-to-
     * segment distance (point.field_20.radiusCollDiffDist) is within the player's
     * collision radius (charaState.radius) — the same test Collision_WallResponse
     * uses. World pos (Q12) = (cellOrigin + splitVertex) << 4 (data is Q8). The
     * full cell geometry (green) is cached separately by CollVis_CaptureCell. */
    if (g_CollVisEnabled &&
        state->charaState.radius > 0 &&
        state->point.field_20.radiusCollDiffDist < state->charaState.radius) {
        extern void CollVis_CaptureHit(s32 ax, s32 ay, s32 az, s32 bx, s32 by, s32 bz);
        s32       ox = collData->positionX;
        s32       oz = collData->positionZ;
        SVECTOR3* va = &collData->splitVertices[subcell->splitVertexIdx0];
        SVECTOR3* vb = &collData->splitVertices[subcell->splitVertexIdx1];
        CollVis_CaptureHit(
            (ox + va->vx) << 4, (s32)va->vy << 4, (oz + va->vz) << 4,
            (ox + vb->vx) << 4, (s32)vb->vy << 4, (oz + vb->vz) << 4);
    }
#endif

    return true;
}

void func_8006B6E8(s_CollisionState* state, s_IpdCollSubcellRange* subcellRanges) // 0x8006B6E8
{
    s32                  idx;
    s32                  surfaceIdx0;
    s32                  surfaceIdx1;
    bool                 disableSurface0Height;
    bool                 disableSurface1Height;
    s_CollisionState_A8* temp_s0;

    surfaceIdx0           = state->point.field_C.cellSurfaces.surfaceIdx0;
    surfaceIdx1           = state->point.field_C.cellSurfaces.surfaceIdx1;
    disableSurface0Height = state->point.disableSurface0Height;
    disableSurface1Height = state->point.disableSurface1Height;

    if (surfaceIdx0 == UCHAR_MAX)
    {
        if (surfaceIdx1 == UCHAR_MAX)
        {
            return;
        }

        idx = state->point.field_F;
    }
    else
    {
        idx = state->point.field_E;
    }

    /* Will - 1: checks if the value at whatever the struct is `temp_s0`
       pointing have defined values. It first checks if `s_CollisionState_A8::field_0`
       is 0 or not, need to mention that this value is defined by default as `-1`
       in `Collision_SubcellInit` so any other value means that the values where defined
       already, but if `s_CollisionState_A8::field_0` is not zero then it checks
       if the value stored in `state->point.field_20.field_8` is
       defined with any value other than 0, if not then it comes back here and continues
       if it is then it proceed to do some other checks that will return true.
       
       Likely if any of those cases fails the check then it will redefine whatever
       is supposed to be stored there.
    */
    temp_s0 = &state->field_A0.s_0.field_8[idx];
    if (!func_8006B7E0(temp_s0, &state->point.field_20))
    {
        return;
    }

    temp_s0->subChunkTransDir   = state->point.field_20.subChunkTransDir;
    temp_s0->radiusCollDiffDist = state->point.field_20.radiusCollDiffDist;
    temp_s0->collDiffDist       = state->point.field_20.collDiffDist;

    if (state->point.field_20.charaVertDiff.vz >= Q8(0.0f))
    {
        if (disableSurface0Height == true)
        {
            temp_s0->surfaceIdx = NO_VALUE;
        }
        else
        {
            temp_s0->surfaceIdx = surfaceIdx0;
        }
    }
    else
    {
        if (disableSurface1Height == true)
        {
            temp_s0->surfaceIdx = NO_VALUE;
        }
        else
        {
            temp_s0->surfaceIdx = surfaceIdx1;
        }
    }
}

bool func_8006B7E0(s_CollisionState_A8* cur, s_CollisionState_CC_20* prev) // 0x8006B7E0
{
    if (cur->subChunkTransDir == SubChunkTransitionDirection_None)
    {
        return true;
    }

    switch (prev->subChunkTransDir)
    {
        case SubChunkTransitionDirection_None:
            return false;

        case SubChunkTransitionDirection_Z:
            switch (cur->subChunkTransDir)
            {
                case 1:
                    if (prev->radiusCollDiffDist < cur->radiusCollDiffDist)
                    {
                        return true;
                    }
                    break;

                case 2:
                    if (prev->radiusCollDiffDist < (cur->radiusCollDiffDist + 6))
                    {
                        return true;
                    }
                    break;
            }
            break;

        case SubChunkTransitionDirection_X:
            switch (cur->subChunkTransDir)
            {
                case SubChunkTransitionDirection_Z:
                    if (prev->radiusCollDiffDist < (cur->radiusCollDiffDist - 6))
                    {
                        return true;
                    }
                    break;

                case SubChunkTransitionDirection_X:
                    if (prev->radiusCollDiffDist < (cur->radiusCollDiffDist - 6))
                    {
                        return true;
                    }

                    if (prev->radiusCollDiffDist < (cur->radiusCollDiffDist + 6) && prev->collDiffDist < cur->collDiffDist)
                    {
                        return true;
                    }
                    break;
            }
    }

    return false;
}

void func_8006B8F8(s_CollisionCellPoint* cellPoint) // 0x8006B8F8
{
    s_CollisionState_CC_20* ptr;
    s32                     temp_a1;
    s32                     temp_a2;
    s32                     temp_a3;

    temp_a3                                = (u16)cellPoint->splitVertex0.vx;
    temp_a1                                = (u16)cellPoint->splitVertex1.vx;
    temp_a2                                = (u16)cellPoint->splitVertex1.vz;
    cellPoint->splitVertex1.vx             = temp_a3;
    temp_a3                                = (u16)cellPoint->splitVertex0.vy;
    cellPoint->splitVertex0.vy             = cellPoint->splitVertex1.vy;
    cellPoint->splitVertex0.vx             = temp_a1;
    cellPoint->field_6.vx                  = -cellPoint->field_6.vx;
    cellPoint->splitVertex1.vy             = temp_a3;
    cellPoint->field_6.vy                  = -cellPoint->field_6.vy;
    temp_a3                                = (u16)cellPoint->splitVertex0.vz;
    cellPoint->splitVertex1.vz             = temp_a3;
    temp_a3                                = cellPoint->field_C.cellSurfaces.surfaceIdx0;
    cellPoint->field_20.charaMoveOffset.vx = -cellPoint->field_20.charaMoveOffset.vx;

    cellPoint->splitVertex0.vz                  = temp_a2;
    cellPoint->field_C.cellSurfaces.surfaceIdx0 = cellPoint->field_C.cellSurfaces.surfaceIdx1;

    ptr = &cellPoint->field_20;

    cellPoint->field_C.cellSurfaces.surfaceIdx1 = temp_a3;
    temp_a3                                     = cellPoint->field_E;
    cellPoint->field_20.charaMoveOffset.vz      = -cellPoint->field_20.charaMoveOffset.vz;
    cellPoint->field_E                          = cellPoint->field_F;
    cellPoint->field_F                          = temp_a3;

    ptr->charaVertDiff.vz     = -ptr->charaVertDiff.vz;
    ptr->charaVertDiff.vx     = (cellPoint->field_6.vz - ptr->charaVertDiff.vx);
    ptr->charaMoveVertDiff.vz = -ptr->charaMoveVertDiff.vz;
    ptr->charaMoveVertDiff.vx = (cellPoint->field_6.vz - ptr->charaMoveVertDiff.vx);
}

void func_8006B9C8(s_CollisionState* state) // 0x8006B9C8
{
    q23_8 charaRadius;

    if (state->point.field_C.cellSurfaces.surfaceIdx1 == UCHAR_MAX &&
        state->point.field_20.charaVertDiff.vz < Q8(0.0f) &&
        (state->charaState.bottomPos >= state->point.splitVertex0.vy ||
         state->charaState.bottomPos >= state->point.splitVertex1.vy))
    {
        if (state->field_0_9 && state->point.field_20.radiusCollDiffDist < state->charaState.radius)
        {
            func_8006BB50(state, 0);
            return;
        }

        charaRadius = state->charaState.radius;
        if (state->field_0_9 && state->point.field_20.radiusCollDiffDist < (charaRadius + 8))
        {
            func_8006BB50(state, 1);
        }

        if (state->isCharaMoving)
        {
            if (!state->field_44.field_0.field_0 &&
                (-charaRadius < state->point.field_20.charaVertDiff.vz ||
                 -charaRadius < state->point.field_20.charaMoveVertDiff.vz) &&
                (-charaRadius < state->point.field_20.charaVertDiff.vx ||
                 -charaRadius < state->point.field_20.charaMoveVertDiff.vx) &&
                (state->point.field_20.charaVertDiff.vx     < (charaRadius + state->point.field_6.vz) ||
                 state->point.field_20.charaMoveVertDiff.vx < (charaRadius + state->point.field_6.vz)))
            {
                func_8006BE40(state);
            }
        }
    }
}

void func_8006BB50(s_CollisionState* state, s32 arg1) // 0x8006BB50
{
    q23_8 charaCollDistX;
    q23_8 charaCollDistZ;
    q7_8  temp2;

    if (func_8006BC34(state) < Q8(0.0f))
    {
        return;
    }

    if (state->point.field_20.charaVertDiff.vx < Q8(0.0f))
    {
        charaCollDistX = state->charaPositionFrom.offset.vx - state->point.splitVertex0.vx;
        charaCollDistZ = state->charaPositionFrom.offset.vz - state->point.splitVertex0.vz;
    }
    else if (state->point.field_6.vz < state->point.field_20.charaVertDiff.vx)
    {
        charaCollDistX = state->charaPositionFrom.offset.vx - state->point.splitVertex1.vx;
        charaCollDistZ = state->charaPositionFrom.offset.vz - state->point.splitVertex1.vz;
    }
    else
    {
        charaCollDistX =  state->point.field_6.vy;
        charaCollDistZ = -state->point.field_6.vx;
    }

    temp2 = state->charaState.radius - state->point.field_20.radiusCollDiffDist;
    func_8006BCC4(&state->field_44, &state->point.ipdCollisionData->subcellCheckIdx[state->point.subcellIdx], arg1, charaCollDistX, charaCollDistZ, temp2);
#ifdef SH_PC_PORT
    WALLSTOP_SET(2, state->point.subcellIdx, state->point.splitVertex0.vx, state->point.splitVertex0.vz,
                 state->point.splitVertex1.vx, state->point.splitVertex1.vz,
                 state->charaState.radius, state->point.field_20.radiusCollDiffDist, arg1);
#endif
}

q23_8 func_8006BC34(s_CollisionState* state)
{
    q7_8 someZ;
    q7_8 height0;
    q7_8 height1;
    q7_8 someX;
    q7_8 height2;
    q7_8 unkHeight;

    someX = state->point.field_20.charaVertDiff.vx;
    if (someX < Q8(0.0f))
    {
        unkHeight = state->point.splitVertex0.vy;
    }
    else
    {
        someZ = state->point.field_6.vz;
        if (someZ < someX)
        {
            unkHeight = state->point.splitVertex1.vy;
        }
        else
        {
            height0 = state->point.splitVertex0.vy;
            height1 = state->point.splitVertex1.vy;
            height2 = state->point.splitVertex0.vy;

            if (height0 == height1)
            {
                unkHeight = height2;
            }
            else
            {
                unkHeight = height2 + ((s32)((height1 - height0) * someX) / someZ);
            }
        }
    }

    return state->charaState.bottomPos - unkHeight;
}

void func_8006BCC4(s_CollisionState_44* arg0, s8* arg1, u32 arg2, q7_8 distX, q7_8 distZ, q7_8 arg5) // 0x8006BCC4
{
    q7_8 rotX;
    q7_8 rotY;

    // TODO: This are not Q12 values nor angles. However the equivalent of both `Q12_ANGLE` in Q8 is weird.
    rotX = Q12_FRACT(ratan2(distZ, distX) - Q12_ANGLE(89.0f));
    rotY = Q12_FRACT(rotX + Q12_ANGLE(178.0f));

    switch (arg2)
    {
        case 0:
            *arg1 += 1;

            func_8006BDDC(&arg0->field_0, rotX, rotY);

            if (arg0->field_6 < arg5)
            {
                arg0->field_6 = arg5;
            }
            break;

        case 1:
            func_8006BDDC(&arg0->field_8, rotX, rotY);

            if (arg0->field_8.field_0 < 9)
            {
                arg0->field_10[arg0->field_8.field_0 - 1] = arg1;
            }
            break;
        
        case 2:
            *arg1 += 1;

            func_8006BDDC(&arg0->field_30, rotX, rotY);

            if (arg0->field_36 < arg5)
            {
                arg0->field_36 = arg5;
            }
            break;
    }
}

void func_8006BDDC(s_CollisionState_44_0* arg0, q7_8 rotX, q7_8 rotY) // 0x8006BDDC
{
    if (arg0->field_0 == 0)
    {
        arg0->field_0 = 1;
        arg0->field_2.vx = rotX;
        arg0->field_2.vy = rotY;
        return;
    }

    arg0->field_0++;
    Vw_ClampAngleRange(&arg0->field_2.vx, &arg0->field_2.vy, rotX, rotY);
}

void func_8006BE40(s_CollisionState* state) // 0x8006BE40
{
    q23_8 negRadius;
    q23_8 var_a2;
    s32   var_a1;
    u32   var_v1;
    s32   temp;

    var_a1    = 0;
    negRadius = -(state->charaState.radius);
    var_a2    = Q8(0.0F);

    if (state->point.field_20.charaVertDiff.vz >= negRadius)
    {
        if (state->point.field_20.charaVertDiff.vx >= Q8(0.0f))
        {
            if (state->point.field_6.vz >= state->point.field_20.charaVertDiff.vx)
            {
                var_a2 = state->point.field_20.charaVertDiff.vx;
                var_v1 = 2;
            }
            else
            {
                var_v1 = 1;
            }
        }
        else
        {
            var_v1 = 0;
        }
    }
    else
    {
        if (state->point.field_20.charaMoveOffset.vz == 0)
        {
            if (state->point.field_20.charaVertDiff.vx > 0 &&
                state->point.field_20.charaVertDiff.vx < state->point.field_6.vz)
            {
                var_a1 = 0;
                var_a2 = state->point.field_20.charaVertDiff.vx;
            }
        }
        else
        {
            var_a1 = FP_TO(negRadius - state->point.field_20.charaVertDiff.vz, Q12_SHIFT) /
                     state->point.field_20.charaMoveOffset.vz;
            temp   = state->point.field_20.charaMoveOffset.vx * var_a1;
            temp   = FP_FROM(temp, Q12_SHIFT);
            var_a2 = temp + state->point.field_20.charaVertDiff.vx;
        }

        if (var_a2 < Q8(0.0F))
        {
            var_v1 = 0;
        }
        else if (state->point.field_6.vz < var_a2)
        {
            var_v1 = 1;
        }
        else
        {
            var_v1 = 2;
        }
    }

    switch (var_v1)
    {
        case 0:
            func_8006BF88(state, &state->point.splitVertex0);
            break;

        case 1:
            func_8006BF88(state, &state->point.splitVertex1);
            break;

        case 2:
            func_8006C0C8(state, var_a1, var_a2);
            break;
    }
}

void func_8006BF88(s_CollisionState* state, const SVECTOR3* splitVert) // 0x8006BF88
{
    q3_12 temp_v0;
    s32   temp2;
    s32   temp3;

    temp_v0 = func_8006C248(*(s32*)&state->charaState.direction, state->charaState.distance,
                            splitVert->vx - state->charaPositionFrom.offset.vx,
                            splitVert->vz - state->charaPositionFrom.offset.vz,
                            state->charaState.radius);
    if (temp_v0 != NO_VALUE && func_8006C1B8(2, temp_v0, state) && state->charaState.bottomPos > splitVert->vy)
    {
        state->field_38 = temp_v0;
        state->field_34 = 2;
        temp2               = state->charaPositionFrom.offset.vx + Q12_MULT(state->charaState.offset.vx, temp_v0);
        state->field_3A = Q12_TO_Q4(state->charaState.distance * temp_v0);
        state->field_40 = &state->point.ipdCollisionData->subcellCheckIdx[state->point.subcellIdx];

        state->field_3C = temp2 - splitVert->vx;
        temp3               = state->charaPositionFrom.offset.vz + Q12_MULT(state->charaState.offset.vz, temp_v0);
        state->field_3E = temp3 - splitVert->vz;
#ifdef SH_PC_PORT
        WALLSTOP_SET(1, state->point.subcellIdx, splitVert->vx, splitVert->vz, splitVert->vy, 0,
                     state->charaState.radius, state->point.field_20.radiusCollDiffDist, temp_v0);
#endif
    }
}

void func_8006C0C8(s_CollisionState* state, s16 arg1, q7_8 arg2) // 0x8006C0C8
{
    q23_8 temp;

    if (!func_8006C1B8(1, arg1, state) || state->point.field_20.charaMoveOffset.vz < 0)
    {
        return;
    }

    temp = ((state->point.splitVertex1.vy - state->point.splitVertex0.vy) * arg2) / state->point.field_6.vz;
    if (temp + state->point.splitVertex0.vy < state->charaState.bottomPos)
    {
        state->field_40 = &state->point.ipdCollisionData->subcellCheckIdx[state->point.subcellIdx];
        state->field_34 = 1;
        state->field_38 = arg1;
        state->field_3A = Q12_TO_Q4(state->charaState.distance * arg1);
        state->field_3C =  state->point.field_6.vy;
        state->field_3E = -state->point.field_6.vx;
    }
}

bool func_8006C1B8(u32 arg0, q3_12 arg1, s_CollisionState* state) // 0x8006C1B8
{
    q27_4 var;

    var = Q12_TO_Q4(state->charaState.distance * arg1);
    switch (arg0)
    {
        default:
        case 0:
            return false;

        case 2:
            if (state->field_34 != 1)
            {
                if (state->field_34 == 0)
                {
                   return true;
                }

                break;
            }

            var += Q4(6.0f);
            break;

        case 1:
            if (state->field_34 != arg0)
            {
                if (state->field_34 != 0)
                {
                    if (state->field_34 == 2)
                    {
                        var -= Q4(6.0f);
                    }

                    break;
                }

                return true;
            }
            break;
    }

    return var < state->field_3A;
}

q3_12 func_8006C248(s32 packedDir, q3_12 arg1, q3_12 deltaX, q7_8 deltaZ, q7_8 arg4) // 0x8006C248
{
    DVECTOR sp10; // Q19.12
    s16     temp_v0;
    s16     dist;
    q3_12   alpha;

    gte_ldR11R12(packedDir);
    gte_ldR13R21(packedDir);
    gte_ldvxy0(PACKED_XZ16(deltaX, deltaZ));
    gte_ldvz0();
    gte_rtv0();
    gte_stMAC12(&sp10.vx);

    if (sp10.vx < Q12(0.0f))
    {
        dist = Math_Vector2MagCalc(sp10.vx, sp10.vy);
    }
    else if (arg1 < sp10.vx)
    {
        temp_v0 = sp10.vx - arg1;
        dist  = Math_Vector2MagCalc(temp_v0, sp10.vy);
    }
    else
    {
        dist = ABS(sp10.vy);
    }

    if (arg1 == Q12(0.0f))
    {
        alpha = NO_VALUE;
        if (dist >= arg4)
        {
            return alpha;
        }
        else
        {
            return Q12(0.0f);
        }
    }

    if (dist >= arg4)
    {
        return NO_VALUE;
    }

    alpha = FP_TO(sp10.vx - SquareRoot0(SQUARE(arg4) - SQUARE(sp10.vy)), Q12_SHIFT) / arg1; // TODO: Use `Math_Vector2MagCalc`.
    alpha = CLAMP(alpha, Q12(0.0f), Q12(1.0f));

    return alpha;
}

bool func_8006C3D4(s_CollisionState* state, s_IpdCollisionData* collData, s32 subcellIdx) // 0x8006C3D4
{
    s_IpdCollisionData_18* temp_a1;

    state->point.ipdCollisionData = collData;
    state->point.subcellIdx       = subcellIdx;
    temp_a1                       = &collData->ptr_18[subcellIdx - collData->subcellCount];

    if (!((state->flags >> temp_a1->field_0_8) & (1 << 0)))
    {
        return false;
    }

    state->point.heightDisabled  = temp_a1->disableHeight;
    state->point.field_6.vx      = temp_a1->offset.vx;
    state->point.field_6.vy      = temp_a1->offset.vy;
    state->point.field_6.vz      = temp_a1->offset.vz;
    state->point.field_C.field_0 = temp_a1->field_8;
    return true;
}

void func_8006C45C(s_CollisionState* state) // 0x8006C45C
{
    q7_8  distMax;
    q7_8  dist;
    q3_12 var_s2;
    q23_8 bound;
    q23_8 distX;
    q23_8 distZ;
    q23_8 temp_v1;
    s32   temp;
    s32   temp2;

    distMax = state->charaState.radius + state->point.field_C.field_0;
    bound   = distMax + 8;
    temp_v1 = state->point.field_6.vx - bound;

    if (state->charaPositionFrom.offset.vx < temp_v1 &&
        state->charaPositionTo.offset.vx   < temp_v1)
    {
        return;
    }

    if ((state->point.field_6.vx + bound) < state->charaPositionFrom.offset.vx &&
        (state->point.field_6.vx + bound) < state->charaPositionTo.offset.vx)
    {
        return;
    }

    if (state->charaPositionFrom.offset.vz < (state->point.field_6.vz - bound) &&
        state->charaPositionTo.offset.vz   < (state->point.field_6.vz - bound))
    {
        return;
    }

    if ((state->point.field_6.vz + bound) < state->charaPositionFrom.offset.vz &&
        (state->point.field_6.vz + bound) < state->charaPositionTo.offset.vz)
    {
        return;
    }

    distX = state->charaPositionFrom.offset.vx - state->point.field_6.vx;
    distZ = state->charaPositionFrom.offset.vz - state->point.field_6.vz;
    dist  = Math_Vector2MagCalc(distX, distZ);

    if (dist < state->point.field_C.field_0 && state->point.heightDisabled != true &&
        (state->subcellIdx == UCHAR_MAX || state->point.field_6.vy < state->groundHeight))
    {
        state->subcellIdx   = state->point.subcellIdx;
        state->groundHeight = state->point.field_6.vy;
    }

#ifdef SH_PC_PORT
    /* Draw this near obstacle in the visualizer (red) whether or not it blocks,
     * so you can still SEE the ptr_18 colliders even with collision disabled.
     * Then skip the SOLID response (ground-height capture above is kept) so a
     * sprinting Harry stops bumping the invisible ones. */
    WALLSTOP_VIS_OBST();
    if (!g_PcObstacleCollision)
    {
        return;
    }
#endif

    if (!state->isCharaMoving && !state->field_0_9 || dist < state->point.field_C.field_0)
    {
        return;
    }

    if (dist < distMax && state->field_0_9)
    {
        func_8006C794(state, 0, dist);
        return;
    }

    if (dist < (distMax + 8) && state->field_0_9)
    {
        func_8006C794(state, 1, dist);
    }

    if (!state->isCharaMoving || state->field_44.field_0.field_0)
    {
        return;
    }

    var_s2 = func_8006C248(*(s32*)&state->charaState.direction, state->charaState.distance,
                           state->point.field_6.vx - state->charaPositionFrom.offset.vx,
                           state->point.field_6.vz - state->charaPositionFrom.offset.vz,
                           distMax);

    if (var_s2 == -1)
    {
        return;
    }

    if (var_s2 < Q12(0.0f))
    {
        var_s2 = 0;
    }

    if (func_8006C1B8(1, var_s2, state) && state->charaState.bottomPos >= state->point.field_6.vy)
    {
        state->field_38 = var_s2;
        state->field_34 = 1;
        temp                = state->charaPositionFrom.offset.vx + Q12_MULT(state->charaState.offset.vx, var_s2);
        state->field_3A = Q12_TO_Q4(state->charaState.distance * var_s2);
        state->field_40 = &state->point.ipdCollisionData->subcellCheckIdx[state->point.subcellIdx];
        state->field_3C = temp - state->point.field_6.vx;
        temp2               = state->charaPositionFrom.offset.vz + Q12_MULT(state->charaState.offset.vz, var_s2);
        state->field_3E = temp2 - state->point.field_6.vz;
#ifdef SH_PC_PORT
        WALLSTOP_SET(3, state->point.subcellIdx, state->point.field_6.vx, state->point.field_6.vz,
                     state->point.field_6.vy, state->point.field_C.field_0, distMax, dist, var_s2);
#endif
    }
}

void func_8006C794(s_CollisionState* state, s32 arg1, s32 dist) // 0x8006C794
{
    if (state->charaState.bottomPos >= (state->point.field_6.vy + (dist - state->point.field_C.field_0)))
    {
        func_8006BCC4(&state->field_44,
                      &state->point.ipdCollisionData->subcellCheckIdx[state->point.subcellIdx],
                      arg1,
                      state->charaPositionFrom.offset.vx - state->point.field_6.vx,
                      state->charaPositionFrom.offset.vz - state->point.field_6.vz,
                      (state->charaState.radius + state->point.field_C.field_0) - dist);
#ifdef SH_PC_PORT
        WALLSTOP_SET(5, state->point.subcellIdx, state->point.field_6.vx, state->point.field_6.vz,
                     state->point.field_6.vy, state->point.field_C.field_0,
                     state->charaState.radius + state->point.field_C.field_0, dist, arg1);
#endif
    }
}

void func_8006C838(s_CollisionState* state, s_IpdCollisionData* collData) // 0x8006C838
{
    q19_12                 slopedGroundHeight;
    s_CollisionState_A8*   curUnk;
    s_IpdCollSurface*      curSurface;
    s_IpdCollisionData_18* temp_a0;

    if (state->field_A0.s_0.subcellRange == NULL)
    {
        return;
    }

    if (state->subcellIdx != UCHAR_MAX && state->groundHeight < state->slopedGroundHeight)
    {
        temp_a0                   = &collData->ptr_18[state->subcellIdx - collData->subcellCount];
        state->slopedGroundHeight = state->groundHeight;
        state->charaCellOffsetX   = state->charaPositionFrom.offset.vx + collData->positionX;
        state->charaCellOffsetZ   = state->charaPositionFrom.offset.vz + collData->positionZ;
        state->tiltAngleX         = Q12_ANGLE(0.0f);
        state->tiltAngleZ         = Q12_ANGLE(0.0f);
        state->heightDisabled     = temp_a0->disableHeight;
        state->groundType         = temp_a0->groundType;
    }

    for (curUnk = &state->field_A0.s_0.field_8[0]; curUnk < &state->field_A0.s_0.field_8[4]; curUnk++)
    {
        if (curUnk->surfaceIdx != UCHAR_MAX)
        {
            curSurface = &collData->surfaces[curUnk->surfaceIdx];

            // Compute sloped ground height.
            slopedGroundHeight = curSurface->baseGroundHeight;
            if (curSurface->tiltAngleX != Q12_ANGLE(0.0f))
            {
                slopedGroundHeight += Q12_MULT(curSurface->tiltAngleX, state->charaPositionFrom.offset.vx - curSurface->field_0);
            }
            if (curSurface->tiltAngleZ != Q12_ANGLE(0.0f))
            {
                slopedGroundHeight += Q12_MULT(curSurface->tiltAngleZ, state->charaPositionFrom.offset.vz - curSurface->field_4);
            }

            if (slopedGroundHeight < state->slopedGroundHeight)
            {
                state->slopedGroundHeight = slopedGroundHeight;
                state->charaCellOffsetX   = state->charaPositionFrom.offset.vx + collData->positionX;
                state->charaCellOffsetZ   = state->charaPositionFrom.offset.vz + collData->positionZ;
                state->tiltAngleX         = curSurface->tiltAngleX;
                state->tiltAngleZ         = curSurface->tiltAngleZ;
                state->heightDisabled     = curSurface->disableHeight;
                state->groundType         = curSurface->groundType;
            }
        }
    }
}

void func_8006CA18(s_CollisionState* state, s_IpdCollisionData* collData, s_IpdCollSubcellRange* subcellRanges) // 0x8006CA18
{
    s32               startIdx;
    s32               endIdx;
    q23_8             slopedGroundHeight;
    u8*               curSurfaceIdx;
    s_IpdCollSurface* curSurface;

    startIdx = subcellRanges[0].field_2;
    endIdx   = subcellRanges[1].field_2;

    // Check for invalid range.
    if (startIdx == endIdx)
    {
        return;
    }

    // Run through surfaces.
    for (curSurfaceIdx = &collData->ptr_2C[startIdx]; curSurfaceIdx < &collData->ptr_2C[endIdx]; curSurfaceIdx++)
    {
        curSurface = &collData->surfaces[*curSurfaceIdx];

        if (((state->flags >> curSurface->field_6_11) & (1 << 0)) &&
            curSurface->disableHeight != true)
        {
            // Compute sloped ground height.
            slopedGroundHeight = curSurface->baseGroundHeight;
            if (curSurface->tiltAngleX != Q8_ANGLE(0.0f))
            {
                slopedGroundHeight += Q12_MULT(curSurface->tiltAngleX, state->charaPositionFrom.offset.vx - curSurface->field_0);
            }
            if (curSurface->tiltAngleZ != Q8_ANGLE(0.0f))
            {
                slopedGroundHeight += Q12_MULT(curSurface->tiltAngleZ, state->charaPositionFrom.offset.vz - curSurface->field_4);
            }

            if (slopedGroundHeight < state->slopedGroundHeight)
            {
                state->slopedGroundHeight = slopedGroundHeight;
                state->charaCellOffsetX   = state->charaPositionFrom.offset.vx + collData->positionX;
                state->charaCellOffsetZ   = state->charaPositionFrom.offset.vz + collData->positionZ;
                state->tiltAngleX         = curSurface->tiltAngleX;
                state->tiltAngleZ         = curSurface->tiltAngleZ;
                state->heightDisabled     = curSurface->disableHeight;
                state->groundType         = curSurface->groundType;
            }
        }
    }
}

q3_12 Collision_OffsetAlphaGet(s_CollisionState* state) // 0x8006CB90
{
    q23_8 groundHeight;

    if (state->slopedGroundHeight == Q8(30.0f))
    {
        return Q12(1.0f);
    }

    groundHeight = Ipd_GroundHeightGet(state->charaState.positionToX, state->charaState.positionToZ, state);
    if ((state->charaState.bottomPos + state->charaState.offset.vy) < groundHeight ||
        groundHeight == state->slopedGroundHeight)
    {
        return Q12(1.0f);
    }

    {
        q19_12 mag = Math_Vector2MagCalc(state->charaState.distance, groundHeight - state->charaState.bottomPos);
#ifdef SH_PC_PORT
        /* Lighthouse-stair crash (progression SilentHill(3/4).log, 0xC0000094):
         * a degenerate slope (distance==0 && groundHeight==bottomPos) makes the
         * magnitude 0. PSX MIPS `div` by zero yields garbage and continues; PC
         * traps. Return the same no-scaling alpha the degenerate cases above use. */
        if (mag == 0)
        {
            return Q12(1.0f);
        }
#endif
        return Q12_DIV(state->charaState.distance, mag);
    }
}

q23_8 Ipd_GroundHeightGet(q23_8 posX, q23_8 posZ, const s_CollisionState* state) // 0x8006CC44
{
    if (state->groundType != GroundType_None)
    {
        return Q12_MULT(state->tiltAngleX, posX - state->charaCellOffsetX) +
               Q12_MULT(state->tiltAngleZ, posZ - state->charaCellOffsetZ) +
               state->slopedGroundHeight;
    }

    return Q8(8.0f);
}

void func_8006CC9C(s_CollisionState* state) // 0x8006CC9C
{
    q23_8  deltaX;
    q23_8  deltaZ;
    q19_12 temp_v0;
    q23_8  temp_s4;
    q23_8  temp;
    q23_8  temp2;
    s32    temp3;
    s32    temp4;
    s32    tarCharaBottom;

    if (state->field_A0.s_1.collisionState < CharaCollisionState_2 ||
        *state->field_A0.s_1.field_8 != 0)
    {
        return;
    }

    if (state->charaPositionFrom.field_0 + (state->field_A0.s_1.field_4 + state->charaState.distance) < state->charaState.positionFromX ||
        state->charaState.positionToX < state->charaPositionFrom.field_0 - (state->field_A0.s_1.field_4 + state->charaState.distance))
    {
        return;
    }

    if (state->charaPositionTo.field_0 + (state->field_A0.s_1.field_4 + state->charaState.distance) < state->charaState.positionFromZ ||
        state->charaState.positionToZ < state->charaPositionTo.field_0 - (state->field_A0.s_1.field_4 + state->charaState.distance) ||
        state->charaState.topPos > state->field_A0.s_1.field_2)
    {
        return;
    }

    deltaX = (state->charaState.positionFromX - state->charaPositionFrom.field_0);

    if (state->charaState.bottomPos < state->field_A0.s_1.field_0)
    {
        return;
    }

    deltaZ = state->charaState.positionFromZ - state->charaPositionTo.field_0;
    temp_s4 = Math_Vector2MagCalc(deltaX, deltaZ);

    temp_v0 = func_8006C248(*(s32*)&state->charaState.direction, state->charaState.distance,
                            state->charaPositionFrom.field_0 - state->charaState.positionFromX,
                            state->charaPositionTo.field_0   - state->charaState.positionFromZ,
                            state->field_A0.s_1.field_4);
    if (temp_v0 == NO_VALUE)
    {
        return;
    }

    if (temp_v0 == 0)
    {
        if (state->field_0_9)
        {
            temp3 = state->field_A0.s_1.field_4 - temp_s4;
            func_8006BCC4(&state->field_44, state->field_A0.s_1.field_8, 2, deltaX, deltaZ, temp3);
#ifdef SH_PC_PORT
            WALLSTOP_SET(4, -1, state->charaPositionFrom.field_0, state->charaPositionTo.field_0,
                         0, 0, state->field_A0.s_1.field_4, temp_s4, 0);
#endif
        }
    }
    else if (state->isCharaMoving && state->field_44.field_0.field_0 == 0 && func_8006C1B8(1, temp_v0, state))
    {
        temp2 = (state->charaState.positionFromZ - state->charaPositionTo.field_0);
        tarCharaBottom = Q12_MULT(temp_v0, state->charaState.offset.vz);

        state->field_40 = state->field_A0.s_1.field_8;
        state->field_38 = temp_v0;

        state->field_34 = 1;

        temp  = (state->charaState.positionFromX - state->charaPositionFrom.field_0);
        temp4 = Q12_MULT(temp_v0, state->charaState.offset.vx);

        state->field_3A = Q12_TO_Q4(state->charaState.distance * temp_v0);
        state->field_3C = temp + temp4;
        state->field_3E = temp2 + tarCharaBottom;
#ifdef SH_PC_PORT
        WALLSTOP_SET(4, -1, state->charaPositionFrom.field_0, state->charaPositionTo.field_0,
                     0, 0, state->field_A0.s_1.field_4, temp_s4, temp_v0);
#endif
    }
}

void func_8006CF18(s_CollisionState* state, s_func_8006CF18* arg1, s32 idx) // 0x8006CF18
{
    q23_8            var_a1;
    s_func_8006CF18* curArg1;

    for (curArg1 = arg1; curArg1 < &arg1[idx]; curArg1++)
    {
        var_a1 = Q12_TO_Q8(curArg1->field_10) + state->charaState.radius;
        if (curArg1->collisionState < (u32)state->charaState.collisionState)
        {
            var_a1 -= 15;
        }

        state->charaPositionFrom.field_0 = Q12_TO_Q8(curArg1->position.vx);
        state->charaPositionTo.field_0   = Q12_TO_Q8(curArg1->position.vz);

        state->field_A0.s_1.field_0        = Q12_TO_Q8(curArg1->field_E + curArg1->position.vy);
        state->field_A0.s_1.field_2        = Q12_TO_Q8(curArg1->field_C + curArg1->position.vy);
        state->field_A0.s_1.field_4        = var_a1;
        state->field_A0.s_1.collisionState = curArg1->collisionState;
        state->field_A0.s_1.field_8        = &curArg1->field_13;

        if (state->field_0_0 == false)
        {
            curArg1->field_13 = 0;
        }

        func_8006CC9C(state);
    }
}

void func_8006D01C(VECTOR3* arg0, VECTOR3* offset, q3_12 offsetAlpha, s_CollisionState* state) // 0x8006D01C
{
    VECTOR3 adjOffset;
    s32     temp_s0;
    s32     temp_s1;
    s32     temp_a0;
    s32     temp_v0;

    adjOffset.vx = Q12_MULT(offset->vx, offsetAlpha);
    adjOffset.vz = Q12_MULT(offset->vz, offsetAlpha);

    if (state->field_44.field_0.field_0 || state->field_44.field_30.field_0)
    {
        arg0->vx = Q12(0.0f);
        arg0->vz = Q12(0.0f);
        *offset    = adjOffset;
        func_8006D2B4(offset, &state->field_44);
        return;
    }

    if (!state->field_34)
    {
        *arg0    = adjOffset;
        offset->vz = Q12(0.0f);
        offset->vx = Q12(0.0f);
        return;
    }

    if (offsetAlpha < state->field_38)
    {
        state->field_34 = 0;
        *arg0          = adjOffset;
        offset->vz       = Q12(0.0f);
        offset->vx       = Q12(0.0f);
        return;
    }

    arg0->vx = Q12_MULT(offset->vx, state->field_38);
    arg0->vz = Q12_MULT(offset->vz, state->field_38);
    offset->vx = adjOffset.vx - arg0->vx;
    offset->vz = adjOffset.vz - arg0->vz;

    temp_s0 = state->field_3C;
    temp_s1 = state->field_3E;
    temp_a0 = SQUARE(temp_s0) + SQUARE(temp_s1);

    if (temp_a0 < 256)
    {
        temp_a0 = SquareRoot0(temp_a0 * 256);
        temp_s0 = (temp_s0 << 16) / temp_a0;
        temp_s1 = (temp_s1 << 16) / temp_a0;
    }
    else
    {
        temp_a0 = SquareRoot0(temp_a0);
        temp_s0 = (temp_s0 << 12) / temp_a0; // `Q12_SHIFT`?
        temp_s1 = (temp_s1 << 12) / temp_a0;
    }

    temp_v0  = FP_FROM((offset->vx * temp_s1) + (offset->vz * -temp_s0), Q12_SHIFT);
    offset->vx = Q12_MULT(temp_v0, temp_s1);
    offset->vz = Q12_MULT(temp_v0, -temp_s0);

    if (temp_s0 > Q12(1.0f / 3.0f))
    {
        arg0->vx += Q12(0.004f);
    }
    else if (temp_s0 < Q12(-1.0f / 3.0f))
    {
        arg0->vx -= Q12(0.004f);
    }

    if (temp_s1 > Q12(1.0f / 3.0f))
    {
        arg0->vz += Q12(0.004f);
    }
    else if (temp_s1 < Q12(-1.0f / 3.0f))
    {
        arg0->vz -= Q12(0.004f);
    }
}

void func_8006D2B4(VECTOR3* arg0, s_CollisionState_44* arg1) // 0x8006D2B4
{
    q3_12  rotY1;
    q3_12  rotX1;
    q3_12  angleMin;
    q3_12  angleMax;
    q3_12  angle1;
    q19_12 unkAngleMax;
    bool   cond;
    bool   var_s1;
    q3_12  angle;
    q19_12 unkAngleMin;
    s16    var_v1_2;
    s16    var_a0;
    q3_12  angle2;
    q3_12  angle3;

    if (arg1->field_30.field_0 != 0)
    {
        var_s1 = false;
        if (arg1->field_0.field_0 != 0)
        {
            rotY1 = arg1->field_0.field_2.vx;
            rotX1 = arg1->field_0.field_2.vy;
            if (arg1->field_8.field_0 != 0)
            {
                Vw_ClampAngleRange(&rotY1, &rotX1,
                                   arg1->field_8.field_2.vx, arg1->field_8.field_2.vy);
            }
        }
        else if (arg1->field_8.field_0 != 0)
        {
            rotY1 = arg1->field_8.field_2.vx;
            rotX1 = arg1->field_8.field_2.vy;
        }
        else
        {
            var_s1 = true;
        }

        if (var_s1)
        {
            cond = true;
        }
        else
        {
            angle1  = ratan2(arg0->vz, arg0->vx);
            angle = Q12_ANGLE(90.0f);
            angle2  = Q12_ANGLE_NORM_S(rotX1 - (angle1 - angle));
            angle3  = Q12_ANGLE_NORM_S(rotY1 - (angle1 + angle));
            if (angle2 < Q12_ANGLE(0.0f) && angle3 > Q12_ANGLE(0.0f))
            {
                cond = false;
            }
            else
            {
                cond = true;
            }
        }

        if (cond)
        {
            if (arg1->field_0.field_0 == 0)
            {
                arg1->field_0 = arg1->field_30;
                arg1->field_6 = arg1->field_36;
            }
            else
            {
                arg1->field_0.field_0 += arg1->field_30.field_0;

                Vw_ClampAngleRange(&arg1->field_0.field_2.vx, &arg1->field_0.field_2.vy,
                                   arg1->field_30.field_2.vx, arg1->field_30.field_2.vy);

                var_a0 = arg1->field_6;
                if (arg1->field_6 < arg1->field_36)
                {
                    var_a0 = arg1->field_36;
                }
                arg1->field_6 = var_a0;
            }
        }

        if (arg1->field_8.field_0 != 0)
        {
            if (arg1->field_0.field_0 == 0)
            {
                arg1->field_0 = arg1->field_8;
                arg1->field_6 = 0;
            }
            else
            {
                arg1->field_0.field_0 += arg1->field_8.field_0;
                Vw_ClampAngleRange(&arg1->field_0.field_2.vx, &arg1->field_0.field_2.vy,
                                   arg1->field_8.field_2.vx, arg1->field_8.field_2.vy);
            }
        }
    }

    if (arg1->field_0.field_0 != 0)
    {
        if (arg1->field_0.field_2.vx == arg1->field_0.field_2.vy)
        {
            arg0->vx = Q12(0.0f);
            arg0->vz = Q12(0.0f);
            return;
        }

        angle = (arg1->field_0.field_2.vx + arg1->field_0.field_2.vy) >> 1;
        if (arg1->field_0.field_2.vy < arg1->field_0.field_2.vx)
        {
            angle = Q12_FRACT(angle + Q12_ANGLE(180.0f));
        }

        if (arg1->field_8.field_0 == 0)
        {
            angleMin = arg1->field_0.field_2.vx;
            angleMax = arg1->field_0.field_2.vy;
        }
        else
        {
            angleMin = arg1->field_0.field_2.vx;
            angleMax = arg1->field_0.field_2.vy;

            Vw_ClampAngleRange(&angleMin, &angleMax, arg1->field_8.field_2.vx, arg1->field_8.field_2.vy);

            angleMin = Q12_FRACT(angleMin);
            angleMax = Q12_FRACT(angleMax);
            if (angleMin == angleMax)
            {
                angleMin = arg1->field_0.field_2.vx;
                angleMax = arg1->field_0.field_2.vy;
            }

            if (angleMin != angleMax)
            {
                unkAngleMin = Q12_ANGLE_NORM_S(angleMin - angle);
                unkAngleMax = Q12_ANGLE_NORM_S(angleMax - angle);
                if (unkAngleMin >= Q12_ANGLE(0.0f) || unkAngleMax <= Q12_ANGLE(0.0f))
                {
                    if (ABS(unkAngleMin) < ABS(unkAngleMax))
                    {
                        angle = angleMin;
                    }
                    else
                    {
                        angle = angleMax;
                    }
                }
            }
        }

        var_v1_2 = MIN(arg1->field_6 + 2, 16) * 16;
        func_8006D600(arg0, angle, angleMin, angleMax, var_v1_2);
    }
}

void func_8006D600(VECTOR3* pos, q19_12 angle, q19_12 angleMin, q19_12 angleMax, q19_12 dist) // 0x8006D600
{
    q19_12 angle3;
    q3_12  normAngle;
    q3_12  normAngleMin;
    q3_12  normAngleMax;
    q19_12 sinAngle2;
    q19_12 deltaX;
    q19_12 deltaZ;
    q19_12 z;
    q19_12 x;
    q19_12 cosAngle2;
    q19_12 angle2;
    q19_12 dist0;
    q19_12 angle1;

    normAngle    = Q12_FRACT(angle);
    normAngleMin = Q12_FRACT(angleMin);
    normAngleMax = Q12_FRACT(angleMax);

    if (dist > Q12(1.0f / 16.0f))
    {
        dist = Q12(1.0f / 16.0f);
    }

    x      = Q12_MULT(dist, Math_Cos(normAngle));
    angle2 = normAngleMax;
    z      = Q12_MULT(dist, Math_Sin(normAngle));
    deltaX = pos->vx - x;
    deltaZ = pos->vz - z;
    angle1 = Q12_FRACT(ratan2(deltaZ, deltaX));

    if (angle1 < normAngleMin)
    {
        angle1 += Q12_ANGLE(360.0f);
    }

    if (angle2 < normAngleMin)
    {
        angle2 += Q12_ANGLE(360.0f);
    }

    angle3 = normAngleMin + Q12_ANGLE(360.0f);
    if (angle2 < angle1)
    {
        if (((angle2 + angle3) >> 1) < angle1)
        {
            angle2 = angle3;
        }

        sinAngle2 = Math_Sin(angle2);
        cosAngle2 = Math_Cos(angle2);

        dist0 = Q12_MULT(deltaX, cosAngle2) + Q12_MULT(deltaZ, sinAngle2);
        if (dist0 < Q12(0.0f))
        {
            dist0 = Q12(0.0f);
        }

        pos->vx = x + Q12_MULT(dist0, cosAngle2);
        pos->vz = z + Q12_MULT(dist0, sinAngle2);
    }
}

void func_8006D774(s_CollisionState* state, VECTOR3* arg1, VECTOR3* arg2) // 0x8006D774
{
    SVECTOR offset0; // Q7.8
    SVECTOR offset1; // Q7.8

    offset0.vx = Q12_TO_Q8(arg1->vx);
    offset0.vy = Q12_TO_Q8(arg1->vz);
    offset1.vx = Q12_TO_Q8(arg2->vx);
    offset1.vy = Q12_TO_Q8(arg2->vz);

    state->field_34                  = 0;
    state->field_44.field_0.field_0  = 0;
    state->field_44.field_6          = 0;
    state->field_44.field_8.field_0  = 0;
    state->field_44.field_36         = 0;
    state->field_44.field_30.field_0 = 0;

    func_8006D7EC(&state->charaState, &offset0, &offset1);
}

void func_8006D7EC(s_CollisionCharaState* charaState, SVECTOR* offset0, SVECTOR* offset1) // 0x8006D7EC
{
    q3_12  headingAngle;
    q19_12 dist;
    q7_8   z;

    charaState->offset.vx = offset1->vx;
    z                     = offset1->vy;
    charaState->offset.vz = offset1->vy;

    dist                 = Math_Vector2MagCalc(charaState->offset.vx, z);
    charaState->distance = dist;

    if (dist != Q12(0.0f))
    {
        charaState->direction.vx = FP_TO(charaState->offset.vx, Q12_SHIFT) / dist;
        charaState->direction.vz = FP_TO(charaState->offset.vz, Q12_SHIFT) / charaState->distance;

        headingAngle             = ratan2(charaState->offset.vz, charaState->offset.vx);
        charaState->direction.vx = Math_Cos(headingAngle);
        charaState->direction.vz = Math_Sin(headingAngle);
    }
    else
    {
        charaState->direction.vx = Q12(1.0f);
        charaState->direction.vz = Q12(0.0f);
    }

    charaState->positionFromX = charaState->positionFromX + offset0->vx;
    charaState->positionFromZ = charaState->positionFromZ + offset0->vy;
    charaState->positionToX   = charaState->positionFromX + charaState->offset.vx;
    charaState->positionToZ   = charaState->positionFromZ + charaState->offset.vz;
}

// Padding with garbage data.
#if VERSION_IS(USA)
    static const u8 __unk_rdata[] = { 0x00, 0x42, 0x05, 0x80, 0x00, 0x00, 0x00, 0x00 };
#elif VERSION_IS(JAP0)
    static const u8 __unk_rdata[] = { 0x00, 0x20, 0x3D, 0x20, 0x00, 0x00, 0x00, 0x00 };
#elif VERSION_IS(JAP1)
    static const u8 __unk_rdata[] = { 0x00, 0x5E, 0x05, 0x80, 0x00, 0x00, 0x00, 0x00 };
#elif VERSION_IS(JAP2)
    static const u8 __unk_rdata[] = { 0x00, 0x00, 0x42, 0x24, 0x00, 0x00, 0x00, 0x00 };
#endif

#ifdef SH_PC_PORT
extern int g_CollVisEnabled; /* Collision visualizer toggle (dbg_overlay.c, ' key) */

/* Cache the known-good collision data pointer and a fingerprint of its key
 * fields. When IPD chunks get overwritten by new chunk loads, the pointer
 * from func_800426E4 still looks valid but the data is garbage. We detect
 * this by checking if the fingerprint still matches. Field names track the
 * upstream modular s_IpdCollisionData layout (positionX/Z, subcellSize,
 * subcellCountX/Z) — the fork's positionX_0/field_1C/1E/1F equivalents. */
#define PC_COLL_MAX_PTRS 256
static s_IpdCollisionData* s_ValidCollPtrs[PC_COLL_MAX_PTRS];
static s32 s_ValidCollFingerprints[PC_COLL_MAX_PTRS];
static int s_ValidCollCount = 0;

static s32 PC_CollFingerprint(s_IpdCollisionData* cd) {
    return cd->positionX ^ cd->positionZ ^ (cd->subcellSize << 16) ^
           (cd->subcellCountX << 8) ^ cd->subcellCountZ;
}

void PC_CollRegisterValid(s_IpdCollisionData* cd) {
    int i;
    s32 fp = PC_CollFingerprint(cd);
    /* Update if already registered (chunk reloaded in same slot) */
    for (i = 0; i < s_ValidCollCount; i++) {
        if (s_ValidCollPtrs[i] == cd) {
            s_ValidCollFingerprints[i] = fp;
            return;
        }
    }
    /* Add new entry */
    if (s_ValidCollCount < PC_COLL_MAX_PTRS) {
        s_ValidCollPtrs[s_ValidCollCount] = cd;
        s_ValidCollFingerprints[s_ValidCollCount] = fp;
        s_ValidCollCount++;
    }
}

static int PC_CollIsValid(s_IpdCollisionData* cd) {
    int i;
    if (!cd) return 0;
    for (i = 0; i < s_ValidCollCount; i++) {
        if (cd == s_ValidCollPtrs[i]) {
            if (PC_CollFingerprint(cd) != s_ValidCollFingerprints[i]) {
                return 0; /* Chunk reloaded — fingerprint stale until next reformat */
            }
            return 1;
        }
    }
    return 0; /* Unknown pointer — not reformatted by us */
}

/* Capture the full wall-segment geometry of the collision cell at (px, pz)
 * for the visualizer wireframe (green). Called once per frame so the overlay
 * shows stable, complete local collision — including walls Harry isn't
 * touching — instead of only the per-frame evaluated faces. World pos (Q12) =
 * (cellOrigin + splitVertex) << 4 (collision data is Q8). Subcell wall faces
 * index splitVertices via splitVertexIdx0/1 (fork field_6/field_7). */
void CollVis_CaptureCell(q19_12 px, q19_12 pz)
{
    extern s_IpdCollisionData* func_800426E4(s32 posX, s32 posZ);
    extern void CollVis_CaptureSeg(s32 ax, s32 ay, s32 az, s32 bx, s32 by, s32 bz, int hit);
    extern void CollVis_ClearCell(void);
    static s_IpdCollisionData* s_lastCell = NULL;
    s_IpdCollisionData* cd;
    s32 count, i, ox, oz;

    if (!g_CollVisEnabled)
    {
        return;
    }

    cd = func_800426E4(px, pz);
    if (cd == NULL || !PC_CollIsValid(cd))
    {
        if (s_lastCell != NULL)
        {
            CollVis_ClearCell();
            s_lastCell = NULL;
        }
        return;
    }

    /* Cell geometry is static — only re-walk the arrays when Harry's collision
     * cell changes. The overlay re-projects the cached geometry every frame. */
    if (cd == s_lastCell)
    {
        return;
    }
    s_lastCell = cd;
    CollVis_ClearCell();

    count = cd->subcellCount; /* subcell (wall-segment) array size */
    if (count <= 0 || count > 2048)
    {
        return;
    }

    ox = cd->positionX;
    oz = cd->positionZ;
    for (i = 0; i < count; i++)
    {
        s_IpdCollSubcell* f  = &cd->subcells[i];
        SVECTOR3*         va = &cd->splitVertices[f->splitVertexIdx0];
        SVECTOR3*         vb = &cd->splitVertices[f->splitVertexIdx1];
        CollVis_CaptureSeg(
            (ox + va->vx) << 4, (s32)va->vy << 4, (oz + va->vz) << 4,
            (ox + vb->vx) << 4, (s32)vb->vy << 4, (oz + vb->vz) << 4,
            0);
    }

    /* ptr_18 cylinder colliders (trees/poles): center offset + radius field_8.
     * Count from the array layout — ptr_18 immediately precedes subcellRanges. */
    {
        extern void CollVis_CaptureCylinder(s32 cx, s32 cy, s32 cz, s32 r);
        s32 cylCount = (s32)(((char*)cd->subcellRanges - (char*)cd->ptr_18) /
                             (s32)sizeof(s_IpdCollisionData_18));
        if (cylCount > 0 && cylCount <= 256)
        {
            for (i = 0; i < cylCount; i++)
            {
                s_IpdCollisionData_18* c  = &cd->ptr_18[i];
                s32                    rr = (s32)c->field_8 << 4;
                if (rr <= 0 || rr > (64 << 12)) /* sane radius only */
                {
                    continue;
                }
                CollVis_CaptureCylinder(
                    (ox + c->offset.vx) << 4, (s32)c->offset.vy << 4, (oz + c->offset.vz) << 4,
                    rr);
            }
        }
    }
}
#endif
